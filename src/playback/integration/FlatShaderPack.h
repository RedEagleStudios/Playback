#pragma once

#include <string>
#include <string_view>

namespace playback::integration {

// The pack the viewport's Flat shading runs on Arke: every surface in its texture colour (and biome tint), with no
// lighting, face shading, ambient occlusion, shadows or post effects.
inline constexpr std::string_view FlatShaderPackName = "Playback Flat";

// Writes the Flat pack into Arke's shaderpacks folder when it is missing or out of date. Returns its name, or an empty
// string when Arke is absent or the pack cannot be written.
[[nodiscard]] std::string installFlatShaderPack();

} // namespace playback::integration
