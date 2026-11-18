#include "PoseCommands.h"

#include <algorithm>
#include <utility>

namespace playback::state::editing::command {

SetPoseKeys::SetPoseKeys(std::vector<model::PoseKey> keys) : mKeys(std::move(keys)) {
    for (auto& key : mKeys) key.tick = std::max(0, key.tick);
    std::ranges::stable_sort(mKeys, {}, &model::PoseKey::tick);
}

void SetPoseKeys::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.poseKeys == mKeys) return;
    mBefore        = state.poseKeys;
    state.poseKeys = mKeys;
}

void SetPoseKeys::undo(model::EditorStateExt& state) {
    if (mBefore) state.poseKeys = *mBefore;
}

std::string SetPoseKeys::label() const { return "Change Pose Keyframes"; }

} // namespace playback::state::editing::command
