#pragma once

#include <cmath>
#include <optional>

namespace playback::editor::graphics {

// A rectangle of the game window, in pixels.
struct ViewportRect {
    float x{};
    float y{};
    float width{};
    float height{};

    bool operator==(ViewportRect const&) const = default;
};

// The centred band of `window` that shows a frame of `aspect` (width / height) across the window's full width, in
// whole pixels. Bedrock's field of view is vertical, so the band sees what an export of that shape sees. Nullopt when
// the frame is not wider than the window: the window's own view then covers it, and the viewport crops it.
inline std::optional<ViewportRect> letterboxViewport(ViewportRect const& window, float aspect) {
    if (!(aspect > 0.0f) || !(window.width >= 1.0f) || !(window.height >= 1.0f)) return std::nullopt;
    float const height = std::round(window.width / aspect);
    if (!(height >= 1.0f) || height >= window.height) return std::nullopt;
    float const top = std::floor((window.height - height) * 0.5f);
    return ViewportRect{window.x, window.y + top, window.width, height};
}

} // namespace playback::editor::graphics
