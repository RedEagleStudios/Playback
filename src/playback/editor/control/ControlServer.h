#pragma once

namespace playback::state {
class EditorContext;
}

namespace playback::editor::control {

// A local HTTP API on 127.0.0.1 that drives the replay editor the way its UI does, so scripts and tools can control a
// replay without the mouse or keyboard:
//   GET  /state    the editor state the UI draws (playhead, tracks, world, replay list, export) as JSON
//   GET  /actions  every action type name
//   POST /action   one EditorAction as JSON, e.g. {"type": "Seek", "tick": 1700}
//   POST /select   selects a keyframe as clicking it would: {"cameraId": "...", "tick": 120}
// Started with the replay UI when the config's control.enabled is set (the default), on control.port (7878).
void startControlServer(state::EditorContext& context, int port);
void stopControlServer();

} // namespace playback::editor::control
