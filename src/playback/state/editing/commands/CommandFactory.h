#pragma once

#include "playback/state/editing/models/CameraEntity.h"
#include "playback/state/editing/models/IEditCommand.h"
#include "playback/state/editing/models/PoseClip.h"
#include "playback/state/editing/models/OverlayItem.h"
#include "playback/state/editing/models/SubActor.h"
#include "playback/state/editing/models/WorldSettings.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace playback::state::editing::command {

class CommandFactory {
public:
    static std::unique_ptr<model::IEditCommand> createSplitWorldActor(int tick);
    static std::unique_ptr<model::IEditCommand> createTrimWorldActor(const std::string& id, int start, int end);

    static std::unique_ptr<model::IEditCommand> createDeleteWorldActorSegment(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createSetWorldActorLocked(bool locked);
    static std::unique_ptr<model::IEditCommand>
    createAddFreeCamera(const std::string& name, model::CameraTrackKind kind);
    static std::unique_ptr<model::IEditCommand> createDeleteCamera(const std::string& id);
    static std::unique_ptr<model::IEditCommand>
    createCreateBindingCamera(const std::string& id, const std::string& name);
    static std::unique_ptr<model::IEditCommand> createUnbindCamera(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createAddCameraKeyframe(const std::string& id, int tick);
    static std::unique_ptr<model::IEditCommand>
    createAddCameraKeyframe(const std::string& id, int tick, std::optional<model::CameraKeyframe> captured);
    static std::unique_ptr<model::IEditCommand> createSaveCameraToKeyframe(
        const std::string&         id,
        int                        tick,
        model::CameraKeyframe      pose,
        std::optional<std::string> subject
    );
    static std::unique_ptr<model::IEditCommand>
    createMoveCameraKeyframe(const std::string& id, int fromTick, int toTick);
    static std::unique_ptr<model::IEditCommand> createDeleteCameraKeyframe(const std::string& id, int tick);
    static std::unique_ptr<model::IEditCommand>
    createSetKeyframeInterpolation(const std::string& id, int tick, model::CameraInterpolationType interpolation);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraKeyframePosition(const std::string& id, int tick, model::Vec3 position);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraKeyframeRotation(const std::string& id, int tick, model::Vec3 rotation);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraKeyframeSubject(const std::string& id, int tick, const std::string& subject);
    static std::unique_ptr<model::IEditCommand> createSetCameraEnabled(const std::string& id, bool enabled);
    static std::unique_ptr<model::IEditCommand> createSetCameraLocked(const std::string& id, bool locked);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraFollow(const std::string& id, model::FollowSettings const& follow);
    static std::unique_ptr<model::IEditCommand>
    createSetCameraProperties(const std::string& id, model::CameraProperties properties);
    static std::unique_ptr<model::IEditCommand> createRenameCamera(const std::string& id, const std::string& name);
    static std::unique_ptr<model::IEditCommand> createClearCameraKeyframes(const std::string& id);
    static std::unique_ptr<model::IEditCommand>
    createSetSubActorDetails(const std::string& id, model::AgentDetails details);

    static std::unique_ptr<model::IEditCommand> createMoveCameraKeyframes(std::vector<model::KeyframeRef> refs, int delta);
    static std::unique_ptr<model::IEditCommand> createDeleteCameraKeyframes(std::vector<model::KeyframeRef> refs);
    static std::unique_ptr<model::IEditCommand> createSetKeyframesInterpolation(
        std::vector<model::KeyframeRef>                  refs,
        model::CameraInterpolationType                   interpolation,
        std::optional<std::array<model::Vec2, 2>> const& bezier = std::nullopt
    );
    static std::unique_ptr<model::IEditCommand> createAddCameraKeyframes(std::vector<model::KeyframeRef> refs);

    static std::unique_ptr<model::IEditCommand>
    createAddMarker(const std::string& id, int tick, const std::string& label);
    static std::unique_ptr<model::IEditCommand> createDeleteMarker(const std::string& id);
    static std::unique_ptr<model::IEditCommand> createMoveMarker(const std::string& id, int tick);
    static std::unique_ptr<model::IEditCommand> createRenameMarker(const std::string& id, const std::string& label);

    static std::unique_ptr<model::IEditCommand> createSetWorldSettings(model::WorldSettings const& settings);
    static std::unique_ptr<model::IEditCommand> createSetPoseKeys(std::vector<model::PoseKey> keys);
    static std::unique_ptr<model::IEditCommand> createSetOverlays(std::vector<model::OverlayItem> overlays);
};

} // namespace playback::state::editing::command
