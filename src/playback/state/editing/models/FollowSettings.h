#pragma once

#include "MathTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace playback::state::editing::model {

// How a third-person camera track frames its subject. Behind is vanilla F5 (4 blocks back).
enum class FollowStyle : uint8_t { Behind, Shoulder, Orbit, Tripod };

struct FollowSettings {
    static constexpr float MaxDistance   = 32.0f;
    static constexpr float MaxHeight     = 16.0f;
    static constexpr float MaxSide       = 4.0f;
    static constexpr float MaxOrbitSpeed = 180.0f;
    static constexpr float MaxSmoothing  = 2.0f;

    FollowStyle style{FollowStyle::Behind};
    // Blocks from the subject's eyes: back along the view (Behind, Shoulder) or around them (Orbit).
    float distance{4.0f};
    // Blocks the camera sits above the eyes.
    float height{0.0f};
    // Shoulder: blocks to the subject's right; negative is the left shoulder.
    float side{0.8f};
    // Orbit: degrees per second from orbitStart at the track's first key. Angles use yaw's convention: 0 puts the
    // camera south of the subject, 90 west, so a positive speed goes south → west → north.
    float orbitSpeed{20.0f};
    float orbitStart{0.0f};
    // Tripod: where the camera stands. Until one is set the track frames like Behind.
    Vec3 tripod{};
    bool hasTripod{};
    // Seconds the camera takes to catch up with the framing (an exponential lag); 0 is rigid.
    float smoothing{0.0f};

    [[nodiscard]] FollowSettings sanitized() const {
        auto const range = [](float value, float low, float high, float fallback) {
            return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
        };
        FollowSettings const defaults;
        FollowSettings       out = *this;
        if (style > FollowStyle::Tripod) out.style = defaults.style;
        out.distance   = range(distance, 0.0f, MaxDistance, defaults.distance);
        out.height     = range(height, -MaxHeight, MaxHeight, defaults.height);
        out.side       = range(side, -MaxSide, MaxSide, defaults.side);
        out.orbitSpeed = range(orbitSpeed, -MaxOrbitSpeed, MaxOrbitSpeed, defaults.orbitSpeed);
        out.orbitStart = std::isfinite(orbitStart) ? std::remainder(orbitStart, 360.0f) : defaults.orbitStart;
        out.smoothing  = range(smoothing, 0.0f, MaxSmoothing, defaults.smoothing);
        if (!std::isfinite(tripod.x) || !std::isfinite(tripod.y) || !std::isfinite(tripod.z)) {
            out.tripod    = {};
            out.hasTripod = false;
        }
        return out;
    }

    bool operator==(FollowSettings const& other) const {
        return style == other.style && distance == other.distance && height == other.height && side == other.side
            && orbitSpeed == other.orbitSpeed && orbitStart == other.orbitStart && tripod.x == other.tripod.x
            && tripod.y == other.tripod.y && tripod.z == other.tripod.z && hasTripod == other.hasTripod
            && smoothing == other.smoothing;
    }
};

} // namespace playback::state::editing::model
