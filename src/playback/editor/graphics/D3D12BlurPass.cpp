#include "D3D12BlurPass.h"

#include "playback/Playback.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <optional>

namespace playback::editor::graphics {
namespace {

using Microsoft::WRL::ComPtr;
using state::editing::model::BlurArea;
using state::editing::model::BlurStyle;

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

constexpr uint32_t ThreadGroupSize = 8;
// Dispatches one apply() may record: a Gaussian takes two, every other style one.
constexpr uint32_t MaxDispatchesPerApply = 32;
// Descriptor sets handed out in turn; enough for every dispatch of more frames than can be in flight, so none is
// rewritten in use.
constexpr uint32_t DescriptorSets = 8 * MaxDispatchesPerApply;
// Each set: the original (t0), the image to blur (t1) and the destination (u0).
constexpr uint32_t DescriptorsPerSet = 3;
// The separable Gaussian's intermediate keeps linear light without banding.
constexpr DXGI_FORMAT IntermediateFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Frames are sRGB-encoded, so every style averages in linear light. The frame is the largest centred rectangle of the
// project's aspect inside the texture; lengths arrive in pixels already scaled to its height. The Gaussian runs
// across into the intermediate (all of it: the pass down reads above and below each pixel), then down into the frame.
// Long blurs stride their taps and let the bilinear sampler fill in between, so the cost stays bounded.
constexpr char const* BlurSource = R"(
Texture2D<float4> Original : register(t0);
Texture2D<float4> Source : register(t1);
RWTexture2D<float4> Destination : register(u0);
SamplerState Clamp : register(s0);

cbuffer Params : register(b0) {
    uint2 Size;
    uint Mode;
    uint Area;
    float2 FrameOrigin;
    float2 FrameSize;
    float2 Centre;
    float2 HalfExtent;
    float2 Rotation;
    float Feather;
    float Mix;
    float2 Direction;
    float Amount;
    uint Invert;
};

static const uint GaussianAcross = 0;
static const uint GaussianDown = 1;
static const uint Mosaic = 2;
static const uint Motion = 3;
static const uint Radial = 4;
static const uint MaxTapsPerSide = 32;
static const uint StreakSamples = 32;

float3 toLinear(float3 colour) { return pow(max(colour, 0.0f), 2.2f); }
float3 toEncoded(float3 colour) { return pow(saturate(colour), 1.0f / 2.2f); }

float3 sampleOriginal(float2 position) {
    return toLinear(Original.SampleLevel(Clamp, position / float2(Size), 0).rgb);
}

// How much of the blur shows at a pixel: the area's shape, its feathered edge, inverted if asked, times the mix.
float areaWeight(float2 position) {
    float inside = 1.0f;
    if (Area != 0) {
        float2 offset = position - Centre;
        float2 local = float2(
            offset.x * Rotation.x + offset.y * Rotation.y,
            -offset.x * Rotation.y + offset.y * Rotation.x
        );
        // Signed distance to the edge in pixels, negative inside.
        float edge = Area == 1
            ? max(abs(local.x) - HalfExtent.x, abs(local.y) - HalfExtent.y)
            : (length(local / HalfExtent) - 1.0f) * min(HalfExtent.x, HalfExtent.y);
        inside = Feather > 0.0f
            ? 1.0f - smoothstep(-Feather * 0.5f, Feather * 0.5f, edge)
            : (edge <= 0.0f ? 1.0f : 0.0f);
    }
    if (Invert != 0) inside = 1.0f - inside;
    return inside * Mix;
}

// Amount is the radius (3 sigma) in pixels.
float3 gaussian(Texture2D<float4> image, bool encoded, float2 position, float2 axis) {
    uint steps = min((uint)ceil(Amount), MaxTapsPerSide);
    float stride = Amount / (float)steps;
    float sigma = Amount / 3.0f;
    float3 total = 0.0f;
    float weights = 0.0f;
    for (int i = -(int)steps; i <= (int)steps; ++i) {
        float offset = (float)i * stride;
        float weight = exp(-offset * offset / (2.0f * sigma * sigma));
        float3 colour = image.SampleLevel(Clamp, (position + axis * offset) / float2(Size), 0).rgb;
        total += (encoded ? toLinear(colour) : colour) * weight;
        weights += weight;
    }
    return total / weights;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Size.x || id.y >= Size.y) return;
    float2 position = float2(id.xy) + 0.5f;
    if (Mode == GaussianAcross) {
        Destination[id.xy] = float4(gaussian(Original, true, position, float2(1.0f, 0.0f)), 1.0f);
        return;
    }

