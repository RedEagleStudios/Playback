#pragma once

#include "playback/state/editing/models/CameraEntity.h"

#include <iterator>
#include <optional>
#include <utility>

namespace playback::state::editing {

// Adds the key AddKeyframe makes at `tick`, where `camera` has none: the captured pose if there is one, else a copy of
// the track's last key. A person track's key keeps viewing the same player as the key before it.
//
// A track's first key also starts the track at the view's FOV, unless the FOV is keyed. `viewFov` is called only then
// and returns the view's FOV as a std::optional<float>, empty when there is no view. A captured key only carries the
// pose, so a track left at the default 70 zoomed the view in or out on its first key.
template <class ViewFov>
void addCameraKey(model::CameraEntity& camera, int tick, std::optional<model::CameraKeyframe> captured, ViewFov&& viewFov) {
    if (camera.keysByTick.empty() && !model::hasKeys(camera.channels, model::CameraProperty::Fov)) {
        if (std::optional<float> const fov = viewFov()) camera.fov = *fov;
    }
    model::CameraKeyframe key;
    if (captured) key = std::move(*captured);
    else if (!camera.keysByTick.empty()) key = camera.keysByTick.rbegin()->second;
    if (camera.kind == model::CameraTrackKind::Keyframe) {
        key.subject.clear();
    } else if (auto const next = camera.keysByTick.lower_bound(tick); next != camera.keysByTick.begin()) {
        key.subject = std::prev(next)->second.subject;
    }
    camera.keysByTick.emplace(tick, std::move(key));
}

} // namespace playback::state::editing
