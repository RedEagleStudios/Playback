#pragma once

#include "playback/keyframe/CameraRenderState.h"
#include "playback/state/editing/models/FollowSettings.h"

#include <functional>
#include <optional>

namespace playback::keyframe {

// How far the camera may move from `from` towards `to` (0 to the full length) before a camera-blocking block.
using FollowCameraClip = std::function<float(CameraRenderState const& from, CameraRenderState const& to)>;

// The third-person camera for a subject whose eyes are at `eye`, `shotSeconds` into the track (orbit turns with it).
// Behind and Shoulder look where the subject looks; Orbit and Tripod look at the eyes. `clip` keeps the camera on the
// subject's side of walls; empty, nothing blocks it. FOV comes from the eye pose.
[[nodiscard]] CameraRenderState followCameraPose(
    state::editing::model::FollowSettings const& settings,
    CameraRenderState const&                     eye,
    double                                       shotSeconds,
    FollowCameraClip const&                      clip
);

// FollowSettings::smoothing: eases each new pose in over the time constant, by replay time, so a frame rate does not
// change it. Time going backwards, a jump of over a second, or reset() starts from the next pose as is.
class FollowSmoother {
public:
    [[nodiscard]] CameraRenderState apply(CameraRenderState const& target, double seconds, float timeConstant);
    void                            reset() { mState.reset(); }

private:
    std::optional<CameraRenderState> mState;
    double                           mSeconds{};
};

} // namespace playback::keyframe
