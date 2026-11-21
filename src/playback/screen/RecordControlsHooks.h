#pragma once

class ClientInstance;

namespace playback::screen {

void hookRecordControls(bool enable);

// Closes a pause screen that opened inside a replay (by focus loss or a menu button) once it shows. Client thread.
void dismissReplayPauseScreen(ClientInstance& client);

// Whether the top-right recording indicator may show while a recording is active.
void               setRecordingIndicatorEnabled(bool enabled);
[[nodiscard]] bool isRecordingIndicatorEnabled();

} // namespace playback::screen
