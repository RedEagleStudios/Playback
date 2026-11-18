#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace playback::integration {

// Optional link to Arke, the mod that runs Iris/OptiFine shader packs; every call is a no-op when it is absent. Arke
// rereads its config.json (enabled, shaderpack) and the running pack's `<pack>.txt` option values on ArkeReload, so
// a pack and its options are switched by rewriting those files and reloading. Render thread only: Arke's own
// settings window reloads from there.
[[nodiscard]] bool isArkeLoaded() noexcept;

// The shader packs in Arke's shaderpacks folder (archives and folders), sorted by name.
[[nodiscard]] std::vector<std::string> arkeShaderPacks();

// The pack Arke's own setting names, also while Arke is off; empty when Arke is absent or names none.
[[nodiscard]] std::string arkeConfiguredShader();

// Where `pack` lives in Arke's shaderpacks folder.
[[nodiscard]] std::optional<std::filesystem::path> arkeShaderPackPath(std::string const& pack);

// The user's own option values for `pack` (what Arke's settings window saved), also while Playback has put its own
// over them.
[[nodiscard]] std::map<std::string, std::string> arkeShaderOptions(std::string const& pack);

// Runs `pack` with `options` over the user's own values, or turns Arke off for vanilla rendering when the pack is
// empty. Nothing is reloaded when that is already what runs. The first change remembers the user's own setting and
// option values.
void applyArkeShader(std::string const& pack, std::map<std::string, std::string> const& options = {});

// The user's own setting and option values back; nothing happens when nothing was changed.
void restoreArkeShader();

} // namespace playback::integration
