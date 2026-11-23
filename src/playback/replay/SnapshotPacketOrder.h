#pragma once

#include <algorithm>
#include <utility>
#include <vector>

namespace playback::replay {

// Orders a snapshot's game packets, each an (id, payload) pair, for applying: controller clips (`controllerClip`, the
// TriggerAnimation id) go after every other packet, so after the actors and ride links they apply to. The recorder
// writes the player's clips before the vehicle that carries them, and a clip whose stop expression tests the ride
// (q.is_riding) would otherwise end on its first evaluation, leaving the rider in the vanilla riding pose. Each group
// keeps its recorded order.
template <class Id, class Payload>
void orderSnapshotPackets(std::vector<std::pair<Id, Payload>>& packets, Id controllerClip) {
    std::ranges::stable_partition(packets, [controllerClip](auto const& entry) {
        return entry.first != controllerClip;
    });
}

} // namespace playback::replay
