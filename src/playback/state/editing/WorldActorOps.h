#pragma once

#include "playback/state/editing/models/WorldActor.h"

#include <optional>
#include <string>

// The replay layer's segments are the stretches of the recording that play, in replay ticks and in order; a gap
// between two is cut out, so playback and export jump from one segment's end to the next one's start.
namespace playback::state::editing::WorldActorOps {
std::string splitAt(model::WorldActor& worldActor, int atTick);
bool        trimSegment(
           model::WorldActor& worldActor,
           const std::string& segmentId,
           int                newStartTick,
           int                newEndTick,
           int                totalTicks
       );

// Cuts the segment out, leaving a gap.
bool deleteSegment(model::WorldActor& worldActor, const std::string& segmentId);

// The first tick at or after `tick` that a segment plays; none past the last segment.
[[nodiscard]] std::optional<int> nextPlayableTick(model::WorldActor const& worldActor, int tick);
} // namespace playback::state::editing::WorldActorOps
