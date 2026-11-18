#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace playback::state::editing::model {

// A pose keyframe: from its tick the entity plays the animation, as `/playanimation` would, until its next key. An
// empty animation hands the entity back to its recorded (in-game) animation. hideItems empties its hands meanwhile.
struct PoseKey {
    std::string id;
    // A player's UUID or an "entity:<id>" choice, as the replay's follow list names them.
    std::string subject;
    // The entity's name when the key was made, shown while it is not in the world.
    std::string subjectLabel;
    int         tick{};
    std::string animation{DefaultAnimation};
    bool        hideItems{true};

    static constexpr std::string_view DefaultAnimation = "animation.playback.pose.lean_crossed_arms";

    bool operator==(PoseKey const&) const = default;
};

// What the replay plays: one entity's pose from startTick until endTick (exclusive).
struct PoseClip {
    std::string id;
    std::string subject;
    std::string animation;
    int         startTick{};
    int         endTick{};
    bool        hideItems{};

    [[nodiscard]] bool activeAt(int tick) const noexcept { return tick >= startTick && tick < endTick; }

    bool operator==(PoseClip const&) const = default;
};

// Each key with an animation plays until the entity's next key, or past the replay's end.
[[nodiscard]] inline std::vector<PoseClip> poseClipsFrom(std::vector<PoseKey> const& keys, int totalTicks) {
    std::map<std::string, std::vector<PoseKey const*>> bySubject;
    for (auto const& key : keys) bySubject[key.subject].push_back(&key);
    std::vector<PoseClip> clips;
    for (auto& [subject, subjectKeys] : bySubject) {
        std::ranges::stable_sort(subjectKeys, {}, &PoseKey::tick);
        for (size_t index = 0; index < subjectKeys.size(); ++index) {
            auto const& key = *subjectKeys[index];
            if (key.animation.empty()) continue;
            int const end = index + 1 < subjectKeys.size() ? subjectKeys[index + 1]->tick : totalTicks + 1;
            if (end <= key.tick) continue;
            clips.push_back({key.id, subject, key.animation, key.tick, end, key.hideItems});
        }
    }
    return clips;
}

} // namespace playback::state::editing::model
