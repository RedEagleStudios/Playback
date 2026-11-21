#include "EditorController.h"

#include "playback/Playback.h"
#include "playback/editor/graphics/BlurEffects.h"
#include "playback/editor/graphics/CameraRenderHooks.h"
#include "playback/editor/graphics/ColorGrading.h"
#include "playback/editor/graphics/FogOverride.h"
#include "playback/io/ReplayLibrary.h"
#include "playback/keyframe/CameraTimelineEvaluator.h"
#include "playback/keyframe/CameraTimelineRegistry.h"
#include "playback/keyframe/ClientCameraCapture.h"
#include "playback/replay/ReplaySession.h"
#include "playback/state/editing/CameraBindingOps.h"
#include "playback/state/editing/ProjectStore.h"
#include "playback/state/editing/RecordedCameraTracks.h"
#include "playback/state/editing/WorldActorOps.h"
#include "playback/state/editing/commands/CameraCommands.h"
#include "playback/state/editing/commands/CommandFactory.h"
#include "playback/visuals/FrameTap.h"

#include "ll/api/i18n/I18n.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <utility>

namespace playback::state {

namespace {

ReplayBrowserEntry makeBrowserEntry(io::ReplaySummary summary) {
    ReplayBrowserEntry entry;
    entry.path          = std::move(summary.path);
    entry.replayId      = std::move(summary.replayId);
    entry.replayName    = std::move(summary.replayName);
    entry.worldName     = std::move(summary.worldName);
    entry.durationTicks = summary.durationTicks;
    entry.totalTicks    = summary.totalTicks;
    entry.fileSize      = summary.fileSize;
    entry.lastModified  = summary.lastModified;
    entry.canOpen       = summary.canOpen;
    entry.problem       = std::move(summary.problem);
    return entry;
}

std::string replayPreferenceKey(std::filesystem::path const& path) {
    auto const utf8Path = path.lexically_normal().generic_u8string();
    return {reinterpret_cast<char const*>(utf8Path.data()), utf8Path.size()};
}

constexpr auto kAutosaveInterval = std::chrono::seconds(30);

auto& logger() { return Playback::getInstance().getSelf().getLogger(); }

// The global resource packs a replay's saved project asks for, read before the replay world loads.
std::optional<std::vector<state::editing::model::GlobalPack>> projectGlobalPacks(std::string_view replayPath) {
    auto const file = state::editing::ProjectStore::defaultProjectPath(replayPath);
    if (!state::editing::ProjectStore::exists(file)) return std::nullopt;
    state::editing::model::EditorStateExt project;
    std::string                           error;
    if (!state::editing::ProjectStore::load(file, project, error) || !project.world.overrideGlobalPacks) {
        return std::nullopt;
    }
    return std::move(project.world.globalPacks);
}

} // namespace

EditorController::EditorController(EditorContext& context)
: mContext(context),
  mBrowserSnapshot(std::make_shared<ReplayBrowserSnapshot>()),
  mExportDriver(
      std::make_unique<exporting::ReplayExportDriver>(mExportCoordinator, replay::ReplaySession::getInstance())
  ) {}

EditorController::~EditorController() = default;

void EditorController::setRendererAvailable(bool available) {
    if (mExportDriver) mExportDriver->setRendererAvailable(available);
}

void EditorController::publishCameraTimeline() {
    if (mProject.cameras.empty()) {
        keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Preview);
        return;
    }
    auto dimensionTransitionTicks = replay::ReplaySession::getInstance().getDimensionTransitionTicks();
    // A camera property being dragged is shown live without becoming an undo step.
    auto project = mProject;
    if (mCameraPreview) {
        auto const camera =
            std::ranges::find(project.cameras, mCameraPreview->first, &state::editing::model::CameraEntity::id);
        if (camera != project.cameras.end()) state::editing::model::setCameraProperties(*camera, mCameraPreview->second);
    }
    // No subject sampler: the replay session follows a person track's subject natively, like the user's follow.
    keyframe::publishCameraTimeline(
        keyframe::CameraTimelineSource::Preview,
        std::make_shared<keyframe::CameraTimelineEvaluator>(
            std::move(project),
            mPreviewCameraId,
            false,
            std::move(dimensionTransitionTicks)
        )
    );
}

std::optional<state::editing::model::CameraKeyframe> EditorController::captureCameraKeyframe() const {
    auto const captured = keyframe::captureClientCamera();
    if (!captured) return std::nullopt;
    auto const&                           state = *captured;
    state::editing::model::CameraKeyframe key;
    key.position = {state.x, state.y, state.z};
    key.yaw      = state.yaw;
    key.pitch    = state.pitch;
    key.roll     = state.roll;
    return key;
}

