#include "ColorGrading.h"

#include <cmath>
#include <mutex>

namespace playback::editor::graphics {

namespace {

std::mutex                  gGradingMutex;
std::optional<ColorGrading> gGrading;

// How far the extremes of the sliders push each channel before brightness is restored.
constexpr float TemperatureStrength = 0.25f;
constexpr float TintStrength        = 0.2f;

} // namespace

ColorGain colorGain(ColorGrading const& grading) {
    float const red   = 1.0f + TemperatureStrength * grading.temperature;
    float const blue  = 1.0f - TemperatureStrength * grading.temperature;
    float const green = 1.0f - TintStrength * grading.tint;
    // Rec. 709 luminance of the white-balance gains, so a temperature or tint change keeps the brightness.
    float const luminance = 0.2126f * red + 0.7152f * green + 0.0722f * blue;
    float const scale     = std::exp2(grading.exposure) / luminance;
    return {red * scale, green * scale, blue * scale};
}

void setColorGrading(std::optional<ColorGrading> grading) {
    std::scoped_lock lock(gGradingMutex);
    gGrading = grading;
}

std::optional<ColorGain> currentColorGain() {
    std::optional<ColorGrading> grading;
    {
        std::scoped_lock lock(gGradingMutex);
        grading = gGrading;
    }
    if (!grading || (grading->exposure == 0.0f && grading->temperature == 0.0f && grading->tint == 0.0f)) {
        return std::nullopt;
    }
    return colorGain(*grading);
}

} // namespace playback::editor::graphics
