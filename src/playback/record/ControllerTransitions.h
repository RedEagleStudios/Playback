#pragma once

#include <algorithm>
#include <utility>
#include <vector>

namespace playback::record {

// Adds an animation controller transition an actor received (one /playanimation packet) to the actor's list, which a
// snapshot replays in order.
//
// The client appends every transition, and a state may have two to the same target (Ore Fusion's attacks go to their
// hold when the hammer is put away and when the attack ends), so distinct transitions all stay. An exact repeat, such as
// a controller sent again on the next equip, adds no transition but is now the latest: the client ends each controller
// on the last one it gets. Kept where it first came, a bicycle stance sent again on mounting lost to a stale one from an
// earlier ride, and the replayed rider held the air pose.
template <class Transition>
void addControllerTransition(std::vector<Transition>& transitions, Transition transition) {
    auto const repeated = std::ranges::find_if(transitions, [&](Transition const& existing) {
        return existing.controller == transition.controller && existing.state == transition.state
            && existing.nextState == transition.nextState && existing.stopExpression == transition.stopExpression;
    });
    if (repeated != transitions.end()) transitions.erase(repeated);
    transitions.push_back(std::move(transition));
}

} // namespace playback::record
