#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

namespace playback::state::editing::model {

// How far a keyframe track's camera shakes at strength 1. Each key sets the strength and frequency; these scale it.
struct ShakeSettings {
    static constexpr float MaxPositionAmount = 4.0f;
    static constexpr float MaxRotationAmount = 30.0f;
    // A key's shake strength and frequency (Hz) stay within these.
    static constexpr float MaxStrength      = 5.0f;
    static constexpr float MinFrequency     = 0.1f;
    static constexpr float MaxFrequency     = 20.0f;
    static constexpr float DefaultFrequency = 2.0f;

    // Blocks the camera wanders on each axis.
    float positionAmount{0.15f};
    // Degrees the view turns in yaw and pitch.
    float rotationAmount{1.5f};
    // Degrees the view tilts.
    float rollAmount{1.0f};
    // Picks the noise, so two tracks with the same keys still shake differently; the same seed always shakes the same.
    uint32_t seed{};

    [[nodiscard]] ShakeSettings sanitized() const {
        auto const range = [](float value, float high, float fallback) {
            return std::isfinite(value) ? std::clamp(value, 0.0f, high) : fallback;
        };
        ShakeSettings const defaults;
        ShakeSettings       out = *this;
        out.positionAmount      = range(positionAmount, MaxPositionAmount, defaults.positionAmount);
        out.rotationAmount      = range(rotationAmount, MaxRotationAmount, defaults.rotationAmount);
        out.rollAmount          = range(rollAmount, MaxRotationAmount, defaults.rollAmount);
        return out;
    }

    bool operator==(ShakeSettings const&) const = default;
};

[[nodiscard]] inline float sanitizedShakeStrength(float strength) {
    return std::isfinite(strength) ? std::clamp(strength, 0.0f, ShakeSettings::MaxStrength) : 0.0f;
}

[[nodiscard]] inline float sanitizedShakeFrequency(float frequency) {
    return std::isfinite(frequency) ? std::clamp(frequency, ShakeSettings::MinFrequency, ShakeSettings::MaxFrequency)
                                    : ShakeSettings::DefaultFrequency;
}

// New tracks and the inspector's New seed button draw one; nothing else is random, so preview and export agree.
[[nodiscard]] inline uint32_t randomShakeSeed() { return std::random_device{}(); }

} // namespace playback::state::editing::model
