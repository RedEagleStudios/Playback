#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::state::editing::command {

// Replaces the project's world settings (time of day, clouds) as one undo step.
class SetWorldSettings final : public model::IEditCommand {
public:
    explicit SetWorldSettings(model::WorldSettings settings);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    model::WorldSettings                 mSettings;
    std::optional<model::EditorStateExt> mBefore;
};

} // namespace playback::state::editing::command
