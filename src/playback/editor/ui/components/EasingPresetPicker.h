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

// What the easing combo picked: a preset, or None (no easing curve) when `preset` is empty.
struct EasingPick {
    std::optional<state::editing::model::EasingPreset> preset;
};

// A combo of None and every preset grouped by family; `preview` shows while it is closed. `noneCurrent` marks None as
// the current choice. Returns what was picked this frame.
std::optional<EasingPick>
drawEasingPresetCombo(char const* id, char const* preview, std::optional<std::size_t> current, bool noneCurrent);

} // namespace playback::editor::ui
