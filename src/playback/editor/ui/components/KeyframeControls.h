#pragma once

#include "playback/state/editing/models/PropertyChannel.h"
#include "playback/state/editing/models/MathTypes.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <string>

// Keyframes as in a video editor: every property keys on its own. Each row shows a diamond beside its name (hollow:
// no key here, filled: a key at the playhead) with arrows to the property's previous and next key; once a property has
// keys, editing it keys it at the playhead, and without keys an edit changes its one static value.
namespace playback::editor::ui::keyframes {

struct RowResult {
    // The value or its keys changed while a control is held (preview it) or by a click (commit it).
    bool changed{};
    // A key arrow asks to move the playhead to this tick (in the channel's own ticks).
    std::optional<int> seek;
};

// One keyable number: `staticValue` and `channels[name]` are edited in place. `tick` is the playhead in the channel's
// ticks. The value is shown times `displayScale` (100 shows a fraction in percent) within [low, high] (shown units).
RowResult propertyRow(
    char const*                           id,
    char const*                           icon,
    std::string const&                    label,
    state::editing::model::ValueChannels& channels,
    std::string const&                    name,
    float&                                staticValue,
    int                                   tick,
    float                                 low,
    float                                 high,
    char const*                           format,
    float                                 displayScale = 1.0f
);

// An RGB colour keyed as one: its three channels are `name` + "R", "G" and "B".
RowResult colorRow(
    char const*                           id,
    char const*                           icon,
    std::string const&                    label,
    state::editing::model::ValueChannels& channels,
    std::string const&                    name,
    std::array<float, 3>&                 staticValue,
    int                                   tick
);

// The keyframe diamond alone, for headers that key several properties at once: true when clicked.
bool diamondButton(char const* id, bool keyedHere, bool keyed, char const* tooltip);

// Key easing names for menus, by KeyEasing.
[[nodiscard]] std::string easingName(state::editing::model::KeyEasing easing);

// Every property's key at `tick`, or none, toggled as one: keys them all at their current values when any is
// missing, else removes them all. `values` gives the value to key for each name.
template <typename ValueOf>
bool toggleKeys(
    state::editing::model::ValueChannels& channels,
    std::initializer_list<char const*>    names,
    int                                   tick,
    ValueOf&&                             valueOf
) {
    bool const all = std::ranges::all_of(names, [&](char const* name) {
        auto const channel = channels.find(name);
        return channel != channels.end() && channel->second.contains(tick);
    });
    for (char const* name : names) {
        if (all) {
            if (auto channel = channels.find(name); channel != channels.end()) channel->second.erase(tick);
        } else {
            channels[name].insert_or_assign(tick, state::editing::model::ValueKey{valueOf(name)});
        }
    }
    state::editing::model::pruneChannels(channels);
    return true;
}

} // namespace playback::editor::ui::keyframes