    float4 original = Original.Load(int3(id.xy, 0));
    float weight = areaWeight(position);
    if (weight <= 0.0f) {
        Destination[id.xy] = float4(original.rgb, 1.0f);
        return;
    }

    float3 blurred = 0.0f;
    if (Mode == GaussianDown) {
        blurred = gaussian(Source, false, position, float2(0.0f, 1.0f));
    } else if (Mode == Mosaic) {
        // Blocks start at the frame's corner so they sit the same in preview and export.
        float2 corner = FrameOrigin + floor((position - FrameOrigin) / Amount) * Amount;
        for (uint y = 0; y < 4; ++y) {
            for (uint x = 0; x < 4; ++x) {
                blurred += sampleOriginal(corner + (float2(x, y) + 0.5f) * (Amount / 4.0f));
            }
        }
        blurred /= 16.0f;
    } else if (Mode == Motion) {
        for (uint i = 0; i < StreakSamples; ++i) {
            float along = (float)i / (float)(StreakSamples - 1) - 0.5f;
            blurred += sampleOriginal(position + Direction * (Amount * along));
        }
        blurred /= (float)StreakSamples;
    } else {
        for (uint i = 0; i < StreakSamples; ++i) {
            float towards = (float)i / (float)(StreakSamples - 1) * Amount;
            blurred += sampleOriginal(position + (Centre - position) * towards);
        }
        blurred /= (float)StreakSamples;
    }
    Destination[id.xy] = float4(toEncoded(lerp(toLinear(original.rgb), blurred, weight)), 1.0f);
}
)";

enum class Mode : uint32_t { GaussianAcross, GaussianDown, Mosaic, Motion, Radial };

struct Params {
    uint32_t width{};
    uint32_t height{};
    uint32_t mode{};
    uint32_t area{};
    float    frameOrigin[2]{};
    float    frameSize[2]{};
    float    centre[2]{};
    float    halfExtent[2]{};
    float    rotation[2]{1.0f, 0.0f};
    float    feather{};
    float    mix{};
    float    direction[2]{1.0f, 0.0f};
    float    amount{};
    uint32_t invert{};
};
static_assert(sizeof(Params) == 20 * sizeof(uint32_t));

// The pass's parameters in the texture's pixels, or nullopt when it would not change the picture.
std::optional<Params> paramsFor(BlurPass const& pass, uint32_t width, uint32_t height, float frameAspect) {
    if (!(pass.mix > 0.0f)) return std::nullopt;
    auto const w = static_cast<float>(width);
    auto const h = static_cast<float>(height);
    float      frameWidth{w};
    float      frameHeight{h};
    if (w / h > frameAspect) frameWidth = h * frameAspect;
    else frameHeight = w / frameAspect;
    float const frameX = (w - frameWidth) * 0.5f;
    float const frameY = (h - frameHeight) * 0.5f;

    Params params{};
    params.width          = width;
    params.height         = height;
    params.area           = static_cast<uint32_t>(pass.area);
    params.frameOrigin[0] = frameX;
    params.frameOrigin[1] = frameY;
    params.frameSize[0]   = frameWidth;
    params.frameSize[1]   = frameHeight;
    params.centre[0]      = frameX + pass.centreX * frameWidth;
    params.centre[1]      = frameY + pass.centreY * frameHeight;
    // Half a pixel at least, so a collapsed area is a sliver rather than a division by zero.
    params.halfExtent[0]  = std::max(pass.width * frameHeight * 0.5f, 0.5f);
    params.halfExtent[1]  = std::max(pass.height * frameHeight * 0.5f, 0.5f);
    float const turn      = pass.rotation * std::numbers::pi_v<float> / 180.0f;
    params.rotation[0]    = std::cos(turn);
    params.rotation[1]    = std::sin(turn);
    params.feather        = std::max(pass.feather, 0.0f) * frameHeight;
    params.mix            = std::min(pass.mix, 1.0f);
    params.invert         = pass.invert ? 1u : 0u;

    switch (pass.style) {
    case BlurStyle::Gaussian:
        params.mode   = static_cast<uint32_t>(Mode::GaussianDown);
        params.amount = pass.amount * frameHeight;
        if (params.amount < 0.5f) return std::nullopt;
        break;
    case BlurStyle::Mosaic:
        params.mode   = static_cast<uint32_t>(Mode::Mosaic);
        params.amount = std::max(1.0f, pass.amount * frameHeight);
        if (params.amount <= 1.0f) return std::nullopt;
        break;
    case BlurStyle::Motion: {
        params.mode          = static_cast<uint32_t>(Mode::Motion);
        params.amount        = pass.amount * frameHeight;
        float const angle    = pass.angle * std::numbers::pi_v<float> / 180.0f;
        params.direction[0]  = std::cos(angle);
        params.direction[1]  = std::sin(angle);
        if (params.amount < 1.0f) return std::nullopt;
        break;
    }
    case BlurStyle::Radial:
        params.mode   = static_cast<uint32_t>(Mode::Radial);
        params.amount = std::clamp(pass.amount, 0.0f, 1.0f);
        if (params.amount <= 0.0f) return std::nullopt;
        break;
    default:
        return std::nullopt;
    }
    return params;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter  = after;
    return barrier;
}

} // namespace

