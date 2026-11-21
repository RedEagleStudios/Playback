#include "playback/integration/ArkeBridge.h"

#include "playback/Playback.h"
#include "playback/integration/FlatShaderPack.h"

#include "nlohmann/json.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string_view>
#include <system_error>

namespace playback::integration {

namespace {

using ReloadFn = void (*)();

struct ArkeSetting {
    bool        enabled{};
    std::string shaderpack;
    // Option values over the user's own for the pack, by option name.
    std::map<std::string, std::string> options;

    bool operator==(ArkeSetting const&) const = default;
};

// The user's own setting while Playback has changed it, and what Playback last told Arke to run.
std::optional<ArkeSetting> gSaved;
std::optional<ArkeSetting> gApplied;
// The user's own `<pack>.txt` of each pack whose options Playback rewrote; nullopt when the pack had none.
std::map<std::string, std::optional<std::string>> gSavedOptionFiles;

auto& logger() { return Playback::getInstance().getSelf().getLogger(); }

HMODULE arkeModule() noexcept { return GetModuleHandleW(L"arke.dll"); }

std::optional<std::filesystem::path> arkeDirectory() {
    auto* module = arkeModule();
    if (!module) return std::nullopt;
    std::array<wchar_t, 32768> buffer{};
    auto const length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::nullopt;
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

std::optional<nlohmann::json> readConfig(std::filesystem::path const& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    auto config = nlohmann::json::parse(in, nullptr, false);
    if (config.is_discarded() || !config.is_object()) return std::nullopt;
    return config;
}

ArkeSetting settingIn(nlohmann::json const& config) {
    auto const enabled = config.find("enabled");
    auto const pack    = config.find("shaderpack");
    return ArkeSetting{
        enabled != config.end() && enabled->is_boolean() && enabled->get<bool>(),
        pack != config.end() && pack->is_string() ? pack->get<std::string>() : std::string{},
    };
}

// Arke keeps each pack's option values beside it as `<pack>.txt`, one `NAME=value` per line.
std::optional<std::filesystem::path> optionFile(std::string const& pack) {
    auto const directory = arkeDirectory();
    if (!directory) return std::nullopt;
    return *directory / "shaderpacks" / std::filesystem::path(std::u8string(pack.begin(), pack.end()) + u8".txt");
}

std::optional<std::string> readText(std::filesystem::path const& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), {});
}

bool writeText(std::filesystem::path const& file, std::optional<std::string> const& text) {
    std::error_code error;
    if (!text) return std::filesystem::remove(file, error) || !error;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << *text;
    return static_cast<bool>(out);
}

std::map<std::string, std::string> optionValues(std::string_view text) {
    std::map<std::string, std::string> values;
    while (!text.empty()) {
        auto const end  = text.find('\n');
        auto       line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (auto const equals = line.find('='); equals != std::string_view::npos && equals != 0) {
            values.insert_or_assign(std::string(line.substr(0, equals)), std::string(line.substr(equals + 1)));
        }
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return values;
}

// Writes the user's own values for the running pack with Playback's over them. Returns whether the file changed.
bool writeOptions(ArkeSetting const& setting) {
    if (!setting.enabled || setting.shaderpack.empty()) return false;
    auto const file = optionFile(setting.shaderpack);
    if (!file) return false;
    auto const current = readText(*file);
    auto const saved   = gSavedOptionFiles.find(setting.shaderpack);
    auto const own     = saved != gSavedOptionFiles.end() ? saved->second : current;
    if (setting.options.empty()) {
        // Nothing over the user's own: their file exactly as it was.
        if (saved == gSavedOptionFiles.end() || current == saved->second) return false;
        return writeText(*file, saved->second);
    }
    auto values = optionValues(own.value_or(std::string{}));
    for (auto const& [name, value] : setting.options) values.insert_or_assign(name, value);
    std::string text;
    for (auto const& [name, value] : values) text += name + '=' + value + '\n';
    if (current == text) return false;
    gSavedOptionFiles.try_emplace(setting.shaderpack, current);
    return writeText(*file, text);
}

// Rewrites only the two fields Playback owns, so Arke's other settings stay as the user left them. Returns whether
// Arke must reload; every write is followed by one, so Arke already runs what its config says.
std::optional<bool> writeSetting(ArkeSetting const& setting) {
    auto const directory = arkeDirectory();
    if (!directory) return std::nullopt;
    auto const file   = *directory / "config.json";
    auto       config = readConfig(file);
    if (!config) {
        logger().warn("Arke's config.json is missing or unreadable; the viewport keeps Arke's own shader");
        return std::nullopt;
    }
    auto const current = settingIn(*config);
    if (!gSaved) gSaved = current;
    if (current.enabled == setting.enabled && (!setting.enabled || current.shaderpack == setting.shaderpack)) {
        return false;
    }
    (*config)["enabled"] = setting.enabled;
    if (!setting.shaderpack.empty()) (*config)["shaderpack"] = setting.shaderpack;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return std::nullopt;
    out << config->dump(4);
    if (!out) return std::nullopt;
    return true;
}

void reload() noexcept {
    auto* module = arkeModule();
    if (!module) return;
    if (auto* proc = GetProcAddress(module, "ArkeReload")) reinterpret_cast<ReloadFn>(reinterpret_cast<void*>(proc))();
}

void run(ArkeSetting const& setting) {
    if (gApplied == setting) return;
    auto const mustReload = writeSetting(setting);
    if (!mustReload) return;
    bool const optionsChanged = writeOptions(setting);
    if (*mustReload || optionsChanged) reload();
    gApplied = setting;
}

} // namespace

bool isArkeLoaded() noexcept { return arkeModule() != nullptr && GetProcAddress(arkeModule(), "ArkeReload"); }

std::vector<std::string> arkeShaderPacks() {
    std::vector<std::string> packs;
    auto const               directory = arkeDirectory();
    if (!directory) return packs;
    std::error_code error;
    for (auto const& entry : std::filesystem::directory_iterator(*directory / "shaderpacks", error)) {
        auto const name = entry.path().filename();
        // Arke keeps each pack's options beside it as `<pack>.txt`.
        if (entry.is_directory(error) || name.extension() == ".zip") {
            auto const text = name.u8string();
            std::string pack(text.begin(), text.end());
            // Playback's own pack for the Flat view is not a look to render with.
            if (pack != FlatShaderPackName) packs.push_back(std::move(pack));
        }
    }
    std::ranges::sort(packs);
    return packs;
}

void applyArkeShader(std::string const& pack, std::map<std::string, std::string> const& options) {
    ArkeSetting setting{!pack.empty(), pack, pack.empty() ? std::map<std::string, std::string>{} : options};
    if (gApplied == setting) return;
    if (!isArkeLoaded()) return;
    run(setting);
}

void restoreArkeShader() {
    bool filesChanged = false;
    for (auto const& [pack, text] : gSavedOptionFiles) {
        auto const file = optionFile(pack);
        if (file && readText(*file) != text) filesChanged = writeText(*file, text) || filesChanged;
    }
    gSavedOptionFiles.clear();
    auto const mustReload = gSaved ? writeSetting(*gSaved) : std::optional<bool>{false};
    if ((mustReload && *mustReload) || filesChanged) reload();
    gSaved.reset();
    gApplied.reset();
}

std::string arkeConfiguredShader() {
    auto const directory = arkeDirectory();
    if (!directory) return {};
    auto const config = readConfig(*directory / "config.json");
    if (!config) return {};
    // Arke keeps the pack it last ran while it is off, which is the user's pack all the same.
    return settingIn(*config).shaderpack;
}

std::optional<std::filesystem::path> arkeShaderPackPath(std::string const& pack) {
    auto const directory = arkeDirectory();
    if (!directory || pack.empty()) return std::nullopt;
    return *directory / "shaderpacks" / std::filesystem::path(std::u8string(pack.begin(), pack.end()));
}

std::map<std::string, std::string> arkeShaderOptions(std::string const& pack) {
    if (auto const saved = gSavedOptionFiles.find(pack); saved != gSavedOptionFiles.end()) {
        return optionValues(saved->second.value_or(std::string{}));
    }
    auto const file = optionFile(pack);
    if (!file) return {};
    return optionValues(readText(*file).value_or(std::string{}));
}

} // namespace playback::integration
