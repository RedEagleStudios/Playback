#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "PropertyChannel.h"

#include <string_view>

namespace playback::state::editing::model {

// The game's graphics modes a project can pick; the values are the game's GraphicsMode option values.
enum class GraphicsStyle : int {
    Simple         = 0,
    Fancy          = 1,
    VibrantVisuals = 2,
};

// The weather a project can force on the replay world.
enum class WeatherStyle : int {
    Clear   = 0,
    Rain    = 1,
    Thunder = 2,
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
    // With the fog override: the fog's own colour (sRGB, 0-1). Shader packs only, in Rendered and exports, and only
    // packs that declare Playback's fog options (FOG_CUSTOM, FOG_END, FOG_COLOR_R/G/B); the game's own fog keeps its
    // colour.
    bool                 overrideFogColor{};
    std::array<float, 3> fogColor{0.74f, 0.82f, 0.92f};
    // Off: the recorded weather plays back. Rain and Thunder fall wherever the biome has precipitation; Thunder also
    // darkens the sky. Lightning bolts are recorded entities and still strike as recorded.
    bool         overrideWeather{};
    WeatherStyle weather{WeatherStyle::Clear};
    // Off: the viewer's own graphics mode.
    bool          overrideGraphics{};
    GraphicsStyle graphics{GraphicsStyle::Fancy};
    // Shader pack option values over the user's own, by pack and option name, in place while the replay is open and
    // in every export. Values are spelt as the pack lists them.
    std::map<std::string, std::map<std::string, std::string>> shaderOptions;
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
    // Keyed numbers (worldProperty names), each on its own, by replay tick; a property without keys keeps the value
    // above. The switches are never keyed.
    ValueChannels channels;

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
        for (auto& channel : out.fogColor) channel = unit(channel, 0.0f, 1.0f, 0.0f);
        if (graphics != GraphicsStyle::Simple && graphics != GraphicsStyle::Fancy
            && graphics != GraphicsStyle::VibrantVisuals)
            out.graphics = GraphicsStyle::Fancy;
        if (weather != WeatherStyle::Clear && weather != WeatherStyle::Rain && weather != WeatherStyle::Thunder)
            out.weather = WeatherStyle::Clear;
        return out;
    }

    bool operator==(WorldSettings const&) const = default;
};

// The world's keyable numbers, by name; an unknown name gives null. The time of day keys as a float and rounds.
[[nodiscard]] inline float* worldProperty(WorldSettings& world, std::string_view name) {
    if (name == "fogStart") return &world.fogStart;
    if (name == "fogEnd") return &world.fogEnd;
    if (name == "fogColorR") return &world.fogColor[0];
    if (name == "fogColorG") return &world.fogColor[1];
    if (name == "fogColorB") return &world.fogColor[2];
    if (name == "exposure") return &world.exposure;
    if (name == "temperature") return &world.temperature;
    if (name == "tint") return &world.tint;
    return nullptr;
}

inline constexpr char const* WorldTimeOfDay = "timeOfDay";

// A property's value as set (not keyed).
[[nodiscard]] inline float worldStaticValue(WorldSettings const& world, std::string_view name) {
    if (name == WorldTimeOfDay) return static_cast<float>(world.timeOfDay);
    // worldProperty only locates the field; nothing is written through it here.
    float const* field = worldProperty(const_cast<WorldSettings&>(world), name);
    return field ? *field : 0.0f;
}

inline void setWorldStaticValue(WorldSettings& world, std::string_view name, float value) {
    if (name == WorldTimeOfDay) {
        world.timeOfDay = static_cast<int>(std::lround(value));
        return;
    }
    if (float* field = worldProperty(world, name)) *field = value;
}

// The world as it stands at `tick`: every keyed property at its keyed value, brought into range.
[[nodiscard]] inline WorldSettings worldAt(WorldSettings const& world, double tick) {
    if (world.channels.empty()) return world;
    WorldSettings result = world;
    for (auto const& [name, channel] : world.channels) {
        if (auto const value = sampleChannel(channel, tick)) setWorldStaticValue(result, name, *value);
    }
    return result.sanitized();
}

} // namespace playback::state::editing::model
