#include "WorldActorOps.h"

#include <algorithm>
#include <cmath>

namespace playback::state::editing::WorldActorOps {
std::string splitAt(model::WorldActor& worldActor, int atTick) {
    auto& segments = worldActor.segments;
    auto  it       = std::find_if(segments.begin(), segments.end(), [atTick](const auto& segment) {
        return atTick > segment.startTick && atTick < segment.endTick && !segment.locked;
    });
    if (it == segments.end()) return {};
    auto right       = *it;
    right.id         = it->id + ".split." + std::to_string(atTick);
    right.startTick  = atTick;
    right.sourceTick = it->sourceTick + static_cast<int>(std::floor((atTick - it->startTick) * it->speed));
    it->endTick      = atTick;
    segments.insert(std::next(it), right);
    return right.id;
}

// Segments play the replay where they sit, so a trim only moves an edge into a gap: it never pushes a neighbour.
bool trimSegment(model::WorldActor& worldActor, const std::string& id, int start, int end, int totalTicks) {
    auto& segments = worldActor.segments;
    auto  it = std::find_if(segments.begin(), segments.end(), [&](const auto& segment) { return segment.id == id; });
    if (it == segments.end() || it->locked || start >= end || (it->startTick == start && it->endTick == end))
        return false;
    size_t const index = static_cast<size_t>(std::distance(segments.begin(), it));
    int const    floor = index ? segments[index - 1].endTick : 0;
    int const    ceil  = index + 1 < segments.size() ? segments[index + 1].startTick : totalTicks;
    if (start < floor || end > ceil) return false;
    it->sourceTick += start - it->startTick;
    it->startTick   = start;
    it->endTick     = end;
    return true;
}

bool deleteSegment(model::WorldActor& worldActor, const std::string& id) {
    auto& segments = worldActor.segments;
    auto  it = std::find_if(segments.begin(), segments.end(), [&](const auto& segment) { return segment.id == id; });
    // The replay keeps at least one segment, or nothing would play.
    if (it == segments.end() || it->locked || segments.size() <= 1) return false;
    segments.erase(it);
    return true;
}

std::optional<int> nextPlayableTick(model::WorldActor const& worldActor, int tick) {
    for (auto const& segment : worldActor.segments) {
        if (tick < segment.endTick) return std::max(tick, segment.startTick);
    }
    return std::nullopt;
}
} // namespace playback::state::editing::WorldActorOps
