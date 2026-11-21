#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>

namespace playback::state::editing::model {

// How a property's value travels from one key to the next, as in a video editor's keyframe menu.
enum class KeyEasing : uint8_t { Linear = 0, EaseInOut, EaseIn, EaseOut, Hold };

struct ValueKey {
    float     value{};
    KeyEasing easing{KeyEasing::EaseInOut};

    bool operator==(ValueKey const&) const = default;
};

// One property's own keyframes, by tick. Every property keys on its own: keying one never adds keys to another.
using ValueChannel = std::map<int, ValueKey>;

// The keyed properties of one object, by property name ("fov", "opacity", ...). A property without an entry, or with
// an empty channel, keeps its static value.
using ValueChannels = std::map<std::string, ValueChannel>;

[[nodiscard]] inline float easeKey(KeyEasing easing, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (easing) {
    case KeyEasing::Linear:
        return t;
    case KeyEasing::EaseIn:
        return t * t * t;
    case KeyEasing::EaseOut: {
        float const u = 1.0f - t;
        return 1.0f - u * u * u;
    }
    case KeyEasing::EaseInOut:
        return t * t * (3.0f - 2.0f * t);
    case KeyEasing::Hold:
        return 0.0f;
    }
    return t;
}

// The channel's value at `tick`: its first key's before it, its last key's after it, eased between by the easing of
// the key the stretch starts from. None when the channel has no keys.
[[nodiscard]] inline std::optional<float> sampleChannel(ValueChannel const& channel, double tick) {
    if (channel.empty()) return std::nullopt;
    auto const right = channel.upper_bound(static_cast<int>(std::floor(tick)));
    if (right == channel.begin()) return channel.begin()->second.value;
    auto const left = std::prev(right);
    if (right == channel.end()) return left->second.value;
    float const span = static_cast<float>(right->first - left->first);
    float const t    = span > 0.0f ? static_cast<float>(tick - left->first) / span : 1.0f;
    float const eased = easeKey(left->second.easing, t);
    return left->second.value + (right->second.value - left->second.value) * eased;
}

// A property's value at `tick`: keyed when its channel has keys, else `fallback`.
[[nodiscard]] inline float channelValue(ValueChannels const& channels, std::string const& name, double tick, float fallback) {
    auto const channel = channels.find(name);
    if (channel == channels.end()) return fallback;
    return sampleChannel(channel->second, tick).value_or(fallback);
}

[[nodiscard]] inline bool hasKeys(ValueChannels const& channels, std::string const& name) {
    auto const channel = channels.find(name);
    return channel != channels.end() && !channel->second.empty();
}

// Drops channels left without keys, so "keyed" always means "has keys".
inline void pruneChannels(ValueChannels& channels) {
    std::erase_if(channels, [](auto const& entry) { return entry.second.empty(); });
}

} // namespace playback::state::editing::model
