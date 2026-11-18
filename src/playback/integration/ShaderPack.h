#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playback::integration {

// One configurable option of an Iris/OptiFine shader pack, as the pack's sources declare it.
struct ShaderOption {
    std::string name;
    // `#define NAME` or `//#define NAME`: on or off, its value "true" or "false".
    bool toggle{};
    std::string defaultValue;
    // The values a choice may take, in the order the pack lists them.
    std::vector<std::string> values;
    // Listed under `sliders=`: drawn as a slider over its values rather than a list.
    bool slider{};
};

// A pack's options and how its own menu lays them out.
struct ShaderPackInfo {
    std::map<std::string, ShaderOption> options;
    // Screen entries by screen name; "" is the main screen. An entry is an option name, `[SCREEN]` for a sub-screen,
    // or a placeholder such as `<empty>` and `<profile>`.
    std::map<std::string, std::vector<std::string>> screens;
    std::map<std::string, std::string>              lang;

    // `option.NAME`, `screen.NAME` and `value.NAME.v` from the pack's English lang file, without colour codes.
    [[nodiscard]] std::string text(std::string const& key, std::string_view fallback) const;
    [[nodiscard]] ShaderOption const* option(std::string const& name) const;
};

// Reads a pack (a zip or a folder holding `shaders/`). Nullopt when it cannot be read.
[[nodiscard]] std::optional<ShaderPackInfo> readShaderPack(std::filesystem::path const& pack);

// The listed value of a numeric choice nearest to `value`, as the pack spells it; empty when the option has no
// numeric values.
[[nodiscard]] std::string nearestShaderValue(ShaderOption const& option, float value);

} // namespace playback::integration
