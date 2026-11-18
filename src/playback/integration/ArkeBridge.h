#pragma once

#include <string>
#include <vector>

namespace playback::integration {

// Optional link to Arke, the mod that runs Iris/OptiFine shader packs; every call is a no-op when it is absent. Arke
// rereads its config.json (enabled, shaderpack) on ArkeReload, so a pack is switched by rewriting those two fields
// and reloading. Render thread only: Arke's own settings window reloads from there.
[[nodiscard]] bool isArkeLoaded() noexcept;

// The shader packs in Arke's shaderpacks folder (archives and folders), sorted by name.
[[nodiscard]] std::vector<std::string> arkeShaderPacks();

// The pack Arke's own setting names, also while Arke is off; empty when Arke is absent or names none.
[[nodiscard]] std::string arkeConfiguredShader();

// Runs `pack`, or turns Arke off for vanilla rendering when it is empty. Nothing is reloaded when that is already
// what runs. The first change remembers the user's own setting.
void applyArkeShader(std::string const& pack);

// The user's own setting back; nothing happens when nothing was changed.
void restoreArkeShader();

} // namespace playback::integration
