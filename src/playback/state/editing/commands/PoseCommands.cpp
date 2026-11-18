#include "PoseCommands.h"

#include <utility>

namespace playback::state::editing::command {

SetPoseClips::SetPoseClips(std::vector<model::PoseClip> clips) : mClips(std::move(clips)) {
    for (auto& clip : mClips) clip = clip.sanitized();
}

void SetPoseClips::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.poses == mClips) return;
    mBefore     = state.poses;
    state.poses = mClips;
}

void SetPoseClips::undo(model::EditorStateExt& state) {
    if (mBefore) state.poses = *mBefore;
}

std::string SetPoseClips::label() const { return "Change Poses"; }

} // namespace playback::state::editing::command