void EditorController::reset() {
    flushProjectOnClose();
    if (mExportDriver) mExportDriver->reset();
    else mExportCoordinator.reset();
    mBrowserVisible   = false;
    mBrowserOperation = ReplayBrowserOperation::None;
    mBrowserError.clear();
    mBrowserSnapshot = std::make_shared<ReplayBrowserSnapshot>();
    mProject         = {};
    mPreviewCameraId.reset();
    keyframe::clearCameraTimeline(keyframe::CameraTimelineSource::Preview);
    mCommandStack.clear();
    mActiveReplayPath.clear();
    mProjectFile.clear();
    mProjectError.clear();
    mSavedRevision                  = mCommandStack.revision();
    mEditorReadyLatched             = false;
    mProjectTotalTicks              = -1;
    mExportTickedBeforeClientUpdate = false;
}

void EditorController::tickExportBeforeClientUpdate() {
    mExportTickedBeforeClientUpdate = false;
    if (!mExportDriver || !mExportDriver->isActive()) return;
    mExportDriver->tick();
    mExportTickedBeforeClientUpdate = true;
}

void EditorController::tickExportDuringGraphics() {
    if (!mExportDriver || !mExportDriver->isActive()) return;
    // Advancing runs inside the graphics hook, which the driver itself can re-enter.
    if (mExportTickReentered) return;
    mExportTickReentered = true;
    mExportDriver->tick();
    mExportTickReentered = false;
}

void EditorController::ensureProject(int totalTicks, std::string_view replayPath) {
    totalTicks = std::max(0, totalTicks);
    if (mProjectTotalTicks == totalTicks && mProject.projectPath == replayPath) return;

    // Metadata settles a few frames in; growing the timeline must not discard the loaded project.
    if (mProject.projectPath == replayPath && !replayPath.empty()) {
        mProject.totalTicks            = totalTicks;
        mProject.worldActor.totalTicks = totalTicks;
        for (auto& segment : mProject.worldActor.segments) {
            if (segment.endTick == mProjectTotalTicks) segment.endTick = totalTicks;
        }
        mProjectTotalTicks = totalTicks;
        publishCameraTimeline();
        return;
    }

    mProject             = {};
    mProject.projectPath = std::string(replayPath);
    mProject.totalTicks  = totalTicks;
    state::editing::CameraBindingOps::addFreeCamera(mProject, {}, state::editing::model::CameraTrackKind::Keyframe);
    mProject.worldActor.segments.push_back({"worldActor", 0, totalTicks, 0});
    mCommandStack.clear();
    mPreviewCameraId.reset();
    mProjectTotalTicks = totalTicks;
    loadProjectForReplay(replayPath);
    mSavedRevision = mCommandStack.revision();
    mLastAutosave  = std::chrono::steady_clock::now();
    publishCameraTimeline();
}

void EditorController::addRecordedCameraTracks() {
    using namespace ll::i18n_literals;
    if (mProject.recordedCameraTracksAdded) return;
    auto const& samples = replay::ReplaySession::getInstance().getCommandCameraSamples();
    if (samples.empty()) return;

    std::vector<state::editing::RecordedCameraView> views;
    views.reserve(samples.size());
    for (auto const& sample : samples) views.push_back({sample.tickOffset, sample.view});
    int number = 0;
    for (auto& track : state::editing::recordedCameraTracks(views)) {
        auto const id = state::editing::CameraBindingOps::addFreeCamera(
            mProject,
            "playback.refactorEditor.defaults.recordedCamera"_tr(++number),
            state::editing::model::CameraTrackKind::Keyframe
        );
        auto const camera = std::ranges::find(mProject.cameras, id, &state::editing::model::CameraEntity::id);
        if (camera == mProject.cameras.end()) continue;
        camera->keysByTick = std::move(track.keys);
        // A FOV that never changes is the track's static value; one that changes keeps its keys.
        if (!track.fov.empty()) {
            float const first = track.fov.begin()->second.value;
            if (std::ranges::all_of(track.fov, [first](auto const& entry) { return entry.second.value == first; })) {
                camera->fov = first;
            } else {
                camera->channels[state::editing::model::CameraProperty::Fov] = std::move(track.fov);
            }
        }
    }
    mProject.recordedCameraTracksAdded = true;
    logger().info("Added {} recorded camera track(s) from {} views", number, samples.size());
    publishCameraTimeline();
}

void EditorController::loadProjectForReplay(std::string_view replayPath) {
    mProjectFile.clear();
    mProjectError.clear();
    if (replayPath.empty()) return;

    mProjectFile = state::editing::ProjectStore::defaultProjectPath(replayPath);
    if (!state::editing::ProjectStore::exists(mProjectFile)) return;

    state::editing::model::EditorStateExt loaded;
    std::string                           error;
    if (!state::editing::ProjectStore::load(mProjectFile, loaded, error)) {
        mProjectError = error;
        logger().warn("Unable to load editor project {}: {}", mProjectFile, error);
        return;
    }

    // The replay on disk is authoritative for length; a stale project must not shrink the timeline.
    loaded.projectPath           = mProject.projectPath;
    loaded.totalTicks            = mProject.totalTicks;
    loaded.worldActor.totalTicks = mProject.totalTicks;
    if (loaded.cameras.empty()) {
        state::editing::CameraBindingOps::addFreeCamera(loaded, {}, state::editing::model::CameraTrackKind::Keyframe);
    }
    if (loaded.worldActor.segments.empty()) {
        loaded.worldActor.segments.push_back({"worldActor", 0, mProject.totalTicks, 0});
    }
    mProject = std::move(loaded);
    logger().debug("Loaded editor project {}", mProjectFile);
}

