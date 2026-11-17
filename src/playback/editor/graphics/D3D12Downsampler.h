#pragma once

#include "playback/editor/graphics/ColorGrading.h"

#include <cstdint>
#include <memory>
#include <optional>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace playback::editor::graphics {

// GPU passes over a captured frame: averages each factor x factor block so the readback only carries output-sized
// pixels, and applies a colour grade. Each call uses the next of a few descriptor sets, so a handful of frames can be
// in flight at once.
class D3D12Downsampler {
public:
    D3D12Downsampler();
    ~D3D12Downsampler();

    D3D12Downsampler(D3D12Downsampler const&)            = delete;
    D3D12Downsampler& operator=(D3D12Downsampler const&) = delete;

    [[nodiscard]] bool ready(ID3D12Device* device);

    // Source must be in PIXEL_SHADER_RESOURCE, destination in UNORDERED_ACCESS; both are left that way. A factor of 1
    // copies, which with a gain grades the frame.
    bool dispatch(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource*            source,
        ID3D12Resource*            destination,
        uint32_t                   destinationWidth,
        uint32_t                   destinationHeight,
        uint32_t                   factor,
        std::optional<ColorGain>   gain
    );

    // Whether grade() works on textures of this format: it reads and writes the same texture.
    [[nodiscard]] bool canGradeInPlace(ID3D12Resource* texture);

    // Grades the texture where it is; it must be in UNORDERED_ACCESS and is left that way.
    bool grade(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource*            texture,
        uint32_t                   width,
        uint32_t                   height,
        ColorGain                  gain
    );

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace playback::editor::graphics
