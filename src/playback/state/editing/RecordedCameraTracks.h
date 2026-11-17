#pragma once

#include "playback/keyframe/CameraRenderState.h"
#include "playback/state/editing/models/CameraKeyframe.h"

#include <map>
#include <span>
#include <vector>

namespace playback::state::editing {

// The recorder's view at one replay tick while a server camera owned it (/camera, add-on cutscenes).
struct RecordedCameraView {
    int                         tick{};
    keyframe::CameraRenderState view;
};

// One keyframe track per unbroken run of ticks (a cutscene), tick ascending in and out. Keys stay only where Linear
// interpolation between them would leave the recorded view; a jump between two ticks (a cut) holds the pose before it.
[[nodiscard]] std::vector<std::map<int, model::CameraKeyframe>>
recordedCameraTracks(std::span<RecordedCameraView const> views);

} // namespace playback::state::editing
