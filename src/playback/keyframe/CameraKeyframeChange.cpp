#include "CameraKeyframeChange.h"

#include "CameraKeyframeHandler.h"

namespace playback::keyframe {

namespace {

float wrapDegrees(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value < -180.0f) value += 360.0f;
    return value;
}

float interpolateAngle(float left, float right, float amount) {
    return wrapDegrees(left + wrapDegrees(right - left) * amount);
}

} // namespace

void CameraKeyframeChange::apply(CameraKeyframeHandler& handler) const { handler.applyCamera(*this); }

// The FOV is the track's own property; the evaluator sets it after the Transform is sampled.
CameraRenderState CameraKeyframeChange::toRenderState() const noexcept {
    return {position.x, position.y, position.z, yaw, pitch, roll};
}

CameraKeyframeChange
CameraKeyframeChange::interpolate(CameraKeyframeChange const& left, CameraKeyframeChange const& right, float amount) {
    // Not clamped: an overshooting easing curve passes amounts outside 0..1; the key span bounds every other caller.
    return {
        {
         left.position.x + (right.position.x - left.position.x) * amount,
         left.position.y + (right.position.y - left.position.y) * amount,
         left.position.z + (right.position.z - left.position.z) * amount,
         },
        interpolateAngle(left.yaw, right.yaw, amount),
        interpolateAngle(left.pitch, right.pitch, amount),
        interpolateAngle(left.roll, right.roll, amount),
    };
}

} // namespace playback::keyframe
