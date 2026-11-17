#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace playback::state::editing::model {

// The game's graphics modes a project can pick; the values are the game's GraphicsMode option values.
enum class GraphicsStyle : int {
    Simple         = 0,
    Fancy          = 1,
    VibrantVisuals = 2,
};

// A resource pack by identity. The name is only shown, and kept for packs that are not installed here.
struct GlobalPack {
    std::string id;
    std::string version;
    std::string name;

    bool operator==(GlobalPack const&) const = default;
};

// Replay-world look that the project overrides, in preview and export alike.
struct WorldSettings {
    static constexpr int TicksPerDay = 24000;

    // Off: the recorded time of day plays back.
    bool overrideTime{};
    // Ticks into the day, [0, TicksPerDay): 0 sunrise, 6000 noon, 13000 night, 18000 midnight.
    int timeOfDay{6000};
    // Frozen: timeOfDay for the whole replay. Running: timeOfDay at the first tick, then one tick per replay tick.
    bool freezeTime{true};
    bool hideClouds{};
    // Off: the world's own fog (biome, weather, /fog) shows.
    bool overrideFog{};
    // Where the fog starts and ends, as fractions of the render distance, in [0, MaxFogDistance]; vanilla is about
    // 0.92 to 1. The fog fades in between, so ends past 1 thin it out until it is gone. The end stays MinFogRange
    // past the start: with no distance to fade over there is no fog at all.
    float fogStart{0.92f};
    float fogEnd{1.0f};
    // Off: the viewer's own graphics mode.
    bool          overrideGraphics{};
    GraphicsStyle graphics{GraphicsStyle::Fancy};
    // Off: the viewer's own global resource packs. On: exactly globalPacks, top priority first, in place from when
    // the replay world loads until it closes; the replay world's own packs stay above them.
    bool                    overrideGlobalPacks{};
    std::vector<GlobalPack> globalPacks;
    // Off: the game's own colours.
    bool overrideColor{};
    // Stops, [-MaxExposure, MaxExposure].
    float exposure{};
    // [-1, 1]: cool to warm, and green to magenta.
    float temperature{};
    float tint{};

    static constexpr float MaxExposure = 3.0f;

    static constexpr float MaxFogDistance = 4.0f;
    static constexpr float MinFogRange    = 0.01f;

    // The settings brought into range, as the UI and project files may hold anything.
    [[nodiscard]] WorldSettings sanitized() const {
        auto const unit = [](float value, float low, float high, float fallback) {
            return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
        };
        WorldSettings out = *this;
        out.timeOfDay     = std::clamp(timeOfDay, 0, TicksPerDay - 1);
        out.fogStart      = unit(fogStart, 0.0f, MaxFogDistance - MinFogRange, 0.92f);
        out.fogEnd =
            unit(fogEnd, out.fogStart + MinFogRange, MaxFogDistance, std::max(out.fogStart + MinFogRange, 1.0f));
        out.exposure    = unit(exposure, -MaxExposure, MaxExposure, 0.0f);
        out.temperature = unit(temperature, -1.0f, 1.0f, 0.0f);
        out.tint        = unit(tint, -1.0f, 1.0f, 0.0f);
        if (graphics != GraphicsStyle::Simple && graphics != GraphicsStyle::Fancy
            && graphics != GraphicsStyle::VibrantVisuals)
            out.graphics = GraphicsStyle::Fancy;
        return out;
    }

    bool operator==(WorldSettings const&) const = default;
};

} // namespace playback::state::editing::model