bool EditorController::saveProject(std::filesystem::path const& path) {
    if (path.empty()) return false;

    std::string error;
    if (!state::editing::ProjectStore::save(mProject, path, error)) {
        mProjectError = error;
        logger().error("Unable to save editor project {}: {}", path, error);
        return false;
    }
    mProjectFile = path;
    mProjectError.clear();
    mSavedRevision = mCommandStack.revision();
    mLastAutosave  = std::chrono::steady_clock::now();
    logger().debug("Saved editor project {}", path);
    return true;
}

void EditorController::autosaveIfDue() {
    if (mProjectFile.empty() || !isProjectDirty()) return;
    if (mExportDriver && mExportDriver->isActive()) return;

    auto const now = std::chrono::steady_clock::now();
    if (now - mLastAutosave < kAutosaveInterval) return;
    mLastAutosave = now;
    (void)saveProject(mProjectFile);
}

void EditorController::flushProjectOnClose() {
    if (mProjectFile.empty() || !isProjectDirty()) return;
    (void)saveProject(mProjectFile);
}

void EditorController::applyEditorAction(EditorAction const& action) {
    using namespace state::editing::command;

    switch (action.type) {
    case EditorActionType::UndoEditorEdit:
        (void)mCommandStack.undo(mProject);
        break;
    case EditorActionType::RedoEditorEdit:
        (void)mCommandStack.redo(mProject);
        break;
    case EditorActionType::AddFreeCamera: {
        auto const kind = action.kind == 1 ? state::editing::model::CameraTrackKind::FirstPerson
                        : action.kind == 2 ? state::editing::model::CameraTrackKind::ThirdPerson
                                           : state::editing::model::CameraTrackKind::Keyframe;
        mCommandStack.push(CommandFactory::createAddFreeCamera(action.name, kind), mProject);
        break;
    }
    case EditorActionType::SplitWorldActor:
        mCommandStack.push(CommandFactory::createSplitWorldActor(action.tick), mProject);
        break;
    case EditorActionType::TrimWorldActor:
        mCommandStack.push(CommandFactory::createTrimWorldActor(action.id, action.tick, action.kind), mProject);
        break;
    case EditorActionType::DeleteWorldActorSegment:
        mCommandStack.push(CommandFactory::createDeleteWorldActorSegment(action.id), mProject);
        break;
    case EditorActionType::SetWorldActorLocked:
        mCommandStack.push(CommandFactory::createSetWorldActorLocked(action.value), mProject);
        break;
    case EditorActionType::AddCameraKeyframe:
        if (action.keyframe) {
            mCommandStack.push(
                CommandFactory::createAddCameraKeyframe(action.id, action.tick, action.keyframe),
                mProject
            );
            break;
        }
        if (auto captured = captureCameraKeyframe()) {
            logger().debug("Captured camera keyframe (camera={}, tick={})", action.id, action.tick);
            // A person track's first key views whoever the user follows; later keys keep the previous subject.
            captured->subject = replay::ReplaySession::getInstance().userReplaySubjectChoice();
            mCommandStack.push(
                CommandFactory::createAddCameraKeyframe(action.id, action.tick, std::move(captured)),
                mProject
            );
        } else {
            logger().debug(
                "Camera keyframe capture unavailable (camera={}, tick={}); using defaults",
                action.id,
                action.tick
            );
            mCommandStack.push(CommandFactory::createAddCameraKeyframe(action.id, action.tick), mProject);
        }
        break;
    case EditorActionType::SaveCameraToKeyframe: {
        auto captured = captureCameraKeyframe();
        if (!captured) {
            logger().debug("Camera keyframe capture unavailable (camera={}, tick={})", action.id, action.tick);
            break;
        }
        auto const&                session = replay::ReplaySession::getInstance();
        std::optional<std::string> subject;
        if (session.isFollowViewActive()) subject = session.replaySubjectChoice();
        mCommandStack.push(
            CommandFactory::createSaveCameraToKeyframe(action.id, action.tick, std::move(*captured), std::move(subject)),
            mProject
        );
        break;
    }
    case EditorActionType::MoveCameraKeyframe:
        mCommandStack.push(
            CommandFactory::createMoveCameraKeyframe(action.id, action.tick, action.secondaryTick),
            mProject
        );
        break;
    case EditorActionType::DeleteCameraKeyframe:
        mCommandStack.push(CommandFactory::createDeleteCameraKeyframe(action.id, action.tick), mProject);
        break;
    case EditorActionType::SetKeyframeInterpolation:
        mCommandStack.push(
            CommandFactory::createSetKeyframeInterpolation(
                action.id,
                action.tick,
                static_cast<state::editing::model::CameraInterpolationType>(action.kind)
            ),
            mProject
        );
        break;
    case EditorActionType::SetKeyframePosition:
        mCommandStack.push(
            CommandFactory::createSetCameraKeyframePosition(action.id, action.tick, action.position),
            mProject
        );
        break;
    case EditorActionType::SetKeyframeRotation:
        mCommandStack.push(
            CommandFactory::createSetCameraKeyframeRotation(action.id, action.tick, action.position),
            mProject
        );
        break;
    case EditorActionType::SetKeyframeSubject:
        mCommandStack.push(
            CommandFactory::createSetCameraKeyframeSubject(action.id, action.tick, action.secondaryId),
            mProject
        );
        break;
    case EditorActionType::SetCameraEnabled:
        mCommandStack.push(CommandFactory::createSetCameraEnabled(action.id, action.value), mProject);
        break;
    case EditorActionType::SetCameraLocked:
        mCommandStack.push(CommandFactory::createSetCameraLocked(action.id, action.value), mProject);
        break;
    case EditorActionType::SetCameraFollow:
        if (action.followSettings) {
            mCommandStack.push(CommandFactory::createSetCameraFollow(action.id, *action.followSettings), mProject);
        }
        break;
    case EditorActionType::SetCameraProperties:
        mCameraPreview.reset();
        if (action.cameraProperties) {
            mCommandStack.push(CommandFactory::createSetCameraProperties(action.id, *action.cameraProperties), mProject);
        }
        break;
    case EditorActionType::PreviewCameraProperties:
        mCameraPreview.reset();
        if (action.cameraProperties) mCameraPreview = std::pair{action.id, *action.cameraProperties};
        break;
    case EditorActionType::RenameCamera:
        mCommandStack.push(CommandFactory::createRenameCamera(action.id, action.name), mProject);
        break;
    case EditorActionType::ClearCameraKeyframes:
        mCommandStack.push(CommandFactory::createClearCameraKeyframes(action.id), mProject);
        break;
    case EditorActionType::DeleteCamera:
        mCommandStack.push(CommandFactory::createDeleteCamera(action.id), mProject);
        break;
    case EditorActionType::UnbindCamera:
        mCommandStack.push(CommandFactory::createUnbindCamera(action.id), mProject);
        break;
    case EditorActionType::CreateBindingCamera:
        mCommandStack.push(CommandFactory::createCreateBindingCamera(action.id, action.name), mProject);
        break;
    case EditorActionType::SetSubActorDetails:
        mCommandStack.push(CommandFactory::createSetSubActorDetails(action.id, action.details), mProject);
        break;
    case EditorActionType::MoveCameraKeyframes:
        mCommandStack.push(CommandFactory::createMoveCameraKeyframes(action.keyframeRefs, action.tick), mProject);
        break;
    case EditorActionType::DeleteCameraKeyframes:
        mCommandStack.push(CommandFactory::createDeleteCameraKeyframes(action.keyframeRefs), mProject);
        break;
    case EditorActionType::SetKeyframesInterpolation:
        mCommandStack.push(
            CommandFactory::createSetKeyframesInterpolation(
                action.keyframeRefs,
                static_cast<state::editing::model::CameraInterpolationType>(action.kind),
                action.bezierControls
            ),
            mProject
        );
        break;
    case EditorActionType::AddCameraKeyframes:
        mCommandStack.push(CommandFactory::createAddCameraKeyframes(action.keyframeRefs), mProject);
        break;
    case EditorActionType::AddMarker:
        mCommandStack.push(CommandFactory::createAddMarker(action.id, action.tick, action.name), mProject);
        break;
    case EditorActionType::DeleteMarker:
        mCommandStack.push(CommandFactory::createDeleteMarker(action.id), mProject);
        break;
    case EditorActionType::MoveMarker:
        mCommandStack.push(CommandFactory::createMoveMarker(action.id, action.tick), mProject);
        break;
    case EditorActionType::RenameMarker:
        mCommandStack.push(CommandFactory::createRenameMarker(action.id, action.name), mProject);
        break;
    case EditorActionType::SetWorldSettings:
        mWorldPreview.reset();
        if (action.worldSettings) {
            mCommandStack.push(CommandFactory::createSetWorldSettings(*action.worldSettings), mProject);
        }
        break;
    case EditorActionType::PreviewWorldSettings:
        mWorldPreview = action.worldSettings ? std::optional{action.worldSettings->sanitized()} : std::nullopt;
        break;
    case EditorActionType::SetPoseKeys:
        if (action.poseKeys) mCommandStack.push(CommandFactory::createSetPoseKeys(*action.poseKeys), mProject);
        break;
    case EditorActionType::SetOverlays:
        mOverlayPreview.reset();
        if (action.overlays) mCommandStack.push(CommandFactory::createSetOverlays(*action.overlays), mProject);
        break;
    case EditorActionType::PreviewOverlays:
        mOverlayPreview = action.overlays;
        break;
    case EditorActionType::HireStandIn: {
        // The actions are extracted first, so a range without the player leaves the project as it was.
        mOverlayPreview.reset();
        std::string error;
        if (!action.overlays) {
            error = "No Stand-in layer was given.";
        } else if (replay::ReplaySession::getInstance().prepareStandIn(action.id, action.tick, action.kind, error)) {
            mCommandStack.push(CommandFactory::createSetOverlays(*action.overlays), mProject);
        }
        mStandInError = std::move(error);
        ++mStandInHireSerial;
        break;
    }
    case EditorActionType::SetPreviewCamera:
        replay::ReplaySession::getInstance().setFollowRecordedPlayer(false);
        mPreviewCameraId.reset();
        if (std::ranges::any_of(mProject.cameras, [&](auto const& camera) {
                return camera.id == action.id && state::editing::model::isCameraRenderable(camera)
                    && camera.kind == state::editing::model::CameraTrackKind::Keyframe;
            })) {
            mPreviewCameraId = action.id;
        }
        break;
    case EditorActionType::ClearPreviewCamera:
        replay::ReplaySession::getInstance().setFollowRecordedPlayer(false);
        mPreviewCameraId.reset();
        break;
    case EditorActionType::MoveViewToKeyframe: {
        auto const camera = std::ranges::find(mProject.cameras, action.id, &state::editing::model::CameraEntity::id);
        if (camera == mProject.cameras.end()) break;
        auto const key = camera->keysByTick.find(action.tick);
        if (key == camera->keysByTick.end()) break;
        auto& session = replay::ReplaySession::getInstance();
        session.setFollowRecordedPlayer(false);
        mPreviewCameraId.reset();
        auto const& pose = key->second;
        session.requestParkObserver(
            keyframe::CameraRenderState{
                pose.position.x,
                pose.position.y,
                pose.position.z,
                pose.yaw,
                pose.pitch,
                pose.roll,
                state::editing::model::cameraPropertyAt(*camera, state::editing::model::CameraProperty::Fov, action.tick),
            }
        );
        break;
    }
    default:
        break;
    }

    if (mPreviewCameraId && !std::ranges::any_of(mProject.cameras, [&](auto const& camera) {
            return camera.id == *mPreviewCameraId && state::editing::model::isCameraRenderable(camera)
                && camera.kind == state::editing::model::CameraTrackKind::Keyframe;
        })) {
        mPreviewCameraId.reset();
    }
    publishCameraTimeline();
}

