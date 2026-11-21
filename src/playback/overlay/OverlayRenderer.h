#pragma once

#include "playback/state/editing/models/OverlayItem.h"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace playback::overlay {

// Premultiplied RGBA8, tightly packed (row pitch = width * 4).
struct OverlayImage {
    uint32_t             width{};
    uint32_t             height{};
    std::vector<uint8_t> rgba;

    void reset(uint32_t w, uint32_t h) {
        width  = w;
        height = h;
        rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    }
};

// The camera the picture under the overlay was rendered with, for layers placed in the world (models).
struct OverlayCamera {
    // World to clip space; NDC y points up.
    ::glm::dmat4 viewProjection{1.0};
    // Points with clip w below this are behind the near plane.
    double nearClipW{0.001};
    // Where NDC (-1, 1) and (1, -1) land in the overlay image, in pixels: the camera's picture may be wider than the
    // frame (the preview crops the game window to the export's aspect).
    float ndcLeft{};
    float ndcTop{};
    float ndcRight{};
    float ndcBottom{};
};

// Renders every item active at `tick` (fractional replay ticks) into `out`, resized to width x height and cleared
// first, in vector order (later items on top). Model items need `camera`; without one they are skipped.
// Thread-safe: the preview (UI thread) and the export (writer thread) render concurrently.
// Returns false when nothing was drawn (out is then fully transparent).
bool renderOverlays(
    std::span<state::editing::model::OverlayItem const> items,
    double                                              tick,
    uint32_t                                            width,
    uint32_t                                            height,
    std::optional<OverlayCamera> const&                 camera,
    OverlayImage&                                       out
);

// True when any item would draw at `tick`.
[[nodiscard]] bool hasActiveOverlays(std::span<state::editing::model::OverlayItem const> items, double tick);

// Source-over blend of `overlay` (premultiplied, same size) onto an opaque 8-bit frame.
void compositeOver(uint8_t* pixels, uint32_t width, uint32_t height, uint32_t rowPitch, bool bgra, OverlayImage const& overlay);

// Installed font family names, sorted, for the title font picker. Cached after the first call.
[[nodiscard]] std::vector<std::string> const& systemFontFamilies();

} // namespace playback::overlay
