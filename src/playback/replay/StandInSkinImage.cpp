#include "StandInSkinImage.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>

namespace playback::replay {

namespace {

using Microsoft::WRL::ComPtr;

class ComScope {
public:
    ComScope() : mNeedsUninit(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() {
        if (mNeedsUninit) CoUninitialize();
    }
    ComScope(ComScope const&)            = delete;
    ComScope& operator=(ComScope const&) = delete;

private:
    bool mNeedsUninit;
};

bool decodePng(std::filesystem::path const& file, StandInSkinImage& out) {
    ComScope                      com;
    ComPtr<IWICImagingFactory>    factory;
    ComPtr<IWICBitmapDecoder>     decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter>   converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
        || FAILED(factory->CreateDecoderFromFilename(
            file.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            &decoder
        ))
        || FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        ))) {
        return false;
    }
    UINT width{};
    UINT height{};
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 || width > 1024 || height > 1024) {
        return false;
    }
    out.width  = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    return SUCCEEDED(
        converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(out.rgba.size()), out.rgba.data())
    );
}

// The 64x32 layout has one arm and one leg; the 64x64 one draws the left limbs from their own lower half. Java
// Edition's conversion: each face of the right limb, mirrored into the left limb's place.
void widenLegacySkin(StandInSkinImage& image) {
    StandInSkinImage wide{64, 64, std::vector<uint8_t>(64 * 64 * 4, 0)};
    std::copy(image.rgba.begin(), image.rgba.end(), wide.rgba.begin());
    auto const copyRect = [&](int x, int y, int dx, int dy, int w, int h) {
        for (int row = 0; row < h; ++row) {
            for (int column = 0; column < w; ++column) {
                size_t const from = (static_cast<size_t>(y + row) * 64 + static_cast<size_t>(x + column)) * 4;
                size_t const to   = (static_cast<size_t>(y + dy + row) * 64 + static_cast<size_t>(x + dx + w - 1 - column)) * 4;
                std::copy_n(wide.rgba.begin() + static_cast<std::ptrdiff_t>(from), 4, wide.rgba.begin() + static_cast<std::ptrdiff_t>(to));
            }
        }
    };
    copyRect(4, 16, 16, 32, 4, 4);
    copyRect(8, 16, 16, 32, 4, 4);
    copyRect(0, 20, 24, 32, 4, 12);
    copyRect(4, 20, 16, 32, 4, 12);
    copyRect(8, 20, 8, 32, 4, 12);
    copyRect(12, 20, 16, 32, 4, 12);
    copyRect(44, 16, -8, 32, 4, 4);
    copyRect(48, 16, -8, 32, 4, 4);
    copyRect(40, 20, 0, 32, 4, 12);
    copyRect(44, 20, -8, 32, 4, 12);
    copyRect(48, 20, -16, 32, 4, 12);
    copyRect(52, 20, -8, 32, 4, 12);
    image = std::move(wide);
}

} // namespace

bool loadStandInSkinImage(std::filesystem::path const& file, StandInSkinImage& out, std::string& error) {
    StandInSkinImage image;
    if (!decodePng(file, image)) {
        error = std::format("The skin image {} could not be read.", file.filename().string());
        return false;
    }
    if (image.width == 64 && image.height == 32) {
        widenLegacySkin(image);
    } else if (!(image.width == 64 && image.height == 64) && !(image.width == 128 && image.height == 128)) {
        error = std::format(
            "The skin image {} is {}x{}; a skin is 64x64, 64x32 or 128x128.",
            file.filename().string(),
            image.width,
            image.height
        );
        return false;
    }
    out = std::move(image);
    return true;
}

} // namespace playback::replay