void EditorController::publishState(bool hudVisible) {
    auto& session = replay::ReplaySession::getInstance();

    bool const sessionActive = session.isActive();

    EditorState state;
    state.replayVisible = sessionActive && session.hasJoinedReplayWorld();
    // Gate only on the loading/progress screen clearing and let chunks stream in natively.
    if (!sessionActive) mEditorReadyLatched = false;
    else if (hudVisible) mEditorReadyLatched = true;
    state.editorVisible           = mEditorReadyLatched;
    state.hudVisible              = hudVisible;
    state.paused                  = session.isPaused();
    state.followingRecordedPlayer = session.isFollowingRecordedPlayer();
    state.followThirdPerson       = session.isFollowThirdPerson();
    state.replaySubjectChoice     = session.userReplaySubjectChoice();
    auto const replayPlayers      = session.replayPlayers();
    state.replayPlayers.reserve(replayPlayers.size());
    for (auto const& player : replayPlayers) {
        state.replayPlayers.push_back({player.uuid, player.name, player.recorder, player.present});
    }
    state.playbackSpeed = session.getPlaybackSpeed();
    state.currentTick             = std::max(0, session.getCurrentTick());
    state.totalTicks              = std::max(0, session.getTotalTicks());
    if (state.editorVisible != mEditorVisibleLogged) {
        mEditorVisibleLogged = state.editorVisible;
        logger().debug(
            "Replay editor {} (joined={}, worldReady={}, hud={}, totalTicks={})",
            state.editorVisible ? "shown" : "hidden",
            session.hasJoinedReplayWorld(),
            session.isReplayWorldReady(),
            hudVisible,
            state.totalTicks
        );
    }
    if (!sessionActive) {
        flushProjectOnClose();
        mActiveReplayPath.clear();
        mWorldPreview.reset();
        mOverlayPreview.reset();
        editor::graphics::setFogOverride(std::nullopt);
        editor::graphics::setFovOverride(std::nullopt);
        editor::graphics::setColorGrading(std::nullopt);
        editor::graphics::setBlurLayers({});
    }
    ensureProject(state.totalTicks, mActiveReplayPath);
    if (sessionActive) addRecordedCameraTracks();
    mProject.currentTick             = state.currentTick;
    mProject.playing                 = !state.paused;
    mProject.playbackSpeed           = state.playbackSpeed;
    if (sessionActive) {
        // Keyed world values take their value at the playhead, in preview and export alike.
        auto const world =
            state::editing::model::worldAt(mWorldPreview ? *mWorldPreview : mProject.world, state.currentTick);
        session.setWorldOverrides(
            world.overrideTime ? std::optional<int>{world.timeOfDay} : std::nullopt,
            world.freezeTime,
            world.hideClouds
        );
        editor::graphics::setFogOverride(
            world.overrideFog ? std::optional<editor::graphics::FogOverride>{{world.fogStart, world.fogEnd}}
                              : std::nullopt
        );
        editor::graphics::setFovOverride(world.overrideFov ? std::optional{world.fov} : std::nullopt);
        session.setWeatherOverride(world.overrideWeather ? std::optional{world.weather} : std::nullopt);
        editor::graphics::setColorGrading(
            world.overrideColor
                ? std::optional<editor::graphics::ColorGrading>{{world.exposure, world.temperature, world.tint}}
                : std::nullopt
        );
        session.setGraphicsOverride(world.overrideGraphics ? std::optional{world.graphics} : std::nullopt);
        session.setPoseClips(state::editing::model::poseClipsFrom(mProject.poseKeys, mProject.totalTicks));
        // The drafted layers while a control is held, so a Stand-in follows the edit as it is made.
        session.setStandIns(mOverlayPreview ? *mOverlayPreview : mProject.overlays);
        state.installedGlobalPacks = session.installedGlobalPacks();
    }
    state.project = std::make_shared<state::editing::model::EditorStateExt>(mProject);
    if (mOverlayPreview) {
        auto preview      = std::make_shared<state::editing::model::EditorStateExt>(mProject);
        preview->overlays = *mOverlayPreview;
        state.project     = std::move(preview);
    }
    // Blur layers act on the rendered picture, so the graphics side evaluates them per presented frame; drafts show
    // while a control is held.
    if (sessionActive) {
        editor::graphics::setBlurLayers(state.project->overlays);
        editor::graphics::setBlurFallbackTick(static_cast<double>(state.currentTick));
    }
    state.cameraTimeline             = keyframe::currentCameraTimeline(keyframe::CameraTimelineSource::Preview);
    if (state.cameraTimeline) {
        if (auto const shot = state.cameraTimeline->shotAt(state.currentTick)) state.activeCameraId = shot.camera->id;
    }
    state.canUndo                    = mCommandStack.canUndo();
    state.canRedo                    = mCommandStack.canRedo();
    state.persistence.dirty          = state.editorVisible && isProjectDirty();
    state.persistence.projectFile    = mProjectFile.empty() ? std::string{} : mProjectFile.filename().string();
    state.persistence.error          = mProjectError;
    state.capabilities.cameraEditing = state.editorVisible;
    state.capabilities.videoEditing  = state.editorVisible;
    state.capabilities.videoExport   = state.editorVisible && mExportDriver && mExportDriver->isAvailable();
    state.capabilities.ffmpegVideoExport =
        state.capabilities.videoExport
        && exporting::ExportCoordinator::isFormatAvailable(exporting::ExportFormat::Mp4Video);
    state.exportStatus      = mExportCoordinator.status();
    state.standInHireSerial = mStandInHireSerial;
    state.standInError      = mStandInError;
    // Blur areas are laid out in the export's frame while one runs; the plan is read once per export.
    bool const exporting = mExportDriver && mExportDriver->isActive();
    if (exporting && !mBlurExportAspectSet) {
        if (auto const plan = mExportCoordinator.plan()) {
            auto const& settings = plan->settings;
            editor::graphics::setBlurExportAspect(
                static_cast<float>(settings.resolutionX) / static_cast<float>(std::max<uint32_t>(1, settings.resolutionY))
            );
            mBlurExportAspectSet = true;
        }
    } else if (!exporting && mBlurExportAspectSet) {
        editor::graphics::setBlurExportAspect(std::nullopt);
        mBlurExportAspectSet = false;
    }
    state.browser.visible   = mBrowserVisible;
    state.browser.operation = mBrowserOperation;
    state.browser.error     = mBrowserError;
    state.browser.snapshot  = mBrowserSnapshot;
    mContext.publish(std::move(state));
}

