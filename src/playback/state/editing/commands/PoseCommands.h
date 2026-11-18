#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>
#include <vector>

namespace playback::state::editing::command {

// Replaces the project's pose clips as one undo step.
class SetPoseClips final : public model::IEditCommand {
public:
    explicit SetPoseClips(std::vector<model::PoseClip> clips);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<model::PoseClip>        mClips;
    std::optional<std::vector<model::PoseClip>> mBefore;
};

} // namespace playback::state::editing::command
