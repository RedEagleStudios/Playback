#include "FollowCamera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace playback::keyframe {

namespace {

using state::editing::model::FollowSettings;
using state::editing::model::FollowStyle;

constexpr double RadiansPerDegree = std::numbers::pi / 180.0;
constexpr double DegreesPerRadian = 180.0 / std::numbers::pi;
// Gap the camera keeps from the block it stopped at.
constexpr float WallMargin = 0.2f;

float angleDelta(float from, float to) { return std::remainder(to - from, 360.0f); }

// Points `pose` at `target`: yaw 0 looks south (+Z), 90 west; positive pitch looks down.
void lookAt(CameraRenderState& pose, CameraRenderState const& target) {
    double const dx = target.x - pose.x;
    double const dy = target.y - pose.y;
    double const dz = target.z - pose.z;
    if (std::abs(dx) + std::abs(dy) + std::abs(dz) < 1e-6) return;
    pose.yaw   = static_cast<float>(std::atan2(-dx, dz) * DegreesPerRadian);
    pose.pitch = static_cast<float>(-std::atan2(dy, std::hypot(dx, dz)) * DegreesPerRadian);
    pose.roll  = 0.0f;
}

// `pose` moved from `eye` to `wanted`, stopping WallMargin short of the first wall in between.
CameraRenderState clipped(CameraRenderState const& eye, CameraRenderState wanted, FollowCameraClip const& clip) {
    double const length = std::hypot(wanted.x - eye.x, wanted.y - eye.y, wanted.z - eye.z);
    if (!clip || length < 1e-6) return wanted;
    float const free = clip(eye, wanted);
    if (!std::isfinite(free) || free >= length) return wanted;
    double const scale = std::max(0.0, static_cast<double>(free) - WallMargin) / length;
    wanted.x           = static_cast<float>(eye.x + (wanted.x - eye.x) * scale);
    wanted.y           = static_cast<float>(eye.y + (wanted.y - eye.y) * scale);
    wanted.z           = static_cast<float>(eye.z + (wanted.z - eye.z) * scale);
    return wanted;
}

} // namespace

CameraRenderState followCameraPose(
    FollowSettings const&    settings,
    CameraRenderState const& eye,
    double                   shotSeconds,
    FollowCameraClip const&  clip
) {
    CameraRenderState camera = eye;
    switch (settings.style) {
    case FollowStyle::Orbit: {
        double const angle = (settings.orbitStart + settings.orbitSpeed * shotSeconds) * RadiansPerDegree;
        camera.x           = static_cast<float>(eye.x - std::sin(angle) * settings.distance);
        camera.y           = eye.y + settings.height;
        camera.z           = static_cast<float>(eye.z + std::cos(angle) * settings.distance);
        camera             = clipped(eye, camera, clip);
        lookAt(camera, eye);
        return camera;
    }
    case FollowStyle::Tripod:
        if (settings.hasTripod) {
            camera.x = settings.tripod.x;
            camera.y = settings.tripod.y;
            camera.z = settings.tripod.z;
            lookAt(camera, eye);
            return camera;
        }
        [[fallthrough]];
    case FollowStyle::Behind:
    case FollowStyle::Shoulder:
    default: {
        double const yaw   = eye.yaw * RadiansPerDegree;
        double const pitch = eye.pitch * RadiansPerDegree;
        // Back is the opposite of the view {-sin(yaw)cos(pitch), -sin(pitch), cos(yaw)cos(pitch)}; right is the
        // subject's right hand, west when it faces south.
        double const backX  = std::sin(yaw) * std::cos(pitch);
        double const backY  = std::sin(pitch);
        double const backZ  = -std::cos(yaw) * std::cos(pitch);
        double const side   = settings.style == FollowStyle::Shoulder ? settings.side : 0.0;
        double const rightX = -std::cos(yaw);
        double const rightZ = -std::sin(yaw);
        camera.x            = static_cast<float>(eye.x + backX * settings.distance + rightX * side);
        camera.y            = static_cast<float>(eye.y + backY * settings.distance + settings.height);
        camera.z            = static_cast<float>(eye.z + backZ * settings.distance + rightZ * side);
        return clipped(eye, camera, clip);
    }
    }
}

CameraRenderState FollowSmoother::apply(CameraRenderState const& target, double seconds, float timeConstant) {
    double const elapsed = seconds - mSeconds;
    if (!mState || timeConstant <= 0.0f || elapsed < -1e-6 || elapsed > 1.0) {
        mState   = target;
        mSeconds = seconds;
        return target;
    }
    if (elapsed <= 0.0) return *mState;
    auto const amount  = static_cast<float>(1.0 - std::exp(-elapsed / timeConstant));
    auto&      state   = *mState;
    state.x           += (target.x - state.x) * amount;
    state.y           += (target.y - state.y) * amount;
    state.z           += (target.z - state.z) * amount;
    state.yaw         += angleDelta(state.yaw, target.yaw) * amount;
    state.pitch       += angleDelta(state.pitch, target.pitch) * amount;
    state.roll        += angleDelta(state.roll, target.roll) * amount;
    state.fov         += (target.fov - state.fov) * amount;
    mSeconds           = seconds;
    return state;
}

} // namespace playback::keyframe