struct D3D12BlurPass::Impl {
    ComPtr<ID3D12Device>         device;
    ComPtr<ID3D12RootSignature>  rootSignature;
    ComPtr<ID3D12PipelineState>  pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    uint32_t                     descriptorSize{};
    uint32_t                     nextSet{};
    bool                         failed{};
    bool                         loggedUnsupported{};
    bool                         loggedTooMany{};
    // The last format supports() checked, and the answer.
    DXGI_FORMAT checkedFormat{DXGI_FORMAT_UNKNOWN};
    bool        formatSupported{};
    // A copy of the frame as each pass found it (rests in COPY_DEST), and the Gaussian's intermediate (rests in
    // UNORDERED_ACCESS). Both match the last texture applied to.
    ComPtr<ID3D12Resource> original;
    ComPtr<ID3D12Resource> intermediate;

    bool build(ID3D12Device* target) {
        if (failed) return false;
        if (pipeline && device.Get() == target) return true;

        rootSignature.Reset();
        pipeline.Reset();
        descriptorHeap.Reset();
        original.Reset();
        intermediate.Reset();
        checkedFormat = DXGI_FORMAT_UNKNOWN;
        device        = target;

        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> errors;
        HRESULT const    compileResult = D3DCompile(
            BlurSource,
            std::strlen(BlurSource),
            nullptr,
            nullptr,
            nullptr,
            "main",
            "cs_5_0",
            0,
            0,
            &shader,
            &errors
        );
        if (FAILED(compileResult)) {
            getLogger().error(
                "Blur shader compilation failed (hr=0x{:08X}): {}",
                static_cast<uint32_t>(compileResult),
                errors ? static_cast<char const*>(errors->GetBufferPointer()) : "no diagnostics"
            );
            failed = true;
            return false;
        }

        std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
        ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors                    = 2;
        ranges[0].BaseShaderRegister                = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors                    = 1;
        ranges[1].BaseShaderRegister                = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 2;

        std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
        parameters[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(ranges.size());
        parameters[0].DescriptorTable.pDescriptorRanges   = ranges.data();
        parameters[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants.ShaderRegister            = 0;
        parameters[1].Constants.Num32BitValues            = sizeof(Params) / sizeof(uint32_t);
        parameters[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD           = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister   = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters     = static_cast<UINT>(parameters.size());
        rootDesc.pParameters       = parameters.data();
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers   = &sampler;

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> rootErrors;
        HRESULT const    serializeResult =
            D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &rootErrors);
        if (FAILED(serializeResult)) {
            getLogger().error(
                "Blur root signature serialization failed (hr=0x{:08X})",
                static_cast<uint32_t>(serializeResult)
            );
            failed = true;
            return false;
        }
        HRESULT const rootResult = device->CreateRootSignature(
            0,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)
        );
        if (FAILED(rootResult)) {
            getLogger().error("Blur root signature creation failed (hr=0x{:08X})", static_cast<uint32_t>(rootResult));
            failed = true;
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
        pipelineDesc.pRootSignature     = rootSignature.Get();
        pipelineDesc.CS.pShaderBytecode = shader->GetBufferPointer();
        pipelineDesc.CS.BytecodeLength  = shader->GetBufferSize();
        HRESULT const pipelineResult    = device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline));
        if (FAILED(pipelineResult)) {
            getLogger().error("Blur pipeline creation failed (hr=0x{:08X})", static_cast<uint32_t>(pipelineResult));
            failed = true;
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type            = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors  = DescriptorsPerSet * DescriptorSets;
        heapDesc.Flags           = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT const heapResult = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
        if (FAILED(heapResult)) {
            getLogger().error("Blur descriptor heap creation failed (hr=0x{:08X})", static_cast<uint32_t>(heapResult));
            failed = true;
            return false;
        }
        descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    bool createScratch(
        D3D12_RESOURCE_DESC const& desc,
        D3D12_RESOURCE_STATES      initialState,
        char const*                what,
        ComPtr<ID3D12Resource>&    resource
    ) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type             = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask  = 1;
        HRESULT const createResult = device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&resource)
        );
        if (FAILED(createResult)) {
            getLogger().error(
                "Blur {} texture allocation failed (hr=0x{:08X}, size={}x{})",
                what,
                static_cast<uint32_t>(createResult),
                desc.Width,
                desc.Height
            );
            return false;
        }
        return true;
    }

    // Matches the scratches to the texture. Replacing them while an older frame still reads them is safe only
    // because the textures handed in change size or format only when their owner recreates them, after waiting for
    // the GPU (a swap chain resize, a new export).
    bool prepareScratches(D3D12_RESOURCE_DESC const& textureDesc) {
        if (original && intermediate) {
            auto const existing = original->GetDesc();
            if (existing.Width == textureDesc.Width && existing.Height == textureDesc.Height
                && existing.Format == textureDesc.Format && existing.MipLevels == textureDesc.MipLevels)
                return true;
        }
        original.Reset();
        intermediate.Reset();

        // CopyResource needs the same format, size and mips; only shaders read the copy.
        auto copyDesc      = textureDesc;
        copyDesc.Alignment = 0;
        copyDesc.Flags     = D3D12_RESOURCE_FLAG_NONE;
        if (!createScratch(copyDesc, D3D12_RESOURCE_STATE_COPY_DEST, "copy", original)) return false;

        D3D12_RESOURCE_DESC intermediateDesc{};
        intermediateDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        intermediateDesc.Width            = textureDesc.Width;
        intermediateDesc.Height           = textureDesc.Height;
        intermediateDesc.DepthOrArraySize = 1;
        intermediateDesc.MipLevels        = 1;
        intermediateDesc.Format           = IntermediateFormat;
        intermediateDesc.SampleDesc.Count = 1;
        intermediateDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!createScratch(intermediateDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "intermediate", intermediate)) {
            original.Reset();
            return false;
        }
        return true;
    }

    // Binds the frame's copy (t0), `source` (t1) and `destination` (u0) in the next descriptor set and runs the
    // shader over the texture.
    void
    run(ID3D12GraphicsCommandList* commandList,
        ID3D12Resource*            source,
        ID3D12Resource*            destination,
        Params const&              params) {
        uint32_t const set = nextSet;
        nextSet            = (nextSet + 1) % DescriptorSets;

        auto cpu  = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        auto gpu  = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr  += static_cast<SIZE_T>(DescriptorsPerSet * set) * descriptorSize;
        gpu.ptr  += static_cast<UINT64>(DescriptorsPerSet * set) * descriptorSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels     = 1;
        srv.Format                  = original->GetDesc().Format;
        device->CreateShaderResourceView(original.Get(), &srv, cpu);
        srv.Format = source->GetDesc().Format;
        device->CreateShaderResourceView(source, &srv, {cpu.ptr + descriptorSize});

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format        = destination->GetDesc().Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(destination, nullptr, &uav, {cpu.ptr + 2 * descriptorSize});

        ID3D12DescriptorHeap* heaps[]{descriptorHeap.Get()};
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetComputeRootSignature(rootSignature.Get());
        commandList->SetPipelineState(pipeline.Get());
        commandList->SetComputeRootDescriptorTable(0, gpu);
        commandList->SetComputeRoot32BitConstants(1, sizeof(Params) / sizeof(uint32_t), &params, 0);
        commandList->Dispatch(
            (params.width + ThreadGroupSize - 1) / ThreadGroupSize,
            (params.height + ThreadGroupSize - 1) / ThreadGroupSize,
            1
        );
    }
};

