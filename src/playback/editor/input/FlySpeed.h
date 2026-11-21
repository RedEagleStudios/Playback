#pragma once

#include "imgui.h"

class Player;

namespace playback::editor::input {

// Multiplier on the observer's flight speed, picked with the wheel while the left button flies the camera.
[[nodiscard]] float flySpeedMultiplier();

// Positive notches speed up. Input thread.
void stepFlySpeed(int notches);

void resetFlySpeed();

// Client thread, once per client update. `player` is the local player (may be null); `replayOpen` is whether a replay
// world is live. While open the multiplier is kept on the player's flight; when it closes the game's own speeds are put
// back and the multiplier returns to 1x for the next replay.
void tickFlySpeed(Player* player, bool replayOpen);

// Render thread. A pill near the bottom centre of the video, shown while flying and briefly after a change.
void drawFlySpeedIndicator(ImDrawList* drawList, ImVec2 videoMin, ImVec2 videoMax);

} // namespace playback::editor::input
