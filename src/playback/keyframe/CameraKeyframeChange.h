#pragma once

#include "CameraRenderState.h"
#include "playback/state/editing/models/MathTypes.h"

namespace playback::keyframe {

class CameraKeyframeHandler;

struct CameraKeyframeChange {
    state::editing::model::Vec3 position{};
    float                       yaw{};
    float                       pitch{};
    float                       roll{};
    float                       fov{70.0f};
    // Shake strength. Never splined: two still keys must give exactly no shake between them.
    float                       shake{};

    void apply(CameraKeyframeHandler& handler) const;

    [[nodiscard]] CameraRenderState toRenderState() const noexcept;

    [[nodiscard]] static CameraKeyframeChange

    interpolate(CameraKeyframeChange const& left, CameraKeyframeChange const& right, float amount);
};

} // namespace playback::keyframe
