#pragma once

#include <array>
#include <cmath>
#include <numbers>
#include <optional>

namespace playback::state::editing::model {

// The lenses the camera inspector offers, as focal lengths in millimetres on a full-frame (36 x 24 mm) camera.
inline constexpr std::array<int, 6> LensFocalLengths{15, 35, 50, 75, 100, 200};

// The vertical field of view, in degrees, a full-frame lens of `focalLength` mm gives a frame `aspect` (width / height)
// wide. The sensor's short side lies along the frame's short side, as a camera turned for a tall frame: a landscape
// frame is 24 mm high, a 9:16 one is 24 mm wide and so 24 / aspect mm high.
[[nodiscard]] inline float lensFov(float focalLength, float aspect) {
    constexpr float ShortSide = 24.0f;
    if (!(aspect > 0.0f) || !std::isfinite(aspect)) aspect = 16.0f / 9.0f;
    float const height = aspect >= 1.0f ? ShortSide : ShortSide / aspect;
    return 2.0f * std::atan(height * 0.5f / focalLength) * 180.0f / std::numbers::pi_v<float>;
}

// The offered lens whose field of view `fov` is at `aspect`, to a tenth of a degree; none for any other view.
[[nodiscard]] inline std::optional<int> matchingLens(float fov, float aspect) {
    for (int const focalLength : LensFocalLengths) {
        if (std::abs(lensFov(static_cast<float>(focalLength), aspect) - fov) < 0.05f) return focalLength;
    }
    return std::nullopt;
}

} // namespace playback::state::editing::model
