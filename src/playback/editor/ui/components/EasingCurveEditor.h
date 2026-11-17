#pragma once

#include "playback/state/editing/models/MathTypes.h"

#include <array>
#include <optional>

namespace playback::editor::ui {

struct EasingCurveEdit {
    std::array<state::editing::model::Vec2, 2> controls;
    // False while a handle is dragged, true once on release: commit then, so a drag is one undo step.
    bool committed{};
};

// A cubic-bezier easing graph: progress (up) over time (right) from one key to the next, with the two control points
// as draggable handles and a dot looping along the curve to show the motion. Time handles stay in [0, 1]; progress
// may leave it to overshoot. Returns the dragged controls each frame a handle moves and once on release.
std::optional<EasingCurveEdit>
drawEasingCurveEditor(char const* id, state::editing::model::Vec2 control1, state::editing::model::Vec2 control2);

} // namespace playback::editor::ui
