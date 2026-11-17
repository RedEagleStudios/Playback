#pragma once

#include <optional>

namespace playback::editor::graphics {

// A project's replacement for the world's distance fog: where it starts and ends as fractions of the render distance.
// Vanilla fog is about 0.92 to 1.
struct FogOverride {
    float start{};
    float end{};
};

// Any thread; the renderer picks it up on its next frame. Nullopt shows the world's own fog.
void setFogOverride(std::optional<FogOverride> fog);

// Installed and removed with the camera render hooks.
[[nodiscard]] bool hookFogOverride(bool enable);

} // namespace playback::editor::graphics
