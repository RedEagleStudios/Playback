#pragma once

#include "playback/exporting/ExportTypes.h"
#include "playback/state/editing/models/CameraEntity.h"
#include "playback/state/editing/models/CameraKeyframe.h"
#include "playback/state/editing/models/FollowSettings.h"
#include "playback/state/editing/models/MathTypes.h"
#include "playback/state/editing/models/PoseClip.h"
#include "playback/state/editing/models/OverlayItem.h"
#include "playback/state/editing/models/ShakeSettings.h"
#include "playback/state/editing/models/WorldSettings.h"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace playback::state {

enum class EditorActionType {
    TogglePause,
    Seek,
    DecreaseSpeed,
    IncreaseSpeed,
    SetPlaybackSpeed,
    StopReplay,
    StartExport,
    CancelExport,
    OpenReplayBrowser,
    CloseReplayBrowser,
    RefreshReplayBrowser,
    OpenReplay,
    ImportReplay,
    DeleteReplays,
    RenameReplay,
    ShowReplayInFolder,
    OpenReplaysFolder,
    ClearReplayBrowserError,
    SaveProject,
    LoadProject,
    TeleportToRecordedPlayer,
    SetFollowRecordedPlayer,
    SetReplaySubjectChoice,
    SetFollowThirdPerson,
    UndoEditorEdit,
    RedoEditorEdit,
    // kind: 0 keyframe track, 1 first-person track, 2 third-person track.
    AddFreeCamera,
    SplitWorldActor,
    TrimWorldActor,
    // Cuts the segment (id) out of the replay layer: playback and export jump over the gap it leaves.
    DeleteWorldActorSegment,
    // Locks (value) every replay layer segment.
    SetWorldActorLocked,
    AddCameraKeyframe,
    // Writes the live camera into the key at tick, adding it if missing. A person track key also takes the player
    // the view follows, when it follows one.
    SaveCameraToKeyframe,
    MoveCameraKeyframe,
    DeleteCameraKeyframe,
    SetKeyframeInterpolation,
    SetKeyframePosition,
    SetKeyframeRotation,
    // A person track key's subject in secondaryId; empty is the recorder.
    SetKeyframeSubject,
    SetCameraEnabled,
    SetCameraLocked,
    SetCameraFollow,
    // A keyframe track's FOV and shake properties and their keys (cameraProperties), one undo step.
    SetCameraProperties,
    // While a camera property is dragged: shown live, not an undo step (id, cameraProperties; none ends it).
    PreviewCameraProperties,
    RenameCamera,
    ClearCameraKeyframes,
    DeleteCamera,
    UnbindCamera,
    CreateBindingCamera,
    SetSubActorDetails,
    SetPreviewCamera,
    ClearPreviewCamera,
    MoveViewToKeyframe,
    MoveCameraKeyframes,
    DeleteCameraKeyframes,
    SetKeyframesInterpolation,
    AddCameraKeyframes,
    AddMarker,
    DeleteMarker,
    MoveMarker,
    RenameMarker,
    SetWorldSettings,
    // While a world slider is dragged: shown live, not an undo step. Without worldSettings it ends the preview.
    PreviewWorldSettings,
    // The project's pose keyframes, replaced whole (poseKeys).
    SetPoseKeys,
    // The project's overlay layers, replaced whole (overlays).
    SetOverlays,
    // While a layer control is dragged: shown live, not an undo step. Without overlays it ends the preview.
    PreviewOverlays,
    // Extracts a recorded player's actions for recorded ticks [tick, kind) (id is the player's UUID, empty for the
    // recorder), then sets the overlays to `overlays`, which holds the new Stand-in layer: one undo step, or nothing
    // and EditorState::standInError when the player cannot be found there.
    HireStandIn,
};

struct EditorAction {
    EditorActionType                         type{};
    int                                      tick{};
    int                                      secondaryTick{};
    std::filesystem::path                    path;
    std::string                              replayId;
    std::string                              name;
    std::string                              id;
    std::string                              secondaryId;
    float                                    speed{};
    int                                      kind{};
    bool                                     value{};
    state::editing::model::Vec3              position{};
    std::optional<exporting::ExportSettings> exportSettings;
    std::vector<std::string>                 replayIds;
    // Set only when pasting a copied keyframe. An empty value captures the live camera, as before.
    std::optional<editing::model::CameraKeyframe> keyframe;
    std::map<std::string, std::string>       details;
    // Batch keyframe edits (multi-select move / delete / interpolation / paste).
    std::vector<editing::model::KeyframeRef> keyframeRefs;
    // SetWorldSettings: the project's new world settings. PreviewWorldSettings: the dragged value, or none to end it.
    std::optional<editing::model::WorldSettings> worldSettings;
    // SetPoseKeys: the project's new pose keyframes.
    std::optional<std::vector<editing::model::PoseKey>> poseKeys;
    // SetKeyframesInterpolation: Bézier control points to set with it (an easing preset); none keeps the keys' own.
    std::optional<std::array<editing::model::Vec2, 2>> bezierControls;
    // SetCameraFollow: the third-person track's new follow settings.
    std::optional<editing::model::FollowSettings> followSettings;
    // SetCameraProperties / PreviewCameraProperties: the keyframe track's new properties.
    std::optional<editing::model::CameraProperties> cameraProperties;
    // SetOverlays / PreviewOverlays: the project's new overlay layers.
    std::optional<std::vector<editing::model::OverlayItem>> overlays;
};

} // namespace playback::state
