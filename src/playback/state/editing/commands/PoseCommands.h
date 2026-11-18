#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>
#include <vector>

namespace playback::state::editing::command {

// Replaces the project's pose keyframes as one undo step.
class SetPoseKeys final : public model::IEditCommand {
public:
    explicit SetPoseKeys(std::vector<model::PoseKey> keys);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<model::PoseKey>                mKeys;
    std::optional<std::vector<model::PoseKey>> mBefore;
};

} // namespace playback::state::editing::command
