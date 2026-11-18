#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace playback::state::editing::model {

// A pose played on a replay player from startTick until endTick, as `/playanimation` would: any animation the
// loaded resource packs define for players. hideItems empties the player's hands for the clip.
struct PoseClip {
    std::string id;
    // The player's UUID, as the replay's player list names them.
    std::string subject;
    std::string animation{DefaultAnimation};
    int         startTick{};
    int         endTick{};
    bool        hideItems{true};

    static constexpr std::string_view DefaultAnimation = "animation.playback.pose.lean_crossed_arms";

    [[nodiscard]] PoseClip sanitized() const {
        PoseClip out  = *this;
        out.startTick = std::max(0, startTick);
        out.endTick   = std::max(out.startTick + 1, endTick);
        if (out.animation.empty()) out.animation = DefaultAnimation;
        return out;
    }

    [[nodiscard]] bool activeAt(int tick) const noexcept { return tick >= startTick && tick < endTick; }

    bool operator==(PoseClip const&) const = default;
};

} // namespace playback::state::editing::model
