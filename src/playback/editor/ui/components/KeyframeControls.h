#pragma once

#include "playback/state/editing/models/PropertyChannel.h"
#include "playback/state/editing/models/MathTypes.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Keyframes as in a video editor: every property keys on its own. Each row shows a diamond beside its name (hollow:
// no key here, filled: a key at the playhead) with arrows to the property's previous and next key. Every edit keys the
// property at the playhead; the static value is what it takes without keys.
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

// The header of properties keyed as one (a camera shake): its diamond keys every name at the playhead, each at
// `valueOf(name)`, or removes them all, and right-click sets their easing together. Their fields carry no diamonds.
RowResult groupHeader(
    char const*                                     id,
    char const*                                     icon,
    std::string const&                              label,
    state::editing::model::ValueChannels&           channels,
    std::vector<std::string> const&                 names,
    int                                             tick,
    std::function<float(std::string const&)> const& valueOf
);

// Key easing names for menus, by KeyEasing.
[[nodiscard]] std::string easingName(state::editing::model::KeyEasing easing);

} // namespace playback::editor::ui::keyframes