D3D12BlurPass::D3D12BlurPass() : mImpl(std::make_unique<Impl>()) {}

D3D12BlurPass::~D3D12BlurPass() = default;

bool D3D12BlurPass::ready(ID3D12Device* device) { return device && mImpl->build(device); }

bool D3D12BlurPass::supports(ID3D12Resource* texture) {
    if (!texture || !mImpl->pipeline) return false;
    auto const desc = texture->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1
        || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
        return false;
    if (desc.Format != mImpl->checkedFormat) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{desc.Format};
        mImpl->checkedFormat = desc.Format;
        mImpl->formatSupported =
            SUCCEEDED(mImpl->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))
            && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    }
    if (!mImpl->formatSupported && !mImpl->loggedUnsupported) {
        mImpl->loggedUnsupported = true;
        getLogger().warn(
            "This GPU cannot write frames of format {} from a shader; blur layers do not show",
            static_cast<uint32_t>(desc.Format)
        );
    }
    return mImpl->formatSupported;
}

bool D3D12BlurPass::apply(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource*            texture,
    D3D12_RESOURCE_STATES      state,
    std::span<BlurPass const>  passes,
    float                      frameAspect
) {
    if (!commandList || !texture || !(frameAspect > 0.0f) || !mImpl->pipeline) return false;
    auto const desc   = texture->GetDesc();
    auto const width  = static_cast<uint32_t>(desc.Width);
    auto const height = desc.Height;
    if (width == 0 || height == 0) return false;

    // At most one Params per dispatch, so the budget bounds the list.
    std::array<Params, MaxDispatchesPerApply> work{};
    uint32_t                                  count      = 0;
    uint32_t                                  dispatches = 0;
    for (auto const& pass : passes) {
        auto params = paramsFor(pass, width, height, frameAspect);
        if (!params) continue;
        uint32_t const cost = params->mode == static_cast<uint32_t>(Mode::GaussianDown) ? 2 : 1;
        if (dispatches + cost > MaxDispatchesPerApply) {
            if (!mImpl->loggedTooMany) {
                mImpl->loggedTooMany = true;
                getLogger().warn("More blur layers overlap than the GPU pass takes; the topmost ones are left out");
            }
            break;
        }
        dispatches += cost;
        work[count++] = *params;
    }
    // Every pass is too faint to change the picture.
    if (count == 0) return true;
    if (!mImpl->prepareScratches(desc)) return false;

    ID3D12Resource* const original     = mImpl->original.Get();
    ID3D12Resource* const intermediate = mImpl->intermediate.Get();
    D3D12_RESOURCE_STATES current      = state;
    for (auto const& params : std::span(work).first(count)) {
        // Snapshot the frame as this pass finds it: earlier passes have already written it.
        if (current != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            auto const toCopy = transition(texture, current, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commandList->ResourceBarrier(1, &toCopy);
        }
        commandList->CopyResource(original, texture);
        std::array toBlur{
            transition(original, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        commandList->ResourceBarrier(static_cast<UINT>(toBlur.size()), toBlur.data());
        current = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        if (params.mode == static_cast<uint32_t>(Mode::GaussianDown)) {
            auto across = params;
            across.mode = static_cast<uint32_t>(Mode::GaussianAcross);
            mImpl->run(commandList, original, intermediate, across);
            auto const toRead = transition(
                intermediate,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            );
            commandList->ResourceBarrier(1, &toRead);
            mImpl->run(commandList, intermediate, texture, params);
            auto const toWrite = transition(
                intermediate,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            );
            commandList->ResourceBarrier(1, &toWrite);
        } else {
            mImpl->run(commandList, original, texture, params);
        }

        auto const toRest =
            transition(original, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1, &toRest);
    }

    if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        // The caller's next shader access must see the last dispatch's writes.
        D3D12_RESOURCE_BARRIER written{};
        written.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        written.UAV.pResource = texture;
        commandList->ResourceBarrier(1, &written);
    } else {
        auto const back = transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, state);
        commandList->ResourceBarrier(1, &back);
    }
    return true;
}

} // namespace playback::editor::graphics
