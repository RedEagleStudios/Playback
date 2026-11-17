#pragma once

#include <cstdint>
#include <string_view>

namespace playback::editor::ui {

// How the viewport draws the replay while editing, as a 3D package's viewport shading. Exports always render with
// the project's graphics and the shader pack picked for Rendered, whatever the viewport shows.
enum class ViewShading : uint8_t {
    // Full brightness, no shadows or lighting, no shader pack: the lightest view to edit in.
    Flat,
    // The game's own lighting and shadows, no shader pack.
    Lit,
    // The picked shader pack (vanilla when none is picked): what the export renders.
    Rendered,
};

[[nodiscard]] constexpr std::string_view viewShadingName(ViewShading shading) noexcept {
    switch (shading) {
    case ViewShading::Flat:
        return "flat";
    case ViewShading::Lit:
        return "lit";
    case ViewShading::Rendered:
        return "rendered";
    }
    return "rendered";
}

[[nodiscard]] constexpr ViewShading viewShadingFromName(std::string_view name) noexcept {
    if (name == "flat") return ViewShading::Flat;
    if (name == "lit") return ViewShading::Lit;
    return ViewShading::Rendered;
}

} // namespace playback::editor::ui
