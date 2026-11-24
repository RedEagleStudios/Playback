#include "playback/exporting/FrameWriterUtils.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace detail  = playback::exporting::detail;
namespace visuals = playback::visuals;

namespace {

// A 2x2 frame whose rows carry 4 bytes of padding, with the alpha a Vibrant Visuals swap chain holds (0).
visuals::CapturedFrame paddedFrame(visuals::FramePixelFormat format) {
    visuals::CapturedFrame frame;
    frame.width       = 2;
    frame.height      = 2;
    frame.rowPitch    = 12;
    frame.pixelFormat = format;
    for (uint8_t byte : {10, 20, 30, 0, 40, 50, 60, 0, 99, 99, 99, 99, 70, 80, 90, 0, 1, 2, 3, 0, 99, 99, 99, 99}) {
        frame.pixels.push_back(static_cast<std::byte>(byte));
    }
    return frame;
}

} // namespace

TEST_CASE("written frames: an RGBA frame keeps its colour and becomes opaque") {
    std::vector<uint8_t> rgba;
    detail::copyPackedOpaqueRgba(paddedFrame(visuals::FramePixelFormat::Rgba8), rgba);
    CHECK(rgba == std::vector<uint8_t>{10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 1, 2, 3, 255});
}

TEST_CASE("written frames: a BGRA frame is swizzled to RGBA and becomes opaque") {
    std::vector<uint8_t> rgba;
    detail::copyPackedOpaqueRgba(paddedFrame(visuals::FramePixelFormat::Bgra8), rgba);
    CHECK(rgba == std::vector<uint8_t>{30, 20, 10, 255, 60, 50, 40, 255, 90, 80, 70, 255, 3, 2, 1, 255});
}