void EditorController::refreshBrowser() {
    auto snapshot      = std::make_shared<ReplayBrowserSnapshot>();
    snapshot->revision = ++mBrowserRevision;
    auto replays       = io::ReplayLibrary::loadReplays();
    snapshot->replays.reserve(replays.size());
    for (auto& replay : replays) snapshot->replays.emplace_back(makeBrowserEntry(std::move(replay)));
    mBrowserSnapshot = std::move(snapshot);
}

ReplayBrowserEntry const* EditorController::findBrowserEntry(std::string_view replayId) const {
    if (!mBrowserSnapshot) return nullptr;
    auto const it = std::find_if(
        mBrowserSnapshot->replays.begin(),
        mBrowserSnapshot->replays.end(),
        [replayId](ReplayBrowserEntry const& entry) { return entry.replayId == replayId; }
    );
    return it == mBrowserSnapshot->replays.end() ? nullptr : &*it;
}

void EditorController::tick(bool hudVisible) {
    using namespace ll::i18n_literals;

    auto& session = replay::ReplaySession::getInstance();

    for (auto const& action : mContext.takeActions()) {
        if (mExportDriver && mExportDriver->isActive() && action.type != EditorActionType::CancelExport
            && action.type != EditorActionType::StopReplay) {
            continue;
        }
        if (action.type >= EditorActionType::UndoEditorEdit) {
            applyEditorAction(action);
            continue;
        }
        switch (action.type) {
        case EditorActionType::TogglePause:
            (void)session.setPaused(!session.isPaused());
            break;
        case EditorActionType::Seek:
            session.requestSeek(action.tick);
            break;
        case EditorActionType::DecreaseSpeed:
            session.adjustPlaybackSpeed(-1);
            break;
        case EditorActionType::IncreaseSpeed:
            session.adjustPlaybackSpeed(1);
            break;
        case EditorActionType::SetPlaybackSpeed:
            session.setPlaybackSpeed(action.speed);
            break;
        case EditorActionType::StopReplay:
            if (mExportDriver) mExportDriver->cancel();
            session.requestStop();
            break;
        case EditorActionType::StartExport: {
            auto settings = action.exportSettings.value_or(exporting::ExportSettings{});
            if (!action.exportSettings) {
                settings.startTick = 0;
                settings.endTick   = std::max<int64_t>(0, session.getTotalTicks());
            }
            if (mExportDriver && mExportDriver->start(std::move(settings), mProject, mPreviewCameraId)) {
                publishState(hudVisible);
            }
            break;
        }
        case EditorActionType::CancelExport:
            if (mExportDriver) mExportDriver->cancel();
            break;
        case EditorActionType::OpenReplayBrowser:
            if (!session.isActive()) {
                mBrowserVisible = true;
                mBrowserError.clear();
                runBrowserOperation(ReplayBrowserOperation::Refreshing, hudVisible, [this] { refreshBrowser(); });
            }
            break;
        case EditorActionType::CloseReplayBrowser:
            mBrowserVisible = false;
            mBrowserError.clear();
            break;
        case EditorActionType::RefreshReplayBrowser:
            runBrowserOperation(ReplayBrowserOperation::Refreshing, hudVisible, [this] {
                mBrowserError.clear();
                refreshBrowser();
            });
            break;
        case EditorActionType::OpenReplay:
            runBrowserOperation(ReplayBrowserOperation::OpeningReplay, hudVisible, [&] {
                auto replay = action.path.empty() ? io::ReplayLibrary::findReplay(action.replayId)
                                                  : io::ReplayLibrary::findReplay(action.path.string());
                if (!replay) {
                    mBrowserError = "playback.replayBrowser.error.fileNotFound"_tr();
                } else if (!replay->canOpen) {
                    mBrowserError =
                        replay->problem.empty() ? "playback.replayBrowser.error.invalidArchive"_tr() : replay->problem;
                } else if (
                    auto const key = replayPreferenceKey(replay->path);
                    !session.start(replay->path, projectGlobalPacks(key))
                ) {
                    mBrowserError = "playback.replayBrowser.error.openFailed"_tr();
                } else {
                    mActiveReplayPath = key;
                    mBrowserVisible   = false;
                    mBrowserError.clear();
                }
            });
            break;
        case EditorActionType::ImportReplay:
            runBrowserOperation(ReplayBrowserOperation::ImportingReplay, hudVisible, [&] {
                if (io::ReplayLibrary::importReplay(action.path, mBrowserError)) refreshBrowser();
            });
            break;
        case EditorActionType::DeleteReplays:
            runBrowserOperation(ReplayBrowserOperation::DeletingReplay, hudVisible, [&] {
                mBrowserError.clear();
                bool changed = false;
                for (auto const& replayId : action.replayIds) {
                    auto const* entry = findBrowserEntry(replayId);
                    if (!entry) {
                        mBrowserError = "playback.replayBrowser.error.fileNotFound"_tr();
                        break;
                    }
                    auto replay = io::ReplayLibrary::findReplay(entry->path.string());
                    if (!replay || !io::ReplayLibrary::deleteReplay(*replay, mBrowserError)) break;
                    changed = true;
                }
                if (changed) refreshBrowser();
            });
            break;
        case EditorActionType::RenameReplay:
            runBrowserOperation(ReplayBrowserOperation::RenamingReplay, hudVisible, [&] {
                auto const* entry  = findBrowserEntry(action.replayId);
                auto        replay = entry ? io::ReplayLibrary::findReplay(entry->path.string()) : std::nullopt;
                if (!replay) {
                    mBrowserError = "playback.replayBrowser.error.fileNotFound"_tr();
                } else if (io::ReplayLibrary::renameReplay(*replay, action.name, mBrowserError)) {
                    refreshBrowser();
                }
            });
            break;
        case EditorActionType::ShowReplayInFolder:
            runBrowserOperation(ReplayBrowserOperation::ShowingInFolder, hudVisible, [&] {
                auto const* entry  = findBrowserEntry(action.replayId);
                auto        replay = entry ? io::ReplayLibrary::findReplay(entry->path.string()) : std::nullopt;
                if (!replay) {
                    mBrowserError = "playback.replayBrowser.error.fileNotFound"_tr();
                } else if (!io::ReplayLibrary::showInFolder(*replay)) {
                    mBrowserError = "playback.replayBrowser.error.showInFolderFailed"_tr();
                } else {
                    mBrowserError.clear();
                }
            });
            break;
        case EditorActionType::OpenReplaysFolder:
            runBrowserOperation(ReplayBrowserOperation::ShowingInFolder, hudVisible, [&] {
                if (io::ReplayLibrary::openReplaysFolder()) {
                    mBrowserError.clear();
                } else {
                    mBrowserError = "playback.replayBrowser.error.openFolderFailed"_tr();
                }
            });
            break;
        case EditorActionType::ClearReplayBrowserError:
            mBrowserError.clear();
            break;
        case EditorActionType::SaveProject: {
            auto target = action.path.empty() ? mProjectFile : action.path;
            if (target.empty()) target = state::editing::ProjectStore::defaultProjectPath(mProject.projectPath);
            if (!target.empty()) (void)saveProject(target);
            break;
        }
        case EditorActionType::TeleportToRecordedPlayer:
            session.requestTeleportToRecordedPlayer();
            break;
        case EditorActionType::SetFollowRecordedPlayer:
            if (action.value) session.setReplaySubjectChoice(action.id);
            session.setFollowRecordedPlayer(action.value);
            break;
        case EditorActionType::SetFollowThirdPerson:
            session.setFollowThirdPerson(action.value);
            break;
        case EditorActionType::SetReplaySubjectChoice: {
            bool legal = action.id.empty();
            if (!legal) {
                for (auto const& player : session.replayPlayers()) {
                    if (player.uuid == action.id && player.present) {
                        legal = true;
                        break;
                    }
                }
            }
            if (!legal) {
                logger().debug("replay subject ignored uuid={}", action.id);
                break;
            }
            session.setReplaySubjectChoice(action.id);
            break;
        }
        case EditorActionType::LoadProject: {
            auto const target = action.path.empty() ? mProjectFile : action.path;
            if (target.empty()) break;
            state::editing::model::EditorStateExt loaded;
            std::string                           error;
            if (!state::editing::ProjectStore::load(target, loaded, error)) {
                mProjectError = error;
                break;
            }
            loaded.projectPath           = mProject.projectPath;
            loaded.totalTicks            = mProject.totalTicks;
            loaded.worldActor.totalTicks = mProject.totalTicks;
            mProject                     = std::move(loaded);
            mProjectFile                 = target;
            mProjectError.clear();
            mCommandStack.clear();
            mSavedRevision = mCommandStack.revision();
            mPreviewCameraId.reset();
            publishCameraTimeline();
            break;
        }
        }
    }

    skipCutReplayGaps();
    if (mExportDriver && !mExportTickedBeforeClientUpdate) mExportDriver->tick();
    mExportTickedBeforeClientUpdate = false;
    autosaveIfDue();
    publishState(hudVisible);
}

// Cut stretches of the replay layer do not play: reaching a gap jumps to the next segment, and past the last one the
// replay pauses. A paused playhead may still sit in a gap, so the user can see what was cut. A seek pauses the
// replay, so playback resumes once the jump has landed.
void EditorController::skipCutReplayGaps() {
    auto& session = replay::ReplaySession::getInstance();
    if (!session.isActive() || (mExportDriver && mExportDriver->isActive())) {
        mResumeAfterCutJump = false;
        return;
    }
    if (session.isSeekPending()) return;
    if (mResumeAfterCutJump) {
        mResumeAfterCutJump = false;
        if (session.isPaused()) (void)session.setPaused(false);
        return;
    }
    if (session.isPaused()) return;
    int const tick = session.getCurrentTick();
    if (tick < 0 || tick >= mProject.totalTicks) return;
    auto const next = state::editing::WorldActorOps::nextPlayableTick(mProject.worldActor, tick);
    if (!next) {
        (void)session.setPaused(true);
        return;
    }
    if (*next > tick) {
        session.requestSeek(*next);
        mResumeAfterCutJump = true;
    }
}

} // namespace playback::state
