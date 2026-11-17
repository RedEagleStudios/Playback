#pragma once

#include <optional>

namespace playback::editor::graphics {

// A project's colour grade, applied to the replay preview and to exported frames.
struct ColorGrading {
    // Stops: +1 doubles the light, -1 halves it.
    float exposure{};
    // -1 cool (blue) to +1 warm (orange); 0 leaves the white balance alone.
    float temperature{};
    // -1 green to +1 magenta.
    float tint{};
};

// Per-channel multipliers in linear light that apply a grade.
struct ColorGain {
    float r{1.0f};
    float g{1.0f};
    float b{1.0f};
};

// Exposure scales all channels; temperature and tint shift the white balance at constant brightness.
[[nodiscard]] ColorGain colorGain(ColorGrading const& grading);

// Any thread; preview and export pick it up on their next frame. Nullopt shows the game's own colours.
void setColorGrading(std::optional<ColorGrading> grading);

// The gain to apply now, or nullopt when there is no grade or it changes nothing.
[[nodiscard]] std::optional<ColorGain> currentColorGain();

} // namespace playback::editor::graphics
