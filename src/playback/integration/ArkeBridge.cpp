#include "playback/integration/ArkeBridge.h"

#include "playback/Playback.h"

#include "nlohmann/json.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>

namespace playback::integration {

namespace {

using ReloadFn = void (*)();

struct ArkeSetting {
    bool        enabled{};
    std::string shaderpack;

    bool operator==(ArkeSetting const&) const = default;
};

// The user's own setting while Playback has changed it, and what Playback last told Arke to run.
std::optional<ArkeSetting> gSaved;
std::optional<ArkeSetting> gApplied;

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
    if (*mustReload) reload();
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
            packs.emplace_back(text.begin(), text.end());
        }
    }
    std::ranges::sort(packs);
    return packs;
}

void applyArkeShader(std::string const& pack) {
    if (gApplied && gApplied->enabled == !pack.empty() && (pack.empty() || gApplied->shaderpack == pack)) return;
    if (!isArkeLoaded()) return;
    run(ArkeSetting{!pack.empty(), pack});
}

void restoreArkeShader() {
    if (!gSaved) return;
    run(*gSaved);
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

} // namespace playback::integration
