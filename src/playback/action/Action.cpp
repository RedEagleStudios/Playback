#include "Action.h"

#include "playback/io/AsyncReplaySaver.h"
#include "playback/replay/ReplaySession.h"

namespace playback::action {
using playback::io::readSnapshotContext;
using playback::io::writeSnapshotContext;

void ActionNextTick::handle(replay::ReplaySession& session, PlaybackBuffer&) { session.handleNextTick(); }

void ActionSnapshotContext::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleSnapshotContext(readSnapshotContext(data));
}

void ActionCreateLocalPlayer::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleCreateLocalPlayer(data);
}

void ActionLevelChunkCached::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleLevelChunkCached(data.getVarInt().value());
}

void ActionSubChunkCached::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleSubChunkCached(data.getVarInt().value());
}

void ActionConfigurationPacket::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleConfigurationPacket(data);
}

void ActionGamePacket::handle(replay::ReplaySession& session, PlaybackBuffer& data) { session.handleGamePacket(data); }

void ActionMoveEntities::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleMoveEntities(data);
}

void ActionLocalCameraOffset::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleLocalCameraOffset(data);
}

void ActionLocalViewSamples::handle(replay::ReplaySession& session, PlaybackBuffer& data) {
    session.handleLocalViewSamples(data);
}

// Read only by the load-time scan (ReplayReader::readCommandCameraSamples); playback skips it.
void ActionCommandCamera::handle(replay::ReplaySession&, PlaybackBuffer& data) { (void)read(data); }

void ActionCommandCamera::write(BinaryStream& stream, keyframe::CameraRenderState const& view) {
    for (float const value : {view.x, view.y, view.z, view.yaw, view.pitch, view.roll, view.fov}) {
        stream.writeFloat(value, nullptr, nullptr);
    }
}

keyframe::CameraRenderState ActionCommandCamera::read(PlaybackBuffer& data) {
    keyframe::CameraRenderState view;
    for (float* value : {&view.x, &view.y, &view.z, &view.yaw, &view.pitch, &view.roll, &view.fov}) {
        *value = data.getFloat().value();
    }
    return view;
}

} // namespace playback::action
