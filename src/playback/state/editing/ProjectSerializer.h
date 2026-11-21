#pragma once

#include "playback/state/editing/models/EditorStateExt.h"

#include <string>

namespace playback::state::editing {

constexpr int kProjectFormatVersion = 1;

[[nodiscard]] std::string serializeProject(model::EditorStateExt const& project);

// Returns false and fills `error` when the payload is not a project of a supported version.
[[nodiscard]] bool deserializeProject(std::string const& payload, model::EditorStateExt& project, std::string& error);

// Overlay layers alone as a JSON array, for the control server.
[[nodiscard]] std::string serializeOverlays(std::vector<model::OverlayItem> const& overlays);
[[nodiscard]] bool        deserializeOverlays(std::string const& payload, std::vector<model::OverlayItem>& overlays);

} // namespace playback::state::editing
