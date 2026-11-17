#pragma once

#include "playback/state/editing/models/EasingPresets.h"

#include <cstddef>
#include <optional>
#include <string>

namespace playback::editor::ui {

// "Sine In", "Back In-Out", ... in the UI language.
[[nodiscard]] std::string easingPresetName(state::editing::model::EasingPreset const& preset);

// One submenu per easing family, drawn inside an open menu. Returns the preset clicked this frame.
std::optional<state::editing::model::EasingPreset> drawEasingPresetMenus(std::optional<std::size_t> current);

// A combo of every preset grouped by family; `preview` shows while it is closed. Returns the preset picked this frame.
std::optional<state::editing::model::EasingPreset>
drawEasingPresetCombo(char const* id, char const* preview, std::optional<std::size_t> current);

} // namespace playback::editor::ui
