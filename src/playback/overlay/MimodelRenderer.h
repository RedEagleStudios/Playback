#pragma once

#include "OverlayRenderer.h"

#include <string>

namespace playback::overlay {

// Draws a Mine-imator model (.mimodel) placed in the world into `target` (premultiplied RGBA, already sized), seen
// through `camera`, depth-tested against itself only. Models are parsed and their textures decoded once, then cached
// by path and file time. `opacity` scales the model's alpha. Thread-safe.
// Returns false when the model cannot be loaded or nothing of it is in view; `error` then says why when it failed.
bool renderMimodel(
    state::editing::model::ModelPlacement const& placement,
    OverlayCamera const&                         camera,
    float                                        opacity,
    OverlayImage&                                target,
    std::string*                                 error = nullptr
);

// Loads (or reuses) the model and reports whether it parsed, for the inspector. Thread-safe.
[[nodiscard]] bool probeMimodel(std::string const& file, std::string& error);

} // namespace playback::overlay
