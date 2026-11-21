#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>
#include <vector>

namespace playback::state::editing::command {

// Replaces the project's overlay layers (titles, shapes, models) as one undo step.
class SetOverlays final : public model::IEditCommand {
public:
    explicit SetOverlays(std::vector<model::OverlayItem> overlays);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<model::OverlayItem>                mOverlays;
    std::optional<std::vector<model::OverlayItem>> mBefore;
};

} // namespace playback::state::editing::command
