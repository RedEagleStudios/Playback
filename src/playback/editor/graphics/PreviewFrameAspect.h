#pragma once

#include <optional>

namespace playback::editor::graphics {

// The part of the game window the last preview frame drew into, for a frame wider than the window.
struct PreviewFrameBand {
    // The frame's aspect (width / height) the band was drawn for.
    float aspect{};
    // The band's top and bottom edges as fractions of the window's height.
    float top{};
    float bottom{};
};

// The editor viewport's frame aspect while the frame is wider than the game window, nullopt otherwise or while the
// viewport is not shown. Set every UI frame. Any thread.
void requestPreviewFrameAspect(std::optional<float> aspect);

// The band the game drew the last frame into; nullopt while it draws the whole window. Any thread.
[[nodiscard]] std::optional<PreviewFrameBand> previewFrameBand();

// Before each frame renders: while a replay is open and a wide frame is requested, the game renders into a centred
// band of the window at the frame's aspect (letterboxViewport); otherwise the window's own viewport is back. Client
// thread.
void applyPreviewFrameAspect(bool replayActive);

// Puts the window's own viewport back at once, before an export sizes the render to its frame. Client thread.
void restorePreviewFrameAspect();

} // namespace playback::editor::graphics
