#include "TimelinePanel.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/EasingPresetPicker.h"
#include "playback/editor/ui/components/KeyframeControls.h"
#include "playback/editor/ui/components/PlayerPicker.h"
#include "playback/editor/ui/components/OverlayEditing.h"
#include "playback/editor/ui/components/TimelineScale.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/replay/ReplaySession.h"
#include "playback/state/editing/commands/MarkerCommands.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <functional>
#include <set>
#include <limits>
#include <ranges>

namespace playback::editor::ui {
using namespace playback::state;
using state::editing::model::findEasingPreset;

using namespace ll::i18n_literals;

namespace {

constexpr float kMinZoomScale   = 1.0f;
constexpr float kMaxZoomScale   = 20.0f;
constexpr float kZoomStep       = 1.15f;
constexpr int   kTicksPerSecond = widgets::kTicksPerSecond;
// Shift-snap reach in screen pixels, so it feels the same at every zoom level.
constexpr float kSnapPixelRadius = 10.0f;
constexpr float kMarkerHalfWidth = 5.0f;
constexpr float kBoxDragThreshold = 4.0f;
// Queued actions run on the controller's next tick; give up on a pending selection after this many frames.
constexpr int kPendingSelectionFrames = 30;

using widgets::formatTick;
using widgets::formatTickCompact;
using widgets::iconButton;
using widgets::iconButtonSize;
using widgets::iconToggle;

int majorTickStep(float pixelsPerTick, float minimumSpacing) {
    constexpr int steps[] = {
        kTicksPerSecond,
        kTicksPerSecond * 2,
        kTicksPerSecond * 5,
        kTicksPerSecond * 10,
        kTicksPerSecond * 20,
        kTicksPerSecond * 30,
        kTicksPerSecond * 60,
        kTicksPerSecond * 120,
        kTicksPerSecond * 300,
        kTicksPerSecond * 600,
    };
    for (int step : steps)
        if (static_cast<float>(step) * pixelsPerTick >= minimumSpacing) return step;
    return steps[std::size(steps) - 1];
}

bool contains(ImVec2 const& minimum, ImVec2 const& maximum, ImVec2 const& point) {
    return point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y && point.y <= maximum.y;
}

float maxScrollFor(float canvasWidth, float zoom, int totalTicks) {
    float const fit = canvasWidth / static_cast<float>(std::max(1, totalTicks));
    return std::max(0.0f, static_cast<float>(totalTicks) * fit * zoom - canvasWidth);
}

void drawKeyframeDiamond(ImDrawList* drawList, float x, float centerY, float radius, ImU32 fill, ImU32 outline) {
    ImVec2 const top{x, centerY - radius};
    ImVec2 const right{x + radius, centerY};
    ImVec2 const bottom{x, centerY + radius};
    ImVec2 const left{x - radius, centerY};
    drawList->AddQuadFilled(top, right, bottom, left, fill);
    drawList->AddQuad(top, right, bottom, left, outline, 1.0f);
}

void drawKeyframeSquare(ImDrawList* drawList, float x, float centerY, float radius, ImU32 fill, ImU32 outline) {
    float const h = radius * 0.8f;
    drawList->AddRectFilled({x - h, centerY - h}, {x + h, centerY + h}, fill);
    drawList->AddRect({x - h, centerY - h}, {x + h, centerY + h}, outline, 0.0f, 0, 1.0f);
}

ImU32 layerColor(state::editing::model::OverlayKind kind) {
    switch (kind) {
    case state::editing::model::OverlayKind::Title:
        return theme::kLayerTitle;
    case state::editing::model::OverlayKind::Shape:
        return theme::kLayerShape;
    case state::editing::model::OverlayKind::Model:
        return theme::kLayerModel;
    case state::editing::model::OverlayKind::Blur:
        return theme::kLayerBlur;
    }
    return theme::kLayerTitle;
}

// A Property row's keys and the timeline tick they count from (a layer's keys count from its start).
struct RowChannel {
    state::editing::model::ValueChannel const* channel{};
    int                                        offset{};
};

// The channels a Property row edits: the camera shake's five as one, else its own.
std::vector<std::string> rowMembers(TrackTreeRow const& row) {
    if (row.owner == PropertyOwner::Camera && row.property == state::editing::model::CameraProperty::Shake) {
        auto const& members = state::editing::model::CameraProperty::ShakeMembers;
        return {members.begin(), members.end()};
    }
    return {row.property};
}

RowChannel rowChannel(state::editing::model::EditorStateExt const& project, TrackTreeRow const& row) {
    state::editing::model::ValueChannels const* channels = nullptr;
    int                                         offset   = 0;
    switch (row.owner) {
    case PropertyOwner::Camera:
        if (auto const camera = std::ranges::find(project.cameras, row.ownerId, &state::editing::model::CameraEntity::id);
            camera != project.cameras.end()) {
            channels = &camera->channels;
        }
        break;
    case PropertyOwner::Overlay:
        if (auto const* item = overlays::find(project, row.ownerId)) {
            channels = &item->channels;
            offset   = item->startTick;
        }
        break;
    case PropertyOwner::World:
        channels = &project.world.channels;
        break;
    }
    if (!channels) return {};
    // The shake row draws its strength (the first of its channels there is); its keys sit at the same ticks in all.
    for (auto const& name : rowMembers(row)) {
        auto const channel = channels->find(name);
        if (channel != channels->end() && !channel->second.empty()) return {&channel->second, offset};
    }
    return {};
}

// Every keyed tick of a set of channels, on the timeline.
std::set<int> keyTicks(state::editing::model::ValueChannels const& channels, int offset) {
    std::set<int> ticks;
    for (auto const& [name, channel] : channels) {
        for (auto const& [tick, key] : channel) ticks.insert(tick + offset);
    }
    return ticks;
}

// Pixels either side of a bar's edge that grab the edge for a trim rather than the bar.
constexpr float kEdgeGrabPixels = 5.0f;

// A layer or segment bar on a lane, with its label clipped to the bar.
void drawLaneBar(
    ImDrawList*  drawList,
    ImVec2       min,
    ImVec2       max,
    ImU32        color,
    bool         selected,
    bool         dimmed,
    char const*  icon,
    char const*  label,
    float        clipLeft,
    float        clipRight
) {
    if (max.x < clipLeft || min.x > clipRight) return;
    float const rounding = 3.0f * metrics::scale();
    drawList->AddRectFilled(min, max, theme::withAlpha(color, dimmed ? 0x38 : selected ? 0xc8 : 0x8c), rounding);
    drawList->AddRect(
        min,
        max,
        selected ? theme::kKeyframeSelected : theme::withAlpha(color, 0xff),
        rounding,
        0,
        selected ? 2.0f : 1.0f
    );
    float const  pad = 4.0f * metrics::scale();
    ImVec4 const clip{std::max(min.x, clipLeft) + pad, min.y, std::min(max.x, clipRight) - pad, max.y};
    if (clip.z <= clip.x) return;
    float const centreY = (min.y + max.y) * 0.5f;
    float const glyph   = std::floor(metrics::font());
    float       textX   = clip.x;
    if (icon && clip.z - clip.x > glyph) {
        widgets::drawIconAtCentre(drawList, icon, {textX + glyph * 0.5f, centreY}, theme::kText, glyph);
        textX += glyph + pad;
    }
    if (label && *label) {
        float const textY = widgets::textYForCentre(centreY, label);
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {textX, textY}, theme::kText, label, nullptr, 0.0f, &clip);
    }
}

} // namespace

void TimelinePanel::setViewPreferences(float trackListWidthRatio, float zoomScale, float horizontalScroll) {
    mTrackListWidthRatio = std::clamp(trackListWidthRatio, 0.18f, 0.55f);
    mZoomScale           = std::clamp(zoomScale, kMinZoomScale, kMaxZoomScale);
    mScrollX             = std::max(0.0f, horizontalScroll);
    mScrollY             = 0.0f;
    mPendingSeekTick     = -1;
    mRulerDragTick       = -1;
    mAnimator.clear();
    mDraggingKeyframeCameraId.clear();
    mDraggingKeyframeStartTick = -1;
    mDraggingKeyframeMoved     = false;
    mRangeDragMode             = 0;
}

void TimelinePanel::submitSeek(PanelContext const& ctx, int tick) {
    mPendingSeekTick = std::clamp(tick, 0, std::max(0, ctx.state.totalTicks));
    EditorAction action{EditorActionType::Seek};
    action.tick = mPendingSeekTick;
    ctx.submitAction(std::move(action));
}

void TimelinePanel::seekTo(PanelContext const& ctx, int tick) { submitSeek(ctx, tick); }

void TimelinePanel::seekRelative(PanelContext const& ctx, int tickDelta) {
    int const baseTick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    submitSeek(ctx, baseTick + tickDelta);
}

void TimelinePanel::seekAdjacentEditPoint(PanelContext const& ctx, bool forward) {
    auto const& state   = ctx.state;
    auto const  project = state.project;
    if (!project) return;

    int const  baseTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;
    int        target   = forward ? state.totalTicks : 0;
    bool       found    = false;
    auto const consider = [&](int tick) {
        if ((forward && tick <= baseTick) || (!forward && tick >= baseTick)) return;
        if (!found || (forward ? tick < target : tick > target)) {
            target = tick;
            found  = true;
        }
    };

    for (auto const& segment : project->worldActor.segments) {
        consider(segment.startTick);
        consider(segment.endTick);
    }
    for (auto const& camera : project->cameras) {
        for (auto const& [tick, _] : camera.keysByTick) consider(tick);
    }
    for (auto const& marker : project->markers) consider(marker.tick);
    submitSeek(ctx, target);
}

bool TimelinePanel::addKeyframeAtPlayhead(PanelContext const& ctx) {
    std::string cameraId = resolveKeyframeTargetCamera(ctx);
    if (cameraId.empty()) return false;

    EditorAction action{EditorActionType::AddCameraKeyframe};
    action.id   = cameraId;
    action.tick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    ctx.submitAction(std::move(action));
    ctx.selection.select(state::editing::model::SelectedCamera{std::move(cameraId)});
    return true;
}

// Resolves a target track so the shortcut works without a prior selection.
std::string TimelinePanel::resolveKeyframeTargetCamera(PanelContext const& ctx) const {
    auto const project = ctx.state.project;
    if (!project) return {};

    auto const isUsable = [&](std::string const& id) {
        return std::ranges::any_of(project->cameras, [&](auto const& camera) {
            return camera.id == id && !camera.locked;
        });
    };

    if (auto const* selected = ctx.selection.getAs<state::editing::model::SelectedCamera>();
        selected && isUsable(selected->cameraId)) {
        return selected->cameraId;
    }
    if (auto const* selected = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
        selected && isUsable(selected->trackId)) {
        return selected->trackId;
    }
    for (auto const& camera : project->cameras) {
        if (camera.enabled && !camera.locked) return camera.id;
    }
    for (auto const& camera : project->cameras) {
        if (!camera.locked) return camera.id;
    }
    return {};
}

namespace {

std::vector<state::editing::model::KeyframeRef> selectedKeyframeRefs(PanelContext const& ctx) {
    std::vector<state::editing::model::KeyframeRef> refs;
    refs.reserve(ctx.selection.keyframeCount());
    for (auto const& key : ctx.selection.keyframes()) refs.push_back({key.trackId, key.tick, std::nullopt});
    return refs;
}

} // namespace

bool TimelinePanel::deleteSelection(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    if (!project) return false;

    EditorAction action;
    if (ctx.selection.keyframeCount() > 1) {
        action.type         = EditorActionType::DeleteCameraKeyframes;
        action.keyframeRefs = selectedKeyframeRefs(ctx);
    } else if (auto const* selectedMarker = ctx.selection.getAs<state::editing::model::SelectedMarker>()) {
        action.type = EditorActionType::DeleteMarker;
        action.id   = selectedMarker->markerId;
    } else if (auto const* selectedPose = ctx.selection.getAs<state::editing::model::SelectedPoseKey>()) {
        action.type     = EditorActionType::SetPoseKeys;
        action.poseKeys = project->poseKeys;
        std::erase_if(*action.poseKeys, [&](auto const& key) { return key.id == selectedPose->poseKeyId; });
    } else if (auto const* selectedOverlay = ctx.selection.getAs<state::editing::model::SelectedOverlay>()) {
        auto const* item = overlays::find(*project, selectedOverlay->overlayId);
        if (!item || item->locked) return false;
        action.type     = EditorActionType::SetOverlays;
        action.overlays = project->overlays;
        std::erase_if(*action.overlays, [&](auto const& layer) { return layer.id == selectedOverlay->overlayId; });
    } else if (auto const* selectedSegment = ctx.selection.getAs<state::editing::model::SelectedWorldActorSegment>()) {
        action.type = EditorActionType::DeleteWorldActorSegment;
        action.id   = selectedSegment->segmentId;
    } else if (auto const* selectedCamera = ctx.selection.getAs<state::editing::model::SelectedCamera>();
               selectedCamera && project->cameras.size() > 1) {
        action.type = EditorActionType::DeleteCamera;
        action.id   = selectedCamera->cameraId;
    } else if (auto const* selectedKeyframe = ctx.selection.getAs<state::editing::model::SelectedKeyframe>()) {
        action.type = EditorActionType::DeleteCameraKeyframe;
        action.id   = selectedKeyframe->trackId;
        action.tick = selectedKeyframe->tick;
    } else {
        return false;
    }

    ctx.submitAction(std::move(action));
    ctx.selection.clear();
    return true;
}

bool TimelinePanel::selectAllKeyframes(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    if (!project) return false;
    // Locked tracks stay out so a group move is never rejected because of them.
    std::vector<state::editing::model::SelectedKeyframe> keys;
    for (auto const& camera : project->cameras) {
        if (camera.locked) continue;
        for (auto const& [tick, _] : camera.keysByTick) keys.push_back({camera.id, tick});
    }
    if (keys.empty()) return false;
    ctx.selection.setKeyframes(std::move(keys));
    return true;
}

bool TimelinePanel::copySelection(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    if (!project || ctx.selection.keyframeCount() == 0) return false;

    std::vector<ClipboardKey> clip;
    int                       firstTick = std::numeric_limits<int>::max();
    for (auto const& selected : ctx.selection.keyframes()) {
        auto const camera = std::ranges::find(project->cameras, selected.trackId, &state::editing::model::CameraEntity::id);
        if (camera == project->cameras.end()) continue;
        auto const key = camera->keysByTick.find(selected.tick);
        if (key == camera->keysByTick.end()) continue;
        clip.push_back({camera->id, key->first, key->second});
        firstTick = std::min(firstTick, key->first);
    }
    if (clip.empty()) return false;
    for (auto& entry : clip) entry.offset -= firstTick;
    mKeyframeClipboard = std::move(clip);
    return true;
}

bool TimelinePanel::pasteAtPlayhead(PanelContext const& ctx) {
    int const tick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    return pasteKeyframes(ctx, tick, resolveKeyframeTargetCamera(ctx));
}

bool TimelinePanel::pasteKeyframes(PanelContext const& ctx, int anchorTick, std::string const& targetCameraId) {
    auto const project = ctx.state.project;
    if (!project || mKeyframeClipboard.empty()) return false;

    bool const singleCamera = std::ranges::all_of(mKeyframeClipboard, [&](ClipboardKey const& entry) {
        return entry.cameraId == mKeyframeClipboard.front().cameraId;
    });
    std::vector<state::editing::model::KeyframeRef>      refs;
    std::vector<state::editing::model::SelectedKeyframe> landed;
    for (auto const& entry : mKeyframeClipboard) {
        std::string const& cameraId = singleCamera && !targetCameraId.empty() ? targetCameraId : entry.cameraId;
        int const          tick     = anchorTick + entry.offset;
        if (tick < 0 || tick > ctx.state.totalTicks) continue;
        auto const camera = std::ranges::find(project->cameras, cameraId, &state::editing::model::CameraEntity::id);
        if (camera == project->cameras.end() || camera->locked || camera->keysByTick.contains(tick)) continue;
        refs.push_back({cameraId, tick, entry.pose});
        landed.push_back({cameraId, tick});
    }
    if (refs.empty()) return false;

    EditorAction action{EditorActionType::AddCameraKeyframes};
    action.keyframeRefs = std::move(refs);
    ctx.submitAction(std::move(action));
    queueKeyframeSelection(std::move(landed));
    return true;
}

bool TimelinePanel::addMarkerAtPlayhead(PanelContext const& ctx) {
    return addMarkerAt(ctx, mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick);
}

bool TimelinePanel::addMarkerAt(PanelContext const& ctx, int tick) {
    auto const project = ctx.state.project;
    if (!project) return false;
    tick = std::clamp(tick, 0, std::max(0, ctx.state.totalTicks));
    // One marker per tick: pressing M on an existing one just selects it.
    if (auto const existing = std::ranges::find(project->markers, tick, &state::editing::model::Marker::tick);
        existing != project->markers.end()) {
        ctx.selection.select(state::editing::model::SelectedMarker{existing->id});
        return false;
    }
    std::string id = state::editing::command::nextMarkerId(*project);
    EditorAction action{EditorActionType::AddMarker};
    action.id   = id;
    action.tick = tick;
    action.name = "playback.refactorEditor.defaults.marker"_tr(project->markers.size() + 1);
    ctx.submitAction(std::move(action));
    queueMarkerSelection(std::move(id));
    return true;
}

void TimelinePanel::seekAdjacentMarker(PanelContext const& ctx, bool forward) {
    auto const project = ctx.state.project;
    if (!project) return;
    int const baseTick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    int       target   = -1;
    for (auto const& marker : project->markers) {
        if (forward ? marker.tick <= baseTick : marker.tick >= baseTick) continue;
        if (target < 0 || (forward ? marker.tick < target : marker.tick > target)) target = marker.tick;
    }
    if (target >= 0) submitSeek(ctx, target);
}

int TimelinePanel::snapToEditPoints(
    PanelContext const&                                 ctx,
    int                                                 tick,
    float                                               pixelsPerTick,
    bool                                                includePlayhead,
    bool                                                includeExportRange,
    std::function<bool(std::string const&, int)> const& skipKey,
    std::string_view                                    skipMarkerId
) {
    auto const project = ctx.state.project;
    if (!project || pixelsPerTick <= 0.0f) return tick;

    float const radius   = kSnapPixelRadius * metrics::scale();
    float       bestDist = std::numeric_limits<float>::max();
    int         best     = -1;
    auto const  consider = [&](int candidate) {
        if (candidate < 0) return;
        float const distance = static_cast<float>(std::abs(candidate - tick)) * pixelsPerTick;
        if (distance <= radius && distance < bestDist) {
            bestDist = distance;
            best     = candidate;
        }
    };
    for (auto const& camera : project->cameras) {
        for (auto const& [keyTick, _] : camera.keysByTick) {
            if (skipKey && skipKey(camera.id, keyTick)) continue;
            consider(keyTick);
        }
    }
    for (auto const& marker : project->markers) {
        if (marker.id != skipMarkerId) consider(marker.tick);
    }
    if (includePlayhead) consider(mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick);
    if (includeExportRange) {
        consider(ctx.commands.exportStartTick());
        consider(ctx.commands.exportEndTick());
    }
    if (best < 0) return tick;
    mSnapGuideTick = best;
    return best;
}

void TimelinePanel::queueKeyframeSelection(std::vector<state::editing::model::SelectedKeyframe> keys) {
    mPendingKeySelection = std::move(keys);
    mPendingMarkerSelection.clear();
    mPendingPoseKeySelection.clear();
    mPendingOverlaySelection.clear();
    mPendingSelectionFrames = kPendingSelectionFrames;
}

void TimelinePanel::queueMarkerSelection(std::string markerId) {
    mPendingKeySelection.clear();
    mPendingMarkerSelection = std::move(markerId);
    mPendingPoseKeySelection.clear();
    mPendingOverlaySelection.clear();
    mPendingSelectionFrames = kPendingSelectionFrames;
}

void TimelinePanel::queuePoseKeySelection(std::string poseKeyId) {
    mPendingKeySelection.clear();
    mPendingMarkerSelection.clear();
    mPendingPoseKeySelection = std::move(poseKeyId);
    mPendingOverlaySelection.clear();
    mPendingSelectionFrames  = kPendingSelectionFrames;
}

void TimelinePanel::queueOverlaySelection(std::string overlayId) {
    mPendingKeySelection.clear();
    mPendingMarkerSelection.clear();
    mPendingPoseKeySelection.clear();
    mPendingOverlaySelection = std::move(overlayId);
    mPendingSelectionFrames  = kPendingSelectionFrames;
}

void TimelinePanel::applyPendingSelection(PanelContext const& ctx) {
    if (mPendingSelectionFrames <= 0) return;
    auto const project = ctx.state.project;
    if (!project) return;
    bool const lastChance = --mPendingSelectionFrames == 0;

    bool done = false;
    if (!mPendingOverlaySelection.empty()) {
        if (overlays::find(*project, mPendingOverlaySelection)) {
            ctx.selection.select(state::editing::model::SelectedOverlay{mPendingOverlaySelection});
            done = true;
        }
    } else if (!mPendingPoseKeySelection.empty()) {
        if (std::ranges::any_of(project->poseKeys, [&](auto const& key) { return key.id == mPendingPoseKeySelection; })) {
            ctx.selection.select(state::editing::model::SelectedPoseKey{mPendingPoseKeySelection});
            done = true;
        }
    } else if (!mPendingMarkerSelection.empty()) {
        if (std::ranges::any_of(project->markers, [&](auto const& marker) { return marker.id == mPendingMarkerSelection; })) {
            ctx.selection.select(state::editing::model::SelectedMarker{mPendingMarkerSelection});
            done = true;
        }
    } else {
        std::vector<state::editing::model::SelectedKeyframe> present;
        for (auto const& key : mPendingKeySelection) {
            auto const camera = std::ranges::find(project->cameras, key.trackId, &state::editing::model::CameraEntity::id);
            if (camera != project->cameras.end() && camera->keysByTick.contains(key.tick)) present.push_back(key);
        }
        // Wait until every key exists; on the last frame settle for whatever made it.
        if (!present.empty() && (present.size() == mPendingKeySelection.size() || lastChance)) {
            ctx.selection.setKeyframes(std::move(present));
            done = true;
        }
    }
    if (done || lastChance) {
        mPendingKeySelection.clear();
        mPendingMarkerSelection.clear();
        mPendingPoseKeySelection.clear();
        mPendingOverlaySelection.clear();
        mPendingSelectionFrames = 0;
    }
}

void TimelinePanel::addPoseKeyAt(
    PanelContext const& ctx,
    std::string const&  subject,
    std::string const&  label,
    int                 tick,
    std::string const&  animation
) {
    using state::editing::model::PoseKey;
    auto const project = ctx.state.project;
    if (!project || subject.empty()) return;
    auto keys = project->poseKeys;
    // One key per entity and tick: adding where one stands replaces its pose.
    auto const existing = std::ranges::find_if(keys, [&](PoseKey const& key) {
        return key.subject == subject && key.tick == tick;
    });
    std::string id;
    if (existing != keys.end()) {
        id                  = existing->id;
        existing->animation = animation;
    } else {
        for (size_t number = keys.size() + 1;; ++number) {
            id = "pose-" + std::to_string(number);
            if (std::ranges::find(keys, id, &PoseKey::id) == keys.end()) break;
        }
        PoseKey key;
        key.id           = id;
        key.subject      = subject;
        key.subjectLabel = label;
        key.tick         = std::clamp(tick, 0, ctx.state.totalTicks);
        key.animation    = animation;
        keys.push_back(std::move(key));
    }
    EditorAction action{EditorActionType::SetPoseKeys};
    action.poseKeys = std::move(keys);
    ctx.submitAction(std::move(action));
    queuePoseKeySelection(std::move(id));
}

std::string TimelinePanel::markerAt(
    PanelContext const&  ctx,
    Layout const&        layout,
    TimelineScale const& scale,
    ImVec2               mouse
) const {
    auto const project = ctx.state.project;
    if (!project || mouse.y < layout.rulerTop || mouse.y > layout.bodyTop || mouse.x < layout.canvasLeft
        || mouse.x > layout.fullMax.x) {
        return {};
    }
    float const reach    = kMarkerHalfWidth * metrics::scale() + 2.0f;
    float       bestDist = reach;
    std::string best;
    for (auto const& marker : project->markers) {
        float const distance = std::abs(scale.xAt(static_cast<float>(marker.tick)) - mouse.x);
        if (distance <= bestDist) {
            bestDist = distance;
            best     = marker.id;
        }
    }
    return best;
}

void TimelinePanel::drawMarkers(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale) {
    auto const project = ctx.state.project;
    if (!project || project->markers.empty()) return;
    auto*       drawList = ImGui::GetWindowDrawList();
    auto const* selected = ctx.selection.getAs<state::editing::model::SelectedMarker>();
    float const half     = kMarkerHalfWidth * metrics::scale();
    float const bottom   = layout.bodyTop - 1.0f;
    float const top      = layout.rulerTop + (layout.bodyTop - layout.rulerTop) * 0.35f;
    ImVec2 const mouse   = ImGui::GetMousePos();
    for (auto const& marker : project->markers) {
        bool const  dragging = mMarkerDragId == marker.id && mMarkerDragMoved;
        int const   tick     = dragging ? mMarkerDragTick : marker.tick;
        float const x        = scale.xAt(static_cast<float>(tick));
        if (x < layout.canvasLeft - half || x > layout.fullMax.x + half) continue;
        bool const  active = dragging || (selected && selected->markerId == marker.id);
        ImU32 const color  = active ? theme::kIconHighlight : theme::kWarning;
        // A tag shape pointing down at its tick, plus a faint line through the lanes.
        drawList->AddLine({x, layout.bodyTop}, {x, layout.bodyBottom}, theme::withAlpha(color, active ? 150 : 70), 1.0f);
        float const shoulder = bottom - half;
        drawList->AddRectFilled({x - half, top}, {x + half, shoulder}, color, 1.5f);
        drawList->AddTriangleFilled({x - half, shoulder}, {x + half, shoulder}, {x, bottom}, color);
        if (std::abs(mouse.x - x) <= half + 2.0f && mouse.y >= layout.rulerTop && mouse.y <= layout.bodyTop
            && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && mMarkerDragId.empty()) {
            ImGui::SetTooltip("%s  %s", marker.label.c_str(), formatTick(marker.tick).c_str());
        }
    }
}

void TimelinePanel::zoomIn() { mZoomScale = std::min(kMaxZoomScale, mZoomScale * kZoomStep); }

void TimelinePanel::zoomOut() { mZoomScale = std::max(kMinZoomScale, mZoomScale / kZoomStep); }

void TimelinePanel::resetZoom() {
    mZoomScale = kMinZoomScale;
    mScrollX   = 0.0f;
}

void TimelinePanel::drawTitleLabel(PanelContext const& ctx, float height) {
    auto const        project = ctx.state.project;
    std::string const title   = "playback.refactorEditor.timeline.title"_tr();
    float const       rowY    = std::max(0.0f, widgets::textOffsetInBox(height, title.c_str()));
    // ImGui adds no offset of its own here, so the cursor Y is the text's draw Y.
    ImGui::SetCursorPos({metrics::gutter(), rowY});
    ImGui::TextDisabled("%s", title.c_str());
    if (!project) return;
    ImGui::SameLine(0.0f, metrics::gutter());
    ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.trackCount"_tr(project->cameras.size()).c_str());
}

void TimelinePanel::drawTransportGroup(PanelContext const& ctx, int displayTick, float width) {
    auto const&       state    = ctx.state;
    float const       unit     = metrics::iconButton();
    float const       rowY     = std::max(0.0f, (metrics::toolbarRow() - unit) * 0.5f);
    std::string const timecode = formatTick(displayTick) + "  /  " + formatTick(state.totalTicks);
    char              speedLabel[24]{};
    std::snprintf(speedLabel, sizeof(speedLabel), "%.2fx", state.playbackSpeed);

    float const timeWidth  = ImGui::CalcTextSize(timecode.c_str()).x;
    float const speedWidth = widgets::dropdownChipWidth(speedLabel);
    float const groupWidth = timeWidth + metrics::gutter() * 2.0f + unit * 5.0f + 4.0f + metrics::gutter() * 2.0f
                           + speedWidth + metrics::gutter() * 2.0f + unit;
    float const startX = std::max(0.0f, (width - groupWidth) * 0.5f);

    // SameLine() restores Y to the line's first item, so the timecode is drawn rather than submitted.
    ImGui::SetCursorPos({startX, rowY});
    ImVec2 const buttonOrigin = ImGui::GetCursorScreenPos();
    ImVec2 const timePos{buttonOrigin.x, widgets::textYForCentre(buttonOrigin.y + unit * 0.5f, timecode.c_str())};
    ImGui::GetWindowDrawList()->AddText(timePos, theme::kTextDim, timecode.c_str());

    ImGui::SetCursorPos({startX + timeWidth + metrics::gutter() * 2.0f, rowY});
    using widgets::VectorIcon;
    if (widgets::vectorIconButton(
            "transport-start",
            VectorIcon::SkipStart,
            "playback.refactorEditor.timeline.skipToStart"_tr().c_str()
        ))
        seekTo(ctx, 0);
    ImGui::SameLine(0.0f, 1.0f);
    if (widgets::vectorIconButton(
            "transport-prev",
            VectorIcon::StepBack,
            "playback.refactorEditor.timeline.previousTick"_tr().c_str()
        ))
        seekRelative(ctx, -1);
    ImGui::SameLine(0.0f, 1.0f);
    if (widgets::vectorIconButton(
            "transport-play",
            state.paused ? VectorIcon::Play : VectorIcon::Pause,
            (state.paused ? "playback.refactorEditor.timeline.play"_tr()
                          : "playback.refactorEditor.timeline.pause"_tr())
                .c_str()
        )) {
        ctx.submitAction({EditorActionType::TogglePause});
    }
    ImGui::SameLine(0.0f, 1.0f);
    if (widgets::vectorIconButton(
            "transport-next",
            VectorIcon::StepForward,
            "playback.refactorEditor.timeline.nextTick"_tr().c_str()
        ))
        seekRelative(ctx, 1);
    ImGui::SameLine(0.0f, 1.0f);
    if (widgets::vectorIconButton(
            "transport-end",
            VectorIcon::SkipEnd,
            "playback.refactorEditor.timeline.skipToEnd"_tr().c_str()
        ))
        seekTo(ctx, state.totalTicks);

    ImGui::SameLine(0.0f, metrics::gutter() * 2.0f);
    if (widgets::dropdownChip(
            "transport-speed",
            speedLabel,
            "playback.refactorEditor.timeline.speedTooltip"_tr().c_str()
        )) {
        widgets::openAnchoredPopup("##transport-speed-menu");
    }
    // The menu lists the speeds themselves; stepping up and down stays on the keyboard shortcuts.
    if (widgets::beginAnchoredPopup("##transport-speed-menu")) {
        for (float const speed : replay::ReplaySession::playbackSpeeds()) {
            char label[16]{};
            std::snprintf(label, sizeof(label), "%gx", static_cast<double>(speed));
            if (ImGui::MenuItem(label, nullptr, std::abs(state.playbackSpeed - speed) < 0.0001f)) {
                EditorAction action{EditorActionType::SetPlaybackSpeed};
                action.speed = speed;
                ctx.submitAction(std::move(action));
            }
        }
        widgets::endAnchoredPopup();
    }

    bool const maximized = ctx.commands.isViewportMaximized();
    ImGui::SameLine(0.0f, metrics::gutter() * 2.0f);
    if (widgets::vectorIconButton(
            "transport-maximize",
            maximized ? VectorIcon::Restore : VectorIcon::Maximize,
            (maximized ? "playback.refactorEditor.timeline.restore"_tr()
                       : "playback.refactorEditor.timeline.maximize"_tr())
                .c_str(),
            maximized
        )) {
        ctx.commands.toggleViewportMaximized();
    }
}

void TimelinePanel::drawTitleRow(PanelContext const& ctx, Layout const& layout, int displayTick, bool allowInput) {
    float const height   = metrics::toolbarRow();
    float const width    = layout.fullMax.x - layout.fullMin.x;
    auto*       drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(layout.fullMin, {layout.fullMax.x, layout.titleBottom}, theme::kBgHeader);
    drawList->AddLine({layout.fullMin.x, layout.titleBottom}, {layout.fullMax.x, layout.titleBottom}, theme::kBorder);

    ImGui::SetCursorScreenPos(layout.fullMin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##TimelineTitle", {width, height}, false, ImGuiWindowFlags_NoScrollbar);
    drawTitleLabel(ctx, height);
    drawTransportGroup(ctx, displayTick, width);

    // Zooming lives on the range bar handles now; only the reset-to-fit affordance stays here.
    float const unit = metrics::iconButton();
    ImGui::SetCursorPos({std::max(0.0f, width - unit - metrics::gutter()), std::max(0.0f, (height - unit) * 0.5f)});
    if (iconButton("timeline-zoom-reset", ICON_RESET, "playback.refactorEditor.timeline.zoomReset"_tr().c_str()))
        resetZoom();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    (void)allowInput;
}

void TimelinePanel::addLayer(
    PanelContext const&                ctx,
    state::editing::model::OverlayKind kind,
    state::editing::model::ShapeKind   shape,
    int                                points,
    state::editing::model::BlurStyle   blur
) {
    auto const* project = ctx.project();
    if (!project) return;
    int const tick = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    auto      item = overlays::makeLayer(*project, kind, tick, shape, points, blur);
    if (kind == state::editing::model::OverlayKind::Model) {
        auto const file = overlays::pickModelFile();
        if (!file) return;
        item.model.file = *file;
        auto const stem = std::filesystem::path(std::u8string(file->begin(), file->end())).stem().u8string();
        if (!stem.empty()) item.name = std::string(stem.begin(), stem.end());
    }
    auto layers = project->overlays;
    layers.push_back(item);
    overlays::commit(ctx, std::move(layers));
    queueOverlaySelection(item.id);
}

void TimelinePanel::drawTrackHeaders(PanelContext const& ctx, Layout const& layout, bool allowInput) {
    using state::editing::model::OverlayKind;
    using state::editing::model::ShapeKind;
    auto const  project  = ctx.state.project;
    auto*       drawList = ImGui::GetWindowDrawList();
    float const listLeft = layout.fullMin.x;
    float const listW    = layout.listWidth;

    drawList
        ->AddRectFilled({listLeft, layout.rulerTop}, {listLeft + listW, layout.bodyBottom}, theme::kTimelineSidebar);

    ImGui::SetCursorScreenPos({listLeft, layout.rulerTop});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild(
        "##TimelineTrackControls",
        {listW, layout.bodyTop - layout.rulerTop},
        false,
        ImGuiWindowFlags_NoScrollbar
    );
    // The add and snap buttons are exactly as tall as the search field, so the band reads as one row of controls.
    float const controlSize = std::floor(ImGui::GetFrameHeight());
    float const controlY    = std::max(0.0f, std::floor((layout.headerHeight - controlSize) * 0.5f));
    float const gap         = std::floor(metrics::gutter() * 0.5f);
    ImGui::SetCursorPos({gap, controlY});
    if (widgets::compactIconToggle(
            "timeline-add",
            ICON_ADD,
            "playback.refactorEditor.timeline.addTrack"_tr().c_str(),
            false,
            controlSize
        )) {
        widgets::openAnchoredPopup("##timeline-add-menu");
    }
    if (widgets::beginAnchoredPopup("##timeline-add-menu")) {
        // An empty name lets the ops layer pick the next free localized default.
        // The action kind picks the track kind: keyframe, first person or third person.
        std::string const labels[] = {
            "playback.refactorEditor.details.addFreeCamera"_tr(),
            "playback.refactorEditor.timeline.addFirstPersonTrack"_tr(),
            "playback.refactorEditor.timeline.addThirdPersonTrack"_tr(),
        };
        for (int kind = 0; kind < static_cast<int>(std::size(labels)); ++kind) {
            if (ImGui::MenuItem(labels[kind].c_str())) {
                EditorAction action{EditorActionType::AddFreeCamera};
                action.kind = kind;
                ctx.submitAction(std::move(action));
            }
        }
        // A pose lane starts with a key at the playhead for the entity picked here.
        if (ImGui::BeginMenu("playback.refactorEditor.timeline.addPoseKey"_tr().c_str())) {
            bool any = false;
            for (auto const& entity : replay::ReplaySession::getInstance().replayEntities()) {
                if (entity.recorder || entity.choice.empty()) continue;
                any = true;
                if (ImGui::MenuItem((entity.label + "##pose-" + entity.choice).c_str())) {
                    addPoseKeyAt(
                        ctx,
                        entity.choice,
                        entity.label,
                        ctx.state.currentTick,
                        std::string{state::editing::model::PoseKey::DefaultAnimation}
                    );
                }
            }
            if (!any) ImGui::TextDisabled("%s", "playback.refactorEditor.viewport.noEntityHere"_tr().c_str());
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem((ICON_TITLE "  " + "playback.refactorEditor.layers.addTitle"_tr()).c_str())) {
            addLayer(ctx, OverlayKind::Title);
        }
        if (ImGui::BeginMenu((ICON_SHAPE "  " + "playback.refactorEditor.layers.addShape"_tr()).c_str())) {
            if (ImGui::MenuItem((ICON_RECTANGLE "  " + "playback.refactorEditor.layers.rectangle"_tr()).c_str())) {
                addLayer(ctx, OverlayKind::Shape, ShapeKind::Rectangle);
            }
            if (ImGui::MenuItem((ICON_ELLIPSE "  " + "playback.refactorEditor.layers.ellipse"_tr()).c_str())) {
                addLayer(ctx, OverlayKind::Shape, ShapeKind::Ellipse);
            }
            if (ImGui::MenuItem((ICON_TRIANGLE "  " + "playback.refactorEditor.layers.triangle"_tr()).c_str())) {
                addLayer(ctx, OverlayKind::Shape, ShapeKind::Polygon, 3);
            }
            if (ImGui::MenuItem((ICON_POLYGON "  " + "playback.refactorEditor.layers.polygon"_tr()).c_str())) {
                addLayer(ctx, OverlayKind::Shape, ShapeKind::Polygon, 6);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem((ICON_MODEL "  " + "playback.refactorEditor.layers.addModel"_tr()).c_str())) {
            addLayer(ctx, OverlayKind::Model);
        }
        // Blurs act on the replay picture over their stretch of the timeline, the whole frame or an area of it.
        if (ImGui::BeginMenu((ICON_BLUR "  " + "playback.refactorEditor.layers.addBlur"_tr()).c_str())) {
            using state::editing::model::BlurStyle;
            for (auto const style : {BlurStyle::Gaussian, BlurStyle::Mosaic, BlurStyle::Motion, BlurStyle::Radial}) {
                std::string const label = std::string(overlays::blurStyleIcon(style)) + "  " + overlays::blurStyleName(style);
                if (ImGui::MenuItem(label.c_str())) {
                    addLayer(ctx, OverlayKind::Blur, ShapeKind::Rectangle, 3, style);
                }
            }
            ImGui::EndMenu();
        }
        widgets::endAnchoredPopup();
    }
    ImGui::SameLine(0.0f, gap);
    ImGui::SetCursorPosY(controlY);
    if (widgets::compactIconToggle(
            "timeline-snap",
            ICON_TRACK_ACTIVE,
            "playback.refactorEditor.timeline.snapTooltip"_tr().c_str(),
            mSnapEnabled,
            controlSize
        )) {
        mSnapEnabled = !mSnapEnabled;
    }
    ImGui::SameLine(0.0f, gap);
    ImGui::SetCursorPosY(controlY);
    char search[128]{};
    std::snprintf(search, sizeof(search), "%s", mTrackSearch.c_str());
    ImGui::SetNextItemWidth(std::max(32.0f, ImGui::GetContentRegionAvail().x - gap));
    if (ImGui::InputTextWithHint(
            "##timeline-search",
            "playback.refactorEditor.timeline.searchTracks"_tr().c_str(),
            search,
            sizeof(search)
        )) {
        mTrackSearch = search;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SetCursorScreenPos({listLeft, layout.bodyTop});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild(
        "##TimelineTrackList",
        {listW, layout.bodyBottom - layout.bodyTop},
        false,
        ImGuiWindowFlags_NoScrollbar
    );
    drawList->PushClipRect({listLeft, layout.bodyTop}, {listLeft + listW, layout.bodyBottom}, true);

    auto const*  selectedCamera   = ctx.selection.getAs<state::editing::model::SelectedCamera>();
    auto const*  selectedKeyframe = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
    auto const*  selectedOverlay  = ctx.selection.getAs<state::editing::model::SelectedOverlay>();
    bool const   replaySelected   = ctx.selection.getAs<state::editing::model::SelectedWorldActor>()
                              || ctx.selection.getAs<state::editing::model::SelectedWorldActorSegment>();
    int const    playheadTick     = mPendingSeekTick >= 0 ? mPendingSeekTick : ctx.state.currentTick;
    ImVec2 const mouse            = ImGui::GetMousePos();
    bool const   listHovered      = ImGui::IsWindowHovered();
    float        rowY             = layout.bodyTop - mScrollY;
    struct {
        char const* id{};
        ImVec2      min{};
        ImVec2      max{};
    } rowMenu;
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = rowY + row.height;
        if (rowBottom < layout.bodyTop || rowY > layout.bodyBottom) {
            rowY = rowBottom;
            continue;
        }
        bool const selected =
            (row.kind == TrackRowKind::Camera
             && ((selectedCamera && selectedCamera->cameraId == row.cameraId)
                 || (selectedKeyframe && selectedKeyframe->trackId == row.cameraId)))
            || (row.kind == TrackRowKind::Overlay && selectedOverlay && selectedOverlay->overlayId == row.overlayId)
            || (row.kind == TrackRowKind::Replay && replaySelected);

        ImGui::SetCursorScreenPos({listLeft, rowY});
        ImGui::InvisibleButton(("##track-row-" + row.id).c_str(), {listW, row.height});
        bool const  hovered         = ImGui::IsItemHovered();
        bool const  clicked         = allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Left);
        bool const  rowRightClicked = allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Right);
        float const selectedAmount  = mAnimator.animate("row-selected", row.id, selected ? 1.0f : 0.0f);
        float const hoverAmount     = mAnimator.animate("row-hover", row.id, hovered ? 1.0f : 0.0f);
        float const highlight       = std::max(selectedAmount, hoverAmount);
        if (highlight > 0.0f) {
            drawList->AddRectFilled(
                {listLeft, rowY},
                {listLeft + listW, rowBottom},
                lerpColor(theme::kTimelineSidebar, theme::kRowHighlight, highlight)
            );
        }
        if (selectedAmount > 0.0f) {
            drawList->AddRect(
                {listLeft + 1.0f, rowY},
                {listLeft + listW - 1.0f, rowBottom},
                lerpColor(theme::withAlpha(theme::kRowSelectedBorder, 0), theme::kRowSelectedBorder, selectedAmount),
                0.0f,
                0,
                1.0f
            );
        }
        ImU32 const swatch = !row.enabled ? theme::kTrackDisabled
                           : row.kind == TrackRowKind::World
                                     || (row.kind == TrackRowKind::Property && row.owner == PropertyOwner::World)
                               ? theme::kKeyframe
                           : row.kind == TrackRowKind::Overlay
                                     || (row.kind == TrackRowKind::Property && row.owner == PropertyOwner::Overlay)
                               ? layerColor(row.overlayKind)
                           : row.kind == TrackRowKind::Replay ? theme::kReplayLayer
                                                              : theme::trackColor(row.cameraIndex);
        drawList->AddRectFilled(
            {listLeft, rowY},
            {listLeft + metrics::trackSwatch(), rowBottom},
            lerpColor(theme::withAlpha(swatch, 0x99), swatch, selectedAmount)
        );

        // Every control is one square slot a little shorter than the row, its glyph near the label's size.
        float const rowCentre = std::floor((rowY + rowBottom) * 0.5f + 0.5f);
        float const slot      = std::floor(std::min(row.height - 4.0f, metrics::iconButton()));
        float const glyph     = std::floor(slot * 0.68f);
        float const slotTop   = rowCentre - std::floor(slot * 0.5f);
        float       slotRight = std::floor(listLeft + listW - metrics::gutter() * 0.5f);
        // Laid out right to left; returns true when clicked.
        auto const control = [&](char const* icon, ImU32 color, char const* tooltip, bool enabled) {
            ImVec2 const min{slotRight - slot, slotTop};
            ImVec2 const max{slotRight, slotTop + slot};
            slotRight -= slot + 2.0f;
            bool const over = enabled && listHovered && contains(min, max, mouse);
            if (over) {
                drawList->AddRectFilled(min, max, theme::withAlpha(theme::kButtonHover, 0xd0), theme::kFrameRounding);
                widgets::hoverTooltip(tooltip);
            }
            widgets::drawIconAtCentre(
                drawList,
                icon,
                {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f},
                !enabled ? theme::withAlpha(theme::kTextDim, 0x80) : over ? theme::kIconHighlight : color,
                glyph
            );
            return allowInput && over && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        };
        auto const lockControl = [&](bool locked) {
            return control(
                locked ? ICON_LOCK : ICON_UNLOCK,
                locked ? theme::kWarning : theme::withAlpha(theme::kIconInactive, 0x90),
                (locked ? "playback.refactorEditor.timeline.unlockLayer"_tr()
                        : "playback.refactorEditor.timeline.lockLayer"_tr())
                    .c_str(),
                true
            );
        };
        auto const eyeControl = [&](bool visible) {
            return control(
                visible ? ICON_EYE : ICON_HIDE,
                visible ? theme::kIconInactive : theme::kTextDim,
                (visible ? "playback.refactorEditor.timeline.disableTrack"_tr()
                         : "playback.refactorEditor.timeline.enableTrack"_tr())
                    .c_str(),
                true
            );
        };

        switch (row.kind) {
        case TrackRowKind::Camera:
            if (eyeControl(row.enabled)) {
                EditorAction action{EditorActionType::SetCameraEnabled};
                action.id    = row.cameraId;
                action.value = !row.enabled;
                ctx.submitAction(std::move(action));
            }
            if (lockControl(row.locked)) {
                EditorAction action{EditorActionType::SetCameraLocked};
                action.id    = row.cameraId;
                action.value = !row.locked;
                ctx.submitAction(std::move(action));
            }
            // A locked track takes no keys.
            if (control(
                    ICON_ADD_KEYFRAME,
                    theme::kIconInactive,
                    "playback.refactorEditor.timeline.saveCameraToKeyframe"_tr().c_str(),
                    !row.locked
                )) {
                EditorAction action{EditorActionType::SaveCameraToKeyframe};
                action.id   = row.cameraId;
                action.tick = playheadTick;
                ctx.submitAction(std::move(action));
            }
            break;
        case TrackRowKind::Overlay:
            if (auto const* item = project ? overlays::find(*project, row.overlayId) : nullptr) {
                if (eyeControl(item->visible)) {
                    auto changed    = *item;
                    changed.visible = !item->visible;
                    overlays::commitItem(ctx, changed);
                }
                if (lockControl(item->locked)) {
                    auto changed   = *item;
                    changed.locked = !item->locked;
                    overlays::commitItem(ctx, changed);
                }
            }
            break;
        case TrackRowKind::Replay:
            // The replay has no eye; its lock stays in the same column as every other row's.
            slotRight -= slot + 2.0f;
            if (lockControl(row.locked)) {
                EditorAction action{EditorActionType::SetWorldActorLocked};
                action.value = !row.locked;
                ctx.submitAction(std::move(action));
            }
            if (control(
                    ICON_SPLIT,
                    theme::kIconInactive,
                    "playback.refactorEditor.timeline.cutAtPlayhead"_tr().c_str(),
                    !row.locked
                )) {
                EditorAction action{EditorActionType::SplitWorldActor};
                action.tick = playheadTick;
                ctx.submitAction(std::move(action));
            }
            break;
        case TrackRowKind::Pose:
        case TrackRowKind::World:
            break;
        case TrackRowKind::Property:
            // ◀ ◆ ▶ as in the inspector: the previous key, a key at the playhead (add or remove), the next key.
            if (auto const keyed = project ? rowChannel(*project, row) : RowChannel{}; keyed.channel) {
                int const  local     = playheadTick - keyed.offset;
                bool const keyedHere = keyed.channel->contains(local);
                auto const after     = keyed.channel->upper_bound(local);
                auto const before    = keyed.channel->lower_bound(local);
                if (control(
                        ICON_CHEVRON_RIGHT,
                        theme::kIconInactive,
                        "playback.refactorEditor.keyframes.next"_tr().c_str(),
                        after != keyed.channel->end()
                    )) {
                    submitSeek(ctx, after->first + keyed.offset);
                }
                if (control(
                        ICON_KEYFRAME,
                        keyedHere ? theme::kKeyframeSelected : theme::kIconInactive,
                        (keyedHere ? "playback.refactorEditor.keyframes.removeKey"_tr()
                                   : "playback.refactorEditor.keyframes.addKey"_tr())
                            .c_str(),
                        !row.locked
                    )) {
                    // Every channel of the row keys at its value here (a camera's from the camera itself, so a
                    // shake value without keys yet keys at its set value).
                    auto const members = rowMembers(row);
                    std::vector<float> values;
                    auto const camera = std::ranges::find(project->cameras, row.ownerId, &state::editing::model::CameraEntity::id);
                    for (auto const& name : members) {
                        values.push_back(
                            row.owner == PropertyOwner::Camera && camera != project->cameras.end()
                                ? state::editing::model::cameraPropertyAt(*camera, name, local)
                                : state::editing::model::sampleChannel(*keyed.channel, local).value_or(0.0f)
                        );
                    }
                    editChannels(ctx, row.owner, row.ownerId, [&](state::editing::model::ValueChannels& channels) {
                        for (size_t index = 0; index < members.size(); ++index) {
                            auto& channel = channels[members[index]];
                            if (keyedHere) channel.erase(local);
                            else channel.insert_or_assign(local, state::editing::model::ValueKey{values[index]});
                        }
                    });
                }
                if (control(
                        ICON_CHEVRON_LEFT,
                        theme::kIconInactive,
                        "playback.refactorEditor.keyframes.previous"_tr().c_str(),
                        before != keyed.channel->begin()
                    )) {
                    submitSeek(ctx, std::prev(before)->first + keyed.offset);
                }
            }
            break;
        }
        bool const overControls = mouse.x >= slotRight;

        // Label: an expand chevron, the kind's glyph and the name, clipped short of the controls. Property rows sit
        // indented under their owner.
        auto const* overlayItem = row.kind == TrackRowKind::Overlay && project
                                    ? overlays::find(*project, row.overlayId)
                                    : nullptr;
        char const* kindIcon    = row.kind == TrackRowKind::Property                                 ? nullptr
                                : row.kind == TrackRowKind::World                                    ? ICON_WORLD
                                : row.kind == TrackRowKind::Replay                                   ? ICON_FILM
                                : overlayItem                                                        ? overlays::icon(*overlayItem)
                                : row.trackKind == state::editing::model::CameraTrackKind::Keyframe ? ICON_CAMERA
                                                                                                     : ICON_USER;
        float const indent    = metrics::gutter() + metrics::trackSwatch();
        float       labelX    = listLeft + indent;
        ImU32 const textColor = row.enabled ? theme::kText : theme::kTextDim;
        float const textY     = widgets::textYForCentre(rowCentre, row.name.c_str());
        float const labelIcon = std::floor(metrics::font() * 1.05f);
        float const chevron   = std::floor(labelIcon * 0.85f);
        float const inkCentre = widgets::textInkCentre(textY, row.name.c_str());
        bool        overChevron = false;
        if (row.expandable) {
            ImVec2 const min{labelX - 2.0f, rowY};
            ImVec2 const max{labelX + chevron + 2.0f, rowBottom};
            overChevron = listHovered && contains(min, max, mouse);
            widgets::drawIconAtCentre(
                drawList,
                row.expanded ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT,
                {labelX + chevron * 0.5f, inkCentre},
                overChevron ? theme::kIconHighlight : theme::kIconInactive,
                chevron
            );
            if (overChevron) {
                widgets::hoverTooltip(
                    (row.expanded ? "playback.refactorEditor.timeline.collapseProperties"_tr()
                                  : "playback.refactorEditor.timeline.expandProperties"_tr())
                        .c_str()
                );
            }
            if (overChevron && clicked) {
                if (row.expanded) mExpandedRows.erase(row.id);
                else mExpandedRows.insert(row.id);
            }
        }
        labelX += chevron + metrics::gutter() * 0.25f;
        if (kindIcon) {
            widgets::drawIconAtCentre(drawList, kindIcon, {labelX + labelIcon * 0.5f, inkCentre}, textColor, labelIcon);
        }
        labelX += labelIcon + metrics::gutter() * 0.5f;
        ImVec4 const clip{labelX, rowY, std::max(labelX, slotRight - 2.0f), rowBottom};
        drawList->AddText(
            ImGui::GetFont(),
            ImGui::GetFontSize(),
            {labelX, textY},
            textColor,
            row.name.c_str(),
            nullptr,
            0.0f,
            &clip
        );

        if (!overControls && !overChevron) {
            switch (row.kind) {
            case TrackRowKind::Camera:
                if (clicked) {
                    ctx.selection.select(state::editing::model::SelectedCamera{row.cameraId});
                    EditorAction action{EditorActionType::SetPreviewCamera};
                    action.id = row.cameraId;
                    ctx.submitAction(std::move(action));
                } else if (rowRightClicked) {
                    ctx.selection.select(state::editing::model::SelectedCamera{row.cameraId});
                    mTrackMenuCameraId = row.cameraId;
                    rowMenu = {"##timeline-track-menu", ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
                }
                break;
            case TrackRowKind::Overlay:
                if (clicked || rowRightClicked) {
                    ctx.selection.select(state::editing::model::SelectedOverlay{row.overlayId});
                }
                if (rowRightClicked) {
                    mOverlayMenuId = row.overlayId;
                    rowMenu = {"##timeline-overlay-menu", ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
                }
                break;
            case TrackRowKind::Replay:
                if (clicked || rowRightClicked) ctx.selection.select(state::editing::model::SelectedWorldActor{});
                break;
            case TrackRowKind::Pose:
            case TrackRowKind::World:
            case TrackRowKind::Property:
                break;
            }
        }
        rowY = rowBottom;
    }
    drawList->PopClipRect();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    // Popups are scoped to the window that opens them; the menus are drawn by the timeline window, not this list.
    if (rowMenu.id) widgets::openAnchoredPopup(rowMenu.id, rowMenu.min, rowMenu.max);

    drawList->AddLine({listLeft + listW, layout.rulerTop}, {listLeft + listW, layout.fullMax.y}, theme::kLine);
}

void TimelinePanel::drawRuler(
    PanelContext const&  ctx,
    Layout const&        layout,
    TimelineScale const& scale,
    int                  displayTick
) {
    auto const& state    = ctx.state;
    auto*       drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(
        {layout.canvasLeft, layout.rulerTop},
        {layout.fullMax.x, layout.bodyTop},
        theme::kTimelineRuler
    );
    drawList->AddLine({layout.canvasLeft, layout.bodyTop}, {layout.fullMax.x, layout.bodyTop}, theme::kLine);

    std::string const longest       = formatTickCompact(state.totalTicks);
    float const       minimumMajor  = std::max(metrics::font() * 6.0f, ImGui::CalcTextSize(longest.c_str()).x + 30.0f);
    int const         majorStep     = majorTickStep(scale.pixelsPerTick, minimumMajor);
    int const         minorStep     = std::max(1, majorStep / 5);
    int const         firstTick     = scale.firstVisibleTick(minorStep);
    float const       rulerBaseline = layout.bodyTop - 1.0f;
    float             lastLabelEnd  = layout.canvasLeft - 6.0f;
    for (int tick = firstTick; tick <= state.totalTicks; tick += minorStep) {
        float const x = scale.xAt(static_cast<float>(tick));
        if (x < layout.canvasLeft || x > layout.fullMax.x) continue;
        bool const  major      = tick % majorStep == 0;
        float const tickHeight = major ? 8.0f : 4.0f;
        drawList->AddLine(
            {x, rulerBaseline - tickHeight},
            {x, rulerBaseline},
            major ? theme::kRulerMajor : theme::kRulerMinor
        );
        if (!major) continue;
        std::string const label      = formatTickCompact(tick);
        float const       labelWidth = ImGui::CalcTextSize(label.c_str()).x;
        float const       labelX     = x + 3.0f;
        float const       labelY     = layout.rulerTop + (layout.headerHeight - 8.0f - metrics::font()) * 0.5f;
        // Skip instead of clamping into place; clamping stacks labels on top of each other at the edges.
        if (labelX >= lastLabelEnd + 6.0f && labelX + labelWidth <= layout.fullMax.x - 4.0f) {
            drawList->AddText({labelX, labelY}, theme::kTextDim, label.c_str());
            lastLabelEnd = labelX + labelWidth;
        }
    }
    (void)displayTick;
}

void TimelinePanel::drawTrackGrid(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale) {
    auto const& state    = ctx.state;
    auto*       drawList = ImGui::GetWindowDrawList();

    std::string const longest      = formatTickCompact(state.totalTicks);
    float const       minimumMajor = std::max(metrics::font() * 6.0f, ImGui::CalcTextSize(longest.c_str()).x + 30.0f);
    int const         majorStep    = majorTickStep(scale.pixelsPerTick, minimumMajor);
    for (int tick = scale.firstVisibleTick(majorStep); tick <= state.totalTicks; tick += majorStep) {
        float const x = scale.xAt(static_cast<float>(tick));
        if (x < layout.canvasLeft || x > layout.fullMax.x) continue;
        drawList->AddLine({x, layout.bodyTop}, {x, layout.bodyBottom}, theme::kTrackGrid);
    }
}

void TimelinePanel::drawExportRange(
    PanelContext const&  ctx,
    Layout const&        layout,
    TimelineScale const& scale,
    bool                 allowInput
) {
    int const total = ctx.state.totalTicks;
    if (total <= 0) return;
    // Unset means "whole replay", so nothing is drawn until the user marks a point.
    int const inTick  = ctx.commands.exportStartTick();
    int const outTick = ctx.commands.exportEndTick();
    if (inTick < 0 || outTick < 0) return;

    auto*       drawList = ImGui::GetWindowDrawList();
    float const left     = scale.xAt(static_cast<float>(std::clamp(inTick, 0, total)));
    float const right    = scale.xAt(static_cast<float>(std::clamp(outTick, 0, total)));
    float const top      = layout.rulerTop;
    float const bottom   = layout.bodyTop;
    drawList->PushClipRect({layout.canvasLeft, top}, {layout.fullMax.x, bottom}, true);

    // The band lives in the ruler strip, bracketed at each end; the bracket is the grab handle.
    drawList->AddRectFilled({left, top}, {right, bottom}, theme::kExportBand);
    ImVec2 const mouse  = ImGui::GetMousePos();
    bool const   inBand = allowInput && mouse.y >= top && mouse.y <= bottom;
    float const  stem   = std::max(1.0f, 1.5f * metrics::scale());
    float const  jaw    = std::max(4.0f, metrics::gutter() * 0.9f);
    auto const   marker = [&](float x, bool isIn, int dragMode) {
        // Grab area stays wide even though the bracket is thin.
        bool const  hovered = inBand && std::abs(mouse.x - x) <= jaw + 3.0f;
        ImU32 const color = (mExportMarkerDrag == dragMode || hovered) ? theme::kIconHighlight : theme::kExportBracket;
        float const inner = isIn ? x + stem : x - stem;
        float const tip   = isIn ? x + jaw : x - jaw;
        drawList->AddRectFilled({std::min(x, inner), top}, {std::max(x, inner), bottom}, color);
        // Jaws turn the stem into a bracket that faces into the range.
        drawList->AddRectFilled({std::min(inner, tip), top}, {std::max(inner, tip), top + stem}, color);
        drawList->AddRectFilled({std::min(inner, tip), bottom - stem}, {std::max(inner, tip), bottom}, color);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) mExportMarkerDrag = dragMode;
        return hovered;
    };
    bool const overIn  = marker(left, true, 1);
    bool const overOut = marker(right, false, 2);
    if (overIn || overOut || mExportMarkerDrag != 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    drawList->PopClipRect();

    if (mExportMarkerDrag == 0) return;
    if (!allowInput || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        mExportMarkerDrag = 0;
        return;
    }
    // Shift snaps to keyframes, markers and the playhead, matching the modifier used elsewhere on the timeline.
    int dragged = std::clamp(scale.tickAt(mouse.x), 0, total);
    if (ImGui::GetIO().KeyShift) {
        dragged = snapToEditPoints(ctx, dragged, scale.pixelsPerTick, true, false, {}, {});
    }
    if (mExportMarkerDrag == 1) ctx.commands.setExportStartTick(std::min(dragged, outTick));
    else ctx.commands.setExportEndTick(std::max(dragged, inTick));
}

void TimelinePanel::drawContextMenus(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    using CameraInterpolationType = state::editing::model::CameraInterpolationType;

    if (widgets::beginAnchoredPopup("##timeline-keyframe-menu")) {
        state::editing::model::CameraEntity const* camera = nullptr;
        if (project) {
            auto const found =
                std::ranges::find(project->cameras, mKeyframeMenuCameraId, &state::editing::model::CameraEntity::id);
            if (found != project->cameras.end()) camera = &*found;
        }
        auto const key = [&]() -> state::editing::model::CameraKeyframe const* {
            if (!camera) return nullptr;
            auto const found = camera->keysByTick.find(mKeyframeMenuTick);
            return found == camera->keysByTick.end() ? nullptr : &found->second;
        }();
        if (!camera || !key) {
            ImGui::CloseCurrentPopup();
        } else {
            // Right-clicking a key inside a multi-selection edits the whole set.
            bool const group    = mKeyframeMenuGroup && ctx.selection.keyframeCount() > 1;
            bool const editable = group || !camera->locked;
            if (group) {
                ImGui::TextDisabled(
                    "%s",
                    "playback.refactorEditor.details.keyframesSelected"_tr(ctx.selection.keyframeCount()).c_str()
                );
                ImGui::Separator();
            }
            auto const setInterpolation = [&](char const* label, CameraInterpolationType type) {
                if (ImGui::MenuItem(label, nullptr, !group && key->interpolationType == type, editable)) {
                    EditorAction action{
                        group ? EditorActionType::SetKeyframesInterpolation : EditorActionType::SetKeyframeInterpolation
                    };
                    action.id   = camera->id;
                    action.tick = mKeyframeMenuTick;
                    action.kind = static_cast<int>(type);
                    if (group) action.keyframeRefs = selectedKeyframeRefs(ctx);
                    ctx.submitAction(std::move(action));
                }
            };
            if (ImGui::BeginMenu("playback.refactorEditor.details.interpolation"_tr().c_str(), editable)) {
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.smooth"_tr().c_str(),
                    CameraInterpolationType::Smooth
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.linear"_tr().c_str(),
                    CameraInterpolationType::Linear
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.hold"_tr().c_str(),
                    CameraInterpolationType::Hold
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeIn"_tr().c_str(),
                    CameraInterpolationType::EaseIn
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeOut"_tr().c_str(),
                    CameraInterpolationType::EaseOut
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.easeInOut"_tr().c_str(),
                    CameraInterpolationType::EaseInOut
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.hermite"_tr().c_str(),
                    CameraInterpolationType::Hermite
                );
                setInterpolation(
                    "playback.refactorEditor.details.interpolationType.cubicBezier"_tr().c_str(),
                    CameraInterpolationType::CubicBezier
                );
                ImGui::Separator();
                if (ImGui::BeginMenu("playback.refactorEditor.details.easingPresets"_tr().c_str())) {
                    auto const current = !group && key->interpolationType == CameraInterpolationType::CubicBezier
                                           ? findEasingPreset(key->bezierCtrl1, key->bezierCtrl2)
                                           : std::nullopt;
                    if (auto const preset = drawEasingPresetMenus(current)) {
                        EditorAction action{EditorActionType::SetKeyframesInterpolation};
                        action.kind           = static_cast<int>(CameraInterpolationType::CubicBezier);
                        action.bezierControls = std::array{preset->control1, preset->control2};
                        if (group) action.keyframeRefs = selectedKeyframeRefs(ctx);
                        else
                            action.keyframeRefs = {
                                {camera->id, mKeyframeMenuTick, std::nullopt}
                            };
                        ctx.submitAction(std::move(action));
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(
                    group ? "playback.refactorEditor.timeline.copyKeyframes"_tr(ctx.selection.keyframeCount()).c_str()
                          : "playback.refactorEditor.timeline.copyKeyframe"_tr().c_str(),
                    "Ctrl+C"
                )) {
                if (group) copySelection(ctx);
                else mKeyframeClipboard = {ClipboardKey{camera->id, 0, *key}};
            }

            ImGui::SetNextItemWidth(96.0f * metrics::scale());
            ImGui::BeginDisabled(!editable || group);
            ImGui::InputInt("##timeline-keyframe-tick", &mKeyframeMenuTickEdit, 0, 0);
            bool const tickCommitted = ImGui::IsItemDeactivatedAfterEdit();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.setTick"_tr().c_str());
            if (editable && tickCommitted) {
                int const next = std::clamp(mKeyframeMenuTickEdit, 0, ctx.state.totalTicks);
                if (next != mKeyframeMenuTick && !camera->keysByTick.contains(next)) {
                    EditorAction action{EditorActionType::MoveCameraKeyframe};
                    action.id            = camera->id;
                    action.tick          = mKeyframeMenuTick;
                    action.secondaryTick = next;
                    ctx.submitAction(std::move(action));
                    ctx.selection.select(state::editing::model::SelectedKeyframe{camera->id, next});
                    mKeyframeMenuTick     = next;
                    mKeyframeMenuTickEdit = next;
                } else {
                    mKeyframeMenuTickEdit = mKeyframeMenuTick;
                }
            }

            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) seekTo(ctx, mKeyframeMenuTick);
            // Apply: the free camera jumps to this keyframe's pose without touching the track.
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.applyKeyframe"_tr().c_str(),
                    nullptr,
                    false,
                    ctx.state.replayVisible && camera->kind == state::editing::model::CameraTrackKind::Keyframe
                )) {
                ctx.selection.clear();
                EditorAction action{EditorActionType::MoveViewToKeyframe};
                action.id   = camera->id;
                action.tick = mKeyframeMenuTick;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.previewCamera"_tr().c_str(),
                    nullptr,
                    false,
                    state::editing::model::isCameraRenderable(*camera)
                        && camera->kind == state::editing::model::CameraTrackKind::Keyframe
                )) {
                EditorAction action{EditorActionType::SetPreviewCamera};
                action.id = camera->id;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    group ? "playback.refactorEditor.details.deleteKeyframes"_tr(ctx.selection.keyframeCount()).c_str()
                          : "playback.refactorEditor.details.deleteKeyframe"_tr().c_str(),
                    "Delete",
                    false,
                    editable
                )) {
                if (group) {
                    deleteSelection(ctx);
                } else {
                    EditorAction action{EditorActionType::DeleteCameraKeyframe};
                    action.id   = camera->id;
                    action.tick = mKeyframeMenuTick;
                    ctx.submitAction(std::move(action));
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-lane-menu")) {
        state::editing::model::CameraEntity const* camera = nullptr;
        if (project) {
            auto const found =
                std::ranges::find(project->cameras, mLaneMenuCameraId, &state::editing::model::CameraEntity::id);
            if (found != project->cameras.end()) camera = &*found;
        }
        if (!camera) {
            ImGui::CloseCurrentPopup();
        } else {
            bool const editable = !camera->locked;
            bool const occupied = camera->keysByTick.contains(mLaneMenuTick);
            if (ImGui::MenuItem(
                    "playback.refactorEditor.timeline.addKeyframeHere"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !occupied
                )) {
                EditorAction action{EditorActionType::AddCameraKeyframe};
                action.id   = camera->id;
                action.tick = mLaneMenuTick;
                ctx.submitAction(std::move(action));
                ctx.selection.select(state::editing::model::SelectedKeyframe{camera->id, mLaneMenuTick});
            }
            if (ImGui::MenuItem(
                    mKeyframeClipboard.size() > 1
                        ? "playback.refactorEditor.timeline.pasteKeyframes"_tr(mKeyframeClipboard.size()).c_str()
                        : "playback.refactorEditor.timeline.pasteKeyframe"_tr().c_str(),
                    nullptr,
                    false,
                    editable && !mKeyframeClipboard.empty()
                )) {
                pasteKeyframes(ctx, mLaneMenuTick, camera->id);
            }
            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) seekTo(ctx, mLaneMenuTick);
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-track-menu")) {
        {
            state::editing::model::CameraEntity const* camera = nullptr;
            if (project) {
                auto const found =
                    std::ranges::find(project->cameras, mTrackMenuCameraId, &state::editing::model::CameraEntity::id);
                if (found != project->cameras.end()) camera = &*found;
            }
            if (!camera) {
                ImGui::CloseCurrentPopup();
            } else {
                bool const editable = !camera->locked;
                if (mRenameCameraId != camera->id) {
                    mRenameCameraId = camera->id;
                    mRenameBuffer   = {};
                    camera->name.copy(mRenameBuffer.data(), mRenameBuffer.size() - 1);
                }
                ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.renameTrack"_tr().c_str());
                ImGui::SetNextItemWidth(160.0f * metrics::scale());
                ImGui::BeginDisabled(!editable);
                bool const renameEntered = ImGui::InputText(
                    "##timeline-track-rename",
                    mRenameBuffer.data(),
                    mRenameBuffer.size(),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
                );
                bool const renameCommitted = renameEntered || ImGui::IsItemDeactivatedAfterEdit();
                ImGui::EndDisabled();
                if (editable && renameCommitted) {
                    std::string name = mRenameBuffer.data();
                    auto const  first = name.find_first_not_of(" \t");
                    name = first == std::string::npos ? std::string{} : name.substr(first, name.find_last_not_of(" \t") - first + 1);
                    if (!name.empty() && name != camera->name) {
                        EditorAction action{EditorActionType::RenameCamera};
                        action.id   = camera->id;
                        action.name = std::move(name);
                        ctx.submitAction(std::move(action));
                    }
                    mRenameCameraId.clear();
                    if (renameEntered) ImGui::CloseCurrentPopup();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                        (camera->enabled ? "playback.refactorEditor.timeline.disableTrack"_tr()
                                         : "playback.refactorEditor.timeline.enableTrack"_tr())
                            .c_str()
                    )) {
                    EditorAction action{EditorActionType::SetCameraEnabled};
                    action.id    = camera->id;
                    action.value = !camera->enabled;
                    ctx.submitAction(std::move(action));
                }
                if (ImGui::MenuItem(
                        (camera->locked ? "playback.refactorEditor.timeline.unlockTrack"_tr()
                                        : "playback.refactorEditor.timeline.lockTrack"_tr())
                            .c_str()
                    )) {
                    EditorAction action{EditorActionType::SetCameraLocked};
                    action.id    = camera->id;
                    action.value = !camera->locked;
                    ctx.submitAction(std::move(action));
                }
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.previewCamera"_tr().c_str(),
                        nullptr,
                        false,
                        state::editing::model::isCameraRenderable(*camera)
                            && camera->kind == state::editing::model::CameraTrackKind::Keyframe
                    )) {
                    EditorAction action{EditorActionType::SetPreviewCamera};
                    action.id = camera->id;
                    ctx.submitAction(std::move(action));
                }
                ImGui::Separator();
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.clearKeyframes"_tr(camera->keysByTick.size()).c_str(),
                        nullptr,
                        false,
                        editable && !camera->keysByTick.empty()
                    )) {
                    EditorAction action{EditorActionType::ClearCameraKeyframes};
                    action.id = camera->id;
                    ctx.submitAction(std::move(action));
                    if (auto const* selected = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
                        selected && selected->trackId == camera->id) {
                        ctx.selection.select(state::editing::model::SelectedCamera{camera->id});
                    }
                }
                if (ImGui::MenuItem(
                        "playback.refactorEditor.timeline.deleteTrack"_tr().c_str(),
                        nullptr,
                        false,
                        editable && project->cameras.size() > 1
                    )) {
                    EditorAction action{EditorActionType::DeleteCamera};
                    action.id = camera->id;
                    ctx.submitAction(std::move(action));
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-ruler-menu")) {
        int const total = ctx.state.totalTicks;
        if (ImGui::MenuItem("playback.refactorEditor.timeline.seekHere"_tr().c_str())) seekTo(ctx, mRulerMenuTick);
        bool const markerFree =
            project
            && std::ranges::none_of(project->markers, [&](auto const& marker) { return marker.tick == mRulerMenuTick; });
        if (ImGui::MenuItem("playback.refactorEditor.timeline.addMarkerHere"_tr().c_str(), nullptr, false, markerFree)) {
            addMarkerAt(ctx, mRulerMenuTick);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("playback.refactorEditor.timeline.setInPoint"_tr().c_str(), nullptr, false, total > 0)) {
            int const tick = std::clamp(mRulerMenuTick, 0, std::max(0, total - 1));
            int       out  = ctx.commands.exportEndTick();
            if (out <= tick) out = total;
            ctx.commands.setExportStartTick(tick);
            ctx.commands.setExportEndTick(out);
        }
        if (ImGui::MenuItem("playback.refactorEditor.timeline.setOutPoint"_tr().c_str(), nullptr, false, total > 0)) {
            int const tick = std::clamp(mRulerMenuTick, std::min(1, total), total);
            int       in   = ctx.commands.exportStartTick();
            if (in < 0 || in >= tick) in = 0;
            ctx.commands.setExportStartTick(in);
            ctx.commands.setExportEndTick(tick);
        }
        bool const ranged = ctx.commands.exportStartTick() >= 0 || ctx.commands.exportEndTick() >= 0;
        if (ImGui::MenuItem("playback.refactorEditor.timeline.clearExportRange"_tr().c_str(), nullptr, false, ranged)) {
            ctx.commands.setExportStartTick(-1);
            ctx.commands.setExportEndTick(-1);
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-marker-menu")) {
        state::editing::model::Marker const* marker = nullptr;
        if (project) {
            auto const found = std::ranges::find(project->markers, mMarkerMenuId, &state::editing::model::Marker::id);
            if (found != project->markers.end()) marker = &*found;
        }
        if (!marker) {
            ImGui::CloseCurrentPopup();
        } else {
            if (mMarkerRenameId != marker->id) {
                mMarkerRenameId     = marker->id;
                mMarkerRenameBuffer = {};
                marker->label.copy(mMarkerRenameBuffer.data(), mMarkerRenameBuffer.size() - 1);
            }
            ImGui::TextDisabled("%s", "playback.refactorEditor.timeline.renameMarker"_tr().c_str());
            ImGui::SetNextItemWidth(160.0f * metrics::scale());
            bool const entered = ImGui::InputText(
                "##timeline-marker-rename",
                mMarkerRenameBuffer.data(),
                mMarkerRenameBuffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
            );
            if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
                std::string label = mMarkerRenameBuffer.data();
                if (!label.empty() && label != marker->label) {
                    EditorAction action{EditorActionType::RenameMarker};
                    action.id   = marker->id;
                    action.name = std::move(label);
                    ctx.submitAction(std::move(action));
                }
                mMarkerRenameId.clear();
                if (entered) ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("playback.refactorEditor.timeline.seekToMarker"_tr().c_str())) seekTo(ctx, marker->tick);
            if (ImGui::MenuItem("playback.refactorEditor.timeline.deleteMarker"_tr().c_str(), "Delete")) {
                EditorAction action{EditorActionType::DeleteMarker};
                action.id = marker->id;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-pose-menu")) {
        if (project) {
            auto const key = std::ranges::find(project->poseKeys, mPoseMenuKeyId, &state::editing::model::PoseKey::id);
            if (key != project->poseKeys.end()) {
                if (ImGui::MenuItem("playback.refactorEditor.timeline.seekToPoseKey"_tr().c_str())) {
                    seekTo(ctx, key->tick);
                }
                if (ImGui::MenuItem("playback.refactorEditor.timeline.deletePoseKey"_tr().c_str(), "Delete")) {
                    EditorAction action{EditorActionType::SetPoseKeys};
                    action.poseKeys = project->poseKeys;
                    std::erase_if(*action.poseKeys, [&](auto const& entry) { return entry.id == mPoseMenuKeyId; });
                    ctx.submitAction(std::move(action));
                    ctx.selection.clear();
                }
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-pose-lane-menu")) {
        if (ImGui::MenuItem("playback.refactorEditor.timeline.addPoseKeyHere"_tr().c_str())) {
            addPoseKeyAt(
                ctx,
                mPoseLaneMenuSubject,
                mPoseLaneMenuLabel,
                mPoseLaneMenuTick,
                std::string{state::editing::model::PoseKey::DefaultAnimation}
            );
        }
        if (ImGui::MenuItem("playback.refactorEditor.timeline.addInGameKeyHere"_tr().c_str())) {
            addPoseKeyAt(ctx, mPoseLaneMenuSubject, mPoseLaneMenuLabel, mPoseLaneMenuTick, {});
        }
        widgets::endAnchoredPopup();
    }
}

void TimelinePanel::drawLayerMenus(PanelContext const& ctx) {
    auto const* project = ctx.project();
    if (!project) return;

    if (widgets::beginAnchoredPopup("##timeline-overlay-menu")) {
        auto const* item = overlays::find(*project, mOverlayMenuId);
        if (!item) {
            ImGui::CloseCurrentPopup();
        } else {
            auto const index = static_cast<size_t>(item - project->overlays.data());
            auto const reorder = [&](size_t to) {
                auto layers = project->overlays;
                auto moved  = layers[index];
                layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(index));
                layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
                overlays::commit(ctx, std::move(layers));
            };
            if (ImGui::MenuItem("playback.refactorEditor.layers.duplicate"_tr().c_str())) {
                auto copy = overlays::makeLayer(*project, item->kind, item->startTick, item->shape.kind, item->shape.points);
                auto const id   = copy.id;
                auto const name = copy.name;
                copy            = *item;
                copy.id         = id;
                copy.name       = name;
                copy.locked     = false;
                auto layers     = project->overlays;
                layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(index) + 1, copy);
                overlays::commit(ctx, std::move(layers));
                queueOverlaySelection(id);
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.layers.bringForward"_tr().c_str(),
                    nullptr,
                    false,
                    index + 1 < project->overlays.size()
                )) {
                reorder(index + 1);
            }
            if (ImGui::MenuItem("playback.refactorEditor.layers.sendBackward"_tr().c_str(), nullptr, false, index > 0)) {
                reorder(index - 1);
            }
            if (ImGui::MenuItem("playback.refactorEditor.layers.trimToPlayhead"_tr().c_str(), nullptr, false, !item->locked)) {
                // Ends the layer at the playhead, or starts it there when the playhead is before it.
                auto changed = *item;
                int const tick = ctx.state.currentTick;
                if (tick > item->startTick) changed.endTick = tick;
                else if (tick < item->endTick) state::editing::model::setOverlayStart(changed, tick);
                overlays::commitItem(ctx, changed);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    (item->locked ? "playback.refactorEditor.timeline.unlockLayer"_tr()
                                  : "playback.refactorEditor.timeline.lockLayer"_tr())
                        .c_str()
                )) {
                auto changed   = *item;
                changed.locked = !item->locked;
                overlays::commitItem(ctx, changed);
            }
            if (ImGui::MenuItem("playback.refactorEditor.layers.delete"_tr().c_str(), "Del", false, !item->locked)) {
                auto layers = project->overlays;
                layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(index));
                overlays::commit(ctx, std::move(layers));
                ctx.selection.clear();
            }
        }
        widgets::endAnchoredPopup();
    }

    if (widgets::beginAnchoredPopup("##timeline-segment-menu")) {
        auto const& segments = project->worldActor.segments;
        auto const  segment  = std::ranges::find(segments, mSegmentMenuId, &state::editing::model::WorldActorSegment::id);
        if (segment == segments.end()) {
            ImGui::CloseCurrentPopup();
        } else {
            int const  playhead = ctx.state.currentTick;
            bool const canSplitAtPlayhead =
                !segment->locked && playhead > segment->startTick && playhead < segment->endTick;
            bool const canSplitHere =
                !segment->locked && mSegmentMenuTick > segment->startTick && mSegmentMenuTick < segment->endTick;
            if (ImGui::MenuItem("playback.refactorEditor.timeline.cutAtPlayhead"_tr().c_str(), nullptr, false, canSplitAtPlayhead)) {
                EditorAction action{EditorActionType::SplitWorldActor};
                action.tick = playhead;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem("playback.refactorEditor.timeline.cutHere"_tr().c_str(), nullptr, false, canSplitHere)) {
                EditorAction action{EditorActionType::SplitWorldActor};
                action.tick = mSegmentMenuTick;
                ctx.submitAction(std::move(action));
            }
            if (ImGui::MenuItem(
                    "playback.refactorEditor.details.cutSegment"_tr().c_str(),
                    "Del",
                    false,
                    !segment->locked && segments.size() > 1
                )) {
                EditorAction action{EditorActionType::DeleteWorldActorSegment};
                action.id = segment->id;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
            ImGui::Separator();
            bool const locked = segment->locked;
            if (ImGui::MenuItem(
                    (locked ? "playback.refactorEditor.timeline.unlockLayer"_tr()
                            : "playback.refactorEditor.timeline.lockLayer"_tr())
                        .c_str()
                )) {
                EditorAction action{EditorActionType::SetWorldActorLocked};
                action.value = !locked;
                ctx.submitAction(std::move(action));
            }
        }
        widgets::endAnchoredPopup();
    }
}

void TimelinePanel::drawRangeBar(PanelContext const& ctx, Layout const& layout, bool allowInput) {
    auto const& state     = ctx.state;
    auto*       drawList  = ImGui::GetWindowDrawList();
    float const barTop    = layout.bodyBottom + 1.0f;
    float const barLeft   = layout.canvasLeft;
    float const barWidth  = layout.canvasWidth;
    float const barHeight = layout.fullMax.y - barTop;
    if (barWidth <= 8.0f || barHeight <= 4.0f) return;

    drawList->AddRectFilled({layout.fullMin.x, barTop}, {layout.fullMax.x, layout.fullMax.y}, theme::kBgHeader);

    // The thumb is the visible window over the whole replay: its end handles scale, the body pans.
    float const windowSpan  = std::clamp(1.0f / std::max(kMinZoomScale, mZoomScale), 0.02f, 1.0f);
    float const maxScroll   = maxScrollFor(layout.canvasWidth, mZoomScale, state.totalTicks);
    float const windowStart = maxScroll > 0.0f ? (mScrollX / maxScroll) * (1.0f - windowSpan) : 0.0f;
    float const uiScale     = metrics::scale();
    float const centreY     = (barTop + layout.fullMax.y) * 0.5f;
    // Premiere style: a thin bar whose ends carry ring handles centred on the window edges.
    float const radius    = std::max(4.0f, std::min(barHeight, metrics::rangeBar()) * 0.26f);
    float const thumbH    = std::max(3.0f, radius * 1.1f);
    float const top       = centreY - thumbH * 0.5f;
    float const bottom    = centreY + thumbH * 0.5f;
    float const rounding  = thumbH * 0.5f;
    float const ring      = std::max(1.5f, 1.5f * uiScale);
    float const handleW   = radius + 2.0f * uiScale;
    float const minThumb  = handleW * 2.0f + 6.0f * uiScale;
    float const railLeft  = barLeft + radius + ring;
    float const railWidth = barWidth - (radius + ring) * 2.0f;
    if (railWidth <= minThumb) {
        mRangeDragMode = 0;
        return;
    }

    // Deep zoom still draws the thumb wide enough to hold both handles.
    float winLeft  = railLeft + windowStart * railWidth;
    float winRight = winLeft + windowSpan * railWidth;
    if (winRight - winLeft < minThumb) {
        float const centre =
            std::clamp((winLeft + winRight) * 0.5f, railLeft + minThumb * 0.5f, railLeft + railWidth - minThumb * 0.5f);
        winLeft  = centre - minThumb * 0.5f;
        winRight = centre + minThumb * 0.5f;
    }

    ImGui::SetCursorScreenPos({barLeft, barTop});
    ImGui::InvisibleButton("##timeline-range", {barWidth, barHeight});
    ImVec2 const mouse      = ImGui::GetMousePos();
    bool const   barHovered = ImGui::IsItemHovered();
    bool const   nearLeft   = barHovered && std::abs(mouse.x - winLeft) <= handleW;
    bool const   nearRight  = barHovered && !nearLeft && std::abs(mouse.x - winRight) <= handleW;
    bool const   overBody   = barHovered && !nearLeft && !nearRight && mouse.x > winLeft && mouse.x < winRight;
    bool const   idle       = mRangeDragMode == 0;
    float const  leftAmount =
        mAnimator.animate("range-bar", "left", mRangeDragMode == 1 || (idle && nearLeft) ? 1.0f : 0.0f);
    float const rightAmount =
        mAnimator.animate("range-bar", "right", mRangeDragMode == 2 || (idle && nearRight) ? 1.0f : 0.0f);
    // Half way is hover, full is dragging, so each state gets its own colour.
    float const bodyAmount =
        mAnimator.animate("range-bar", "body", mRangeDragMode == 3 ? 1.0f : (idle && overBody ? 0.5f : 0.0f));

    drawList->AddRectFilled({railLeft, top}, {railLeft + railWidth, bottom}, theme::kRangeTrack, rounding);
    ImU32 const thumbColor = bodyAmount <= 0.5f
                               ? lerpColor(theme::kRangeThumb, theme::kRangeThumbHover, bodyAmount * 2.0f)
                               : lerpColor(theme::kRangeThumbHover, theme::kRangeThumbActive, bodyAmount * 2.0f - 1.0f);
    drawList->AddRectFilled({winLeft, top}, {winRight, bottom}, thumbColor, rounding);

    // Hollow rings: the panel colour fills the centre so the bar appears to pass behind them.
    auto const handle = [&](float centreX, float amount) {
        float const r = radius * (1.0f + 0.15f * amount);
        drawList->AddCircleFilled({centreX, centreY}, r, theme::kBgHeader);
        drawList->AddCircle(
            {centreX, centreY},
            r - ring * 0.5f,
            lerpColor(theme::kRangeHandle, theme::kIconHighlight, amount),
            0,
            ring
        );
    };
    handle(winLeft, leftAmount);
    handle(winRight, rightAmount);

    if (!allowInput) {
        mRangeDragMode = 0;
        return;
    }
    if (barHovered || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGui::IsItemActive() ? theme::kCursorGrabbing : theme::kCursorGrab);
    }
    if (ImGui::IsItemActivated()) {
        if (nearLeft) mRangeDragMode = 1;
        else if (nearRight) mRangeDragMode = 2;
        else if (mouse.x > winLeft && mouse.x < winRight) mRangeDragMode = 3;
        else mRangeDragMode = 4;
        mRangeDragOriginX      = mouse.x;
        mRangeDragOriginScroll = mScrollX;
        mRangeDragOriginZoom   = mZoomScale;
    }
    if (ImGui::IsItemDeactivated()) mRangeDragMode = 0;
    if (!ImGui::IsItemActive() || mRangeDragMode == 0) {
        if (ImGui::IsItemDeactivated() && mRangeDragMode == 4) resetZoom();
        return;
    }

    float const spanBefore      = std::clamp(1.0f / std::max(kMinZoomScale, mRangeDragOriginZoom), 0.02f, 1.0f);
    float const maxScrollBefore = maxScrollFor(layout.canvasWidth, mRangeDragOriginZoom, state.totalTicks);
    float const startBefore =
        maxScrollBefore > 0.0f ? (mRangeDragOriginScroll / maxScrollBefore) * (1.0f - spanBefore) : 0.0f;
    float const delta = (mouse.x - mRangeDragOriginX) / railWidth;

    float newStart = startBefore;
    float newSpan  = spanBefore;
    if (mRangeDragMode == 1) {
        newStart = std::clamp(startBefore + delta, 0.0f, startBefore + spanBefore - 0.02f);
        newSpan  = startBefore + spanBefore - newStart;
    } else if (mRangeDragMode == 2) {
        newSpan = std::clamp(spanBefore + delta, 0.02f, 1.0f - startBefore);
    } else if (mRangeDragMode == 3) {
        newStart = std::clamp(startBefore + delta, 0.0f, 1.0f - spanBefore);
    } else {
        return;
    }

    mZoomScale               = std::clamp(1.0f / std::max(0.02f, newSpan), kMinZoomScale, kMaxZoomScale);
    float const clampedSpan  = std::clamp(1.0f / mZoomScale, 0.02f, 1.0f);
    float const maxScrollNow = maxScrollFor(layout.canvasWidth, mZoomScale, state.totalTicks);
    float const startSpan    = std::max(0.0001f, 1.0f - clampedSpan);
    mScrollX                 = std::clamp(newStart / startSpan * maxScrollNow, 0.0f, maxScrollNow);
}

void TimelinePanel::draw(PanelContext const& ctx, bool allowInput) {
    auto const& state   = ctx.state;
    auto const  project = state.project;
    if (!project) {
        widgets::noActiveProjectPlaceholder();
        return;
    }

    if (!allowInput) {
        mRulerDragTick    = -1;
        mDraggingPlayhead = false;
        mDraggingKeyframeCameraId.clear();
        mDraggingKeyframeStartTick = -1;
        mDraggingKeyframeMoved     = false;
        mRangeDragMode             = 0;
        mGroupDrag                 = false;
        mGroupDragDelta            = 0;
        mBoxPending                = false;
        mBoxSelecting              = false;
        mMarkerDragId.clear();
        mMarkerDragMoved = false;
        mPoseDragId.clear();
        mPoseDragMoved = false;
        mClipDragId.clear();
        mClipDragMoved = false;
    }
    mSnapGuideTick = -1;
    applyPendingSelection(ctx);

    mAnimator.beginFrame();
    mTrackTree.setSearch(mTrackSearch);
    mTrackTree.rebuild(*project, mExpandedRows);
    // A row opens on its own when a property of it gets its first key, so the new key shows on the timeline at once;
    // one the user collapses stays collapsed until its keys are all gone.
    {
        bool opened = false;
        std::set<std::string> expandable;
        for (auto const& row : mTrackTree.rows()) {
            if (!row.expandable) continue;
            expandable.insert(row.id);
            if (!mExpandableRows.contains(row.id) && !row.expanded) opened = mExpandedRows.insert(row.id).second || opened;
        }
        mExpandableRows = std::move(expandable);
        if (opened) mTrackTree.rebuild(*project, mExpandedRows);
    }
    int displayTick = mPendingSeekTick >= 0 ? mPendingSeekTick : state.currentTick;
    if (mRulerDragTick >= 0) displayTick = mRulerDragTick;
    if (mPendingSeekTick >= 0 && state.currentTick == mPendingSeekTick) mPendingSeekTick = -1;

    ImVec2 const fullMin   = ImGui::GetCursorScreenPos();
    ImVec2 const available = ImGui::GetContentRegionAvail();
    if (available.x < 220.0f || available.y < 120.0f) return;

    Layout layout;
    layout.fullMin     = fullMin;
    layout.fullMax     = {fullMin.x + available.x, fullMin.y + available.y};
    layout.titleBottom = fullMin.y + metrics::toolbarRow();
    layout.rulerTop    = layout.titleBottom;
    // The band beside the ruler holds the add button and search field, so it follows the control height.
    layout.headerHeight = std::max(metrics::ruler(), metrics::iconButton() + 4.0f);
    layout.bodyTop      = layout.rulerTop + layout.headerHeight;
    layout.bodyBottom   = layout.fullMax.y - metrics::rangeBar();

    float const fontSize         = ImGui::GetFontSize();
    float const minimumListWidth = std::max(fontSize * 10.0f, 140.0f);
    layout.listWidth             = std::clamp(
        available.x * mTrackListWidthRatio,
        minimumListWidth,
        available.x - std::max(fontSize * 12.0f, 180.0f)
    );
    layout.canvasLeft  = fullMin.x + layout.listWidth + metrics::splitter();
    layout.canvasWidth = layout.fullMax.x - layout.canvasLeft;

    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(fullMin, layout.fullMax, theme::kTimelineBg);

    drawTitleRow(ctx, layout, displayTick, allowInput);

    float const fitPixelsPerTick = layout.canvasWidth / static_cast<float>(std::max(1, state.totalTicks));
    float const pixelsPerTick    = fitPixelsPerTick * mZoomScale;
    float const maxScroll        = std::max(0.0f, state.totalTicks * pixelsPerTick - layout.canvasWidth);
    mScrollX                     = std::clamp(mScrollX, 0.0f, maxScroll);

    TimelineScale const scale{layout.canvasLeft, layout.canvasWidth, pixelsPerTick, mScrollX, state.totalTicks};

    float trackContentHeight = 0.0f;
    for (auto const& row : mTrackTree.rows()) trackContentHeight += row.height;
    float const visibleTrackHeight = std::max(0.0f, layout.bodyBottom - layout.bodyTop);
    float const maxScrollY         = std::max(0.0f, trackContentHeight - visibleTrackHeight);
    mScrollY                       = std::clamp(mScrollY, 0.0f, maxScrollY);

    // The band is the gap between the track list and the canvas; the child windows on either side would take its hover.
    ImVec2 const listSplitterMin{fullMin.x + layout.listWidth, layout.rulerTop};
    ImVec2 const listSplitterMax{layout.canvasLeft, layout.bodyBottom};
    ImGui::SetCursorScreenPos(listSplitterMin);
    ImGui::InvisibleButton(
        "##timeline-list-splitter",
        {listSplitterMax.x - listSplitterMin.x, listSplitterMax.y - listSplitterMin.y}
    );
    bool const listSplitterHovered = ImGui::IsItemHovered();
    bool const listSplitterActive  = ImGui::IsItemActive();
    if (listSplitterHovered || listSplitterActive) {
        ImGui::SetMouseCursor(listSplitterActive ? theme::kCursorGrabbing : theme::kCursorGrab);
    }
    if (allowInput && listSplitterActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        float const listRight = ImGui::GetMousePos().x - metrics::splitter() * 0.5f;
        mTrackListWidthRatio  = std::clamp((listRight - fullMin.x) / available.x, 0.18f, 0.55f);
    }

    drawTrackHeaders(ctx, layout, allowInput);
    if (listSplitterHovered || listSplitterActive) {
        drawList->AddRectFilled(
            listSplitterMin,
            listSplitterMax,
            listSplitterActive ? theme::kAccent : theme::withAlpha(theme::kAccent, 0xa0)
        );
    }

    drawList
        ->AddRectFilled({layout.canvasLeft, layout.bodyTop}, {layout.fullMax.x, layout.bodyBottom}, theme::kTimelineBg);
    drawList->PushClipRect({layout.canvasLeft, layout.rulerTop}, {layout.fullMax.x, layout.bodyBottom}, true);
    drawRuler(ctx, layout, scale, displayTick);

    ImGui::SetCursorScreenPos({layout.canvasLeft, layout.rulerTop});
    ImGui::InvisibleButton("##timeline-ruler", {layout.canvasWidth, layout.bodyTop - layout.rulerTop});
    // A marker pin under the cursor takes the press, so markers can be dragged without scrubbing.
    std::string const hoveredMarker =
        ImGui::IsItemHovered() ? markerAt(ctx, layout, scale, ImGui::GetMousePos()) : std::string{};
    if (allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        if (!hoveredMarker.empty()) {
            auto const marker = std::ranges::find(project->markers, hoveredMarker, &state::editing::model::Marker::id);
            ctx.selection.select(state::editing::model::SelectedMarker{hoveredMarker});
            mMarkerMenuId   = hoveredMarker;
            mMarkerRenameId.clear();
            float const x = scale.xAt(static_cast<float>(marker->tick));
            widgets::openAnchoredPopup("##timeline-marker-menu", {x, layout.rulerTop}, {x, layout.bodyTop});
        } else {
            mRulerMenuTick = std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks);
            ImVec2 const anchor{scale.xAt(static_cast<float>(mRulerMenuTick)), layout.rulerTop};
            widgets::openAnchoredPopup("##timeline-ruler-menu", anchor, {anchor.x, layout.bodyTop});
        }
    }
    if (allowInput && ImGui::IsItemClicked(ImGuiMouseButton_Left) && !hoveredMarker.empty()) {
        auto const marker = std::ranges::find(project->markers, hoveredMarker, &state::editing::model::Marker::id);
        ctx.selection.select(state::editing::model::SelectedMarker{hoveredMarker});
        mMarkerDragId     = hoveredMarker;
        mMarkerDragStartX = ImGui::GetMousePos().x;
        mMarkerDragTick   = marker->tick;
        mMarkerDragMoved  = false;
    }
    if (!hoveredMarker.empty() || !mMarkerDragId.empty()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (allowInput && mMarkerDragId.empty() && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        mRulerDragTick = std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks);
        if (ImGui::GetIO().KeyShift) {
            mRulerDragTick = snapToEditPoints(ctx, mRulerDragTick, scale.pixelsPerTick, false, true, {}, {});
        }
        displayTick = mRulerDragTick;
    }
    if (allowInput && ImGui::IsItemDeactivated()) {
        if (mRulerDragTick >= 0) submitSeek(ctx, mRulerDragTick);
        mRulerDragTick = -1;
    }
    if (allowInput && !mMarkerDragId.empty()) {
        auto const marker = std::ranges::find(project->markers, mMarkerDragId, &state::editing::model::Marker::id);
        if (marker == project->markers.end()) {
            mMarkerDragId.clear();
        } else {
            if (std::abs(ImGui::GetMousePos().x - mMarkerDragStartX) >= 2.0f) mMarkerDragMoved = true;
            if (mMarkerDragMoved) {
                int tick = std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks);
                if (ImGui::GetIO().KeyShift) {
                    tick = snapToEditPoints(ctx, tick, scale.pixelsPerTick, true, true, {}, mMarkerDragId);
                }
                // Never land on another marker's tick; the command would reject it anyway.
                if (std::ranges::any_of(project->markers, [&](auto const& other) {
                        return other.id != mMarkerDragId && other.tick == tick;
                    })) {
                    tick = mMarkerDragTick;
                }
                mMarkerDragTick = tick;
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (!mMarkerDragMoved) {
                    submitSeek(ctx, marker->tick);
                } else if (mMarkerDragTick != marker->tick) {
                    EditorAction action{EditorActionType::MoveMarker};
                    action.id   = mMarkerDragId;
                    action.tick = mMarkerDragTick;
                    ctx.submitAction(std::move(action));
                }
                mMarkerDragId.clear();
                mMarkerDragMoved = false;
            }
        }
    }

    auto tickFromMouse = [&] { return std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks); };
    auto snapTick      = [&](int tick) {
        tick = std::clamp(tick, 0, state.totalTicks);
        if (!mSnapEnabled) return tick;
        constexpr float GridTicks = static_cast<float>(kTicksPerSecond);
        return std::clamp(
            static_cast<int>(std::round(static_cast<float>(tick) / GridTicks) * GridTicks),
            0,
            state.totalTicks
        );
    };
    auto boundedKeyframeTick = [&](int tick) {
        auto const camera =
            std::ranges::find(project->cameras, mDraggingKeyframeCameraId, &state::editing::model::CameraEntity::id);
        if (camera == project->cameras.end()) return tick;
        auto const key = camera->keysByTick.find(mDraggingKeyframeStartTick);
        if (key == camera->keysByTick.end()) return tick;

        int minimum = 0;
        int maximum = state.totalTicks;
        if (key != camera->keysByTick.begin()) minimum = std::min(state.totalTicks, std::prev(key)->first + 1);
        auto const next = std::next(key);
        if (next != camera->keysByTick.end()) maximum = std::max(0, next->first - 1);
        if (minimum > maximum) return mDraggingKeyframeStartTick;
        return std::clamp(tick, minimum, maximum);
    };

    // Box select only engages once the press has travelled a few pixels, so plain clicks still seek.
    if (allowInput && mBoxPending && !mBoxSelecting) {
        ImVec2 const now = ImGui::GetMousePos();
        if (std::abs(now.x - mBoxStart.x) + std::abs(now.y - mBoxStart.y) >= kBoxDragThreshold * metrics::scale()) {
            mBoxSelecting = true;
        }
    }
    ImVec2 const boxMouse = ImGui::GetMousePos();
    ImVec2 const boxMin{std::min(mBoxStart.x, boxMouse.x), std::min(mBoxStart.y, boxMouse.y)};
    ImVec2 const boxMax{std::max(mBoxStart.x, boxMouse.x), std::max(mBoxStart.y, boxMouse.y)};
    auto const   boxContains = [&](float x, float centerY) {
        float const reach = metrics::keyframeRadius();
        return x + reach >= boxMin.x && x - reach <= boxMax.x && centerY + reach >= boxMin.y
            && centerY - reach <= boxMax.y;
    };
    std::vector<state::editing::model::SelectedKeyframe> boxHits;

    auto const* selectedKeyframe = ctx.selection.getAs<state::editing::model::SelectedKeyframe>();
    float const keyRadius        = metrics::keyframeRadius();
    float       y                = layout.bodyTop - mScrollY;
    bool        clickConsumed    = false;
    int         bandIndex        = 0;
    // Alternating bands continue past the last track so the empty area still reads as timeline lanes.
    {
        float const bandHeight = metrics::cameraRow();
        int         band       = 0;
        for (float bandY = layout.bodyTop - mScrollY; bandY < layout.bodyBottom; bandY += bandHeight, ++band) {
            if (bandY + bandHeight < layout.bodyTop) continue;
            if (band % 2 == 0) continue;
            drawList->AddRectFilled(
                {layout.canvasLeft, std::max(bandY, layout.bodyTop)},
                {layout.fullMax.x, std::min(bandY + bandHeight, layout.bodyBottom)},
                theme::kTimelineTrackBg
            );
        }
    }
    for (auto const& row : mTrackTree.rows()) {
        float const rowBottom = y + row.height;
        if (rowBottom < layout.bodyTop || y > layout.bodyBottom) {
            y = rowBottom;
            ++bandIndex;
            continue;
        }
        float const centerY = (y + rowBottom) * 0.5f;
        // Rows carry their own band so real tracks stay distinct from the filler bands underneath.
        drawList->AddRectFilled(
            {layout.canvasLeft, std::max(y, layout.bodyTop)},
            {layout.fullMax.x, std::min(rowBottom, layout.bodyBottom)},
            bandIndex % 2 == 0 ? theme::kTimelineBg : theme::kTimelineTrackBg
        );
        drawList->AddLine({layout.canvasLeft, rowBottom}, {layout.fullMax.x, rowBottom}, IM_COL32(0, 0, 0, 64));
        ++bandIndex;

        if (row.kind == TrackRowKind::Camera && row.cameraIndex >= 0
            && row.cameraIndex < static_cast<int>(project->cameras.size())) {
            auto const& camera = project->cameras[row.cameraIndex];
            bool const  cameraSelected =
                (ctx.selection.getAs<state::editing::model::SelectedCamera>()
                 && ctx.selection.getAs<state::editing::model::SelectedCamera>()->cameraId == camera.id)
                || (selectedKeyframe && selectedKeyframe->trackId == camera.id);
            ImU32 const trackColor = theme::trackColor(row.cameraIndex);
            {
                float const amount = mAnimator.animate("canvas-selected", camera.id, cameraSelected ? 1.0f : 0.0f);
                ImU32 const fill   = camera.enabled ? lerpColor(
                                                        theme::withAlpha(trackColor, theme::kTrackFillAlpha),
                                                        theme::withAlpha(trackColor, theme::kTrackFillSelectedAlpha),
                                                        amount
                                                    )
                                                    : theme::withAlpha(theme::kTrackDisabled, 0x80);
                drawList->AddRectFilled({layout.canvasLeft, y + 3.0f}, {layout.fullMax.x, rowBottom - 3.0f}, fill);
            }

            for (auto const& [keyTick, key] : camera.keysByTick) {
                bool const selected = ctx.selection.isKeyframeSelected(camera.id, keyTick);
                bool const dragging = mDraggingKeyframeCameraId == camera.id && mDraggingKeyframeStartTick == keyTick;
                int        drawnTick = keyTick;
                if (mGroupDrag && selected) drawnTick = keyTick + mGroupDragDelta;
                else if (!mGroupDrag && dragging) drawnTick = mDraggingKeyframeTick;
                float const x = scale.xAt(static_cast<float>(drawnTick));
                if (x < layout.canvasLeft - keyRadius || x > layout.fullMax.x + keyRadius) continue;
                bool const inBox = mBoxSelecting && !camera.locked && boxContains(x, centerY);
                if (inBox) boxHits.push_back({camera.id, keyTick});
                bool const hold = key.interpolationType == state::editing::model::CameraInterpolationType::Hold;

                bool const  lit     = selected || inBox;
                float const radius  = lit ? keyRadius + 1.0f : keyRadius;
                ImU32 const fill    = !camera.enabled ? theme::kKeyframeDisabled
                                    : lit             ? theme::kKeyframeSelected
                                                      : theme::kKeyframe;
                ImU32 const outline = lit ? theme::kIconHighlight : theme::kKeyframeOutline;
                if (hold) drawKeyframeSquare(drawList, x, centerY, radius, fill, outline);
                else drawKeyframeDiamond(drawList, x, centerY, radius, fill, outline);
                ImVec2 const keyMouse = ImGui::GetMousePos();
                bool const   overKey  = std::abs(keyMouse.x - x) <= radius + 3.0f && keyMouse.y >= y
                                   && keyMouse.y <= rowBottom;
                if (allowInput && !clickConsumed && overKey && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed            = true;
                    mKeyframeMenuCameraId    = camera.id;
                    mKeyframeMenuTick        = keyTick;
                    mKeyframeMenuTickEdit    = keyTick;
                    // Right-clicking inside a multi-selection keeps it so the menu can act on the whole set.
                    mKeyframeMenuGroup = selected && ctx.selection.keyframeCount() > 1;
                    if (!mKeyframeMenuGroup) {
                        ctx.selection.select(state::editing::model::SelectedKeyframe{camera.id, keyTick});
                    }
                    widgets::openAnchoredPopup(
                        "##timeline-keyframe-menu",
                        {x - radius, centerY - radius},
                        {x + radius, centerY + radius}
                    );
                }
                if (allowInput && !clickConsumed && overKey && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                    && ImGui::GetIO().KeyCtrl && !camera.locked) {
                    clickConsumed = true;
                    ctx.selection.toggleKeyframe({camera.id, keyTick});
                }
                if (allowInput && !clickConsumed && overKey && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                    && selected && ctx.selection.keyframeCount() > 1) {
                    // Grabbing a member of the set moves the set; a click without movement narrows to this key.
                    clickConsumed                = true;
                    mGroupDrag                   = true;
                    mGroupDragDelta              = 0;
                    mDraggingKeyframeCameraId    = camera.id;
                    mDraggingKeyframeStartMouseX = ImGui::GetMousePos().x;
                    mDraggingKeyframeStartTick   = keyTick;
                    mDraggingKeyframeTick        = keyTick;
                    mDraggingKeyframeMoved       = false;
                }
                if (allowInput && !clickConsumed && overKey && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    ctx.selection.select(state::editing::model::SelectedKeyframe{camera.id, keyTick});
                    EditorAction previewAction{EditorActionType::SetPreviewCamera};
                    previewAction.id = camera.id;
                    ctx.submitAction(std::move(previewAction));
                    if (camera.locked || !camera.enabled) {
                        submitSeek(ctx, keyTick);
                    } else {
                        mDraggingKeyframeCameraId    = camera.id;
                        mDraggingKeyframeStartMouseX = ImGui::GetMousePos().x;
                        mDraggingKeyframeStartTick   = keyTick;
                        mDraggingKeyframeTick        = keyTick;
                        mDraggingKeyframeMoved       = false;
                    }
                }
            }

            // Runs after the keyframe pass so a keyframe hit always wins over the row.
            if (row.kind == TrackRowKind::Camera && allowInput && !clickConsumed
                && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                && contains(
                    {layout.canvasLeft, y + 3.0f},
                    {layout.fullMax.x, rowBottom - 3.0f},
                    ImGui::GetMousePos()
                )) {
                clickConsumed = true;
                int tick      = std::clamp(scale.tickAt(ImGui::GetMousePos().x), 0, state.totalTicks);
                if (mSnapEnabled) {
                    tick = static_cast<int>(std::round(static_cast<float>(tick) / kTicksPerSecond)) * kTicksPerSecond;
                    tick = std::clamp(tick, 0, state.totalTicks);
                }
                mLaneMenuCameraId = camera.id;
                mLaneMenuTick     = tick;
                float const anchorX = scale.xAt(static_cast<float>(tick));
                widgets::openAnchoredPopup(
                    "##timeline-lane-menu",
                    {anchorX, y + 3.0f},
                    {anchorX, rowBottom - 3.0f}
                );
            }
            if (row.kind == TrackRowKind::Camera && allowInput && !clickConsumed
                && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && contains(
                    {layout.canvasLeft, y + 3.0f},
                    {layout.fullMax.x, rowBottom - 3.0f},
                    ImGui::GetMousePos()
                )) {
                // Resolved on release: a click selects the track and seeks, a drag becomes a box select.
                clickConsumed     = true;
                mBoxPending       = true;
                mBoxSelecting     = false;
                mBoxAdditive      = ImGui::GetIO().KeyCtrl;
                mBoxStart         = ImGui::GetMousePos();
                mBoxClickCameraId = camera.id;
            }
        }
        if (row.kind == TrackRowKind::Pose) {
            using state::editing::model::PoseKey;
            auto const* selectedPose = ctx.selection.getAs<state::editing::model::SelectedPoseKey>();
            ImU32 const laneColor    = theme::trackColor(bandIndex);
            std::vector<PoseKey const*> laneKeys;
            for (auto const& key : project->poseKeys) {
                if (key.subject == row.poseSubject) laneKeys.push_back(&key);
            }
            auto const shownTick = [&](PoseKey const& key) {
                return mPoseDragId == key.id && mPoseDragMoved ? mPoseDragTick : key.tick;
            };
            std::ranges::stable_sort(laneKeys, {}, [&](PoseKey const* key) { return shownTick(*key); });
            // A pose holds until the entity's next key; the stretch it covers is filled.
            for (size_t index = 0; index < laneKeys.size(); ++index) {
                if (laneKeys[index]->animation.empty()) continue;
                float const startX = scale.xAt(static_cast<float>(shownTick(*laneKeys[index])));
                float const endX   = index + 1 < laneKeys.size()
                                       ? scale.xAt(static_cast<float>(shownTick(*laneKeys[index + 1])))
                                       : scale.xAt(static_cast<float>(state.totalTicks));
                if (endX < layout.canvasLeft || startX > layout.fullMax.x) continue;
                drawList->AddRectFilled(
                    {std::max(startX, layout.canvasLeft), y + 3.0f},
                    {std::min(endX, layout.fullMax.x), rowBottom - 3.0f},
                    theme::withAlpha(laneColor, theme::kTrackFillSelectedAlpha)
                );
            }
            ImVec2 const mouse = ImGui::GetMousePos();
            for (auto const* entry : laneKeys) {
                auto const& key = *entry;
                float const x   = scale.xAt(static_cast<float>(shownTick(key)));
                if (x < layout.canvasLeft - keyRadius || x > layout.fullMax.x + keyRadius) continue;
                bool const  selected = selectedPose && selectedPose->poseKeyId == key.id;
                float const radius   = selected ? keyRadius + 1.0f : keyRadius;
                ImU32 const fill     = selected ? theme::kKeyframeSelected : theme::kKeyframe;
                ImU32 const outline  = selected ? theme::kIconHighlight : theme::kKeyframeOutline;
                // A square hands the entity back to its recorded animation.
                if (key.animation.empty()) drawKeyframeSquare(drawList, x, centerY, radius, fill, outline);
                else drawKeyframeDiamond(drawList, x, centerY, radius, fill, outline);
                bool const overKey = std::abs(mouse.x - x) <= radius + 3.0f && mouse.y >= y && mouse.y <= rowBottom;
                if (allowInput && !clickConsumed && overKey && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clickConsumed = true;
                    ctx.selection.select(state::editing::model::SelectedPoseKey{key.id});
                    mPoseMenuKeyId = key.id;
                    widgets::openAnchoredPopup(
                        "##timeline-pose-menu",
                        {x - radius, centerY - radius},
                        {x + radius, centerY + radius}
                    );
                }
                if (allowInput && !clickConsumed && overKey && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    clickConsumed = true;
                    ctx.selection.select(state::editing::model::SelectedPoseKey{key.id});
                    mPoseDragId     = key.id;
                    mPoseDragStartX = mouse.x;
                    mPoseDragTick   = key.tick;
                    mPoseDragMoved  = false;
                }
            }
            if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                && contains({layout.canvasLeft, y + 3.0f}, {layout.fullMax.x, rowBottom - 3.0f}, mouse)) {
                clickConsumed        = true;
                mPoseLaneMenuSubject = row.poseSubject;
                mPoseLaneMenuLabel   = row.name;
                mPoseLaneMenuTick    = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                float const anchorX  = scale.xAt(static_cast<float>(mPoseLaneMenuTick));
                widgets::openAnchoredPopup("##timeline-pose-lane-menu", {anchorX, y + 3.0f}, {anchorX, rowBottom - 3.0f});
            }
        }
        // Layer and replay bars: the body moves a layer, an edge trims a layer or a replay segment.
        if (row.kind == TrackRowKind::Overlay || row.kind == TrackRowKind::Replay) {
            bool const   replayLane = row.kind == TrackRowKind::Replay;
            ImVec2 const mouse      = ImGui::GetMousePos();
            float const  edgeGrab   = kEdgeGrabPixels * metrics::scale();
            float const  barTop     = y + 3.0f;
            float const  barBottom  = rowBottom - 3.0f;
            auto const   barHit     = [&](float x0, float x1, std::string const& id, int start, int end, bool locked) {
                if (!allowInput || clickConsumed || mouse.y < barTop || mouse.y > barBottom) return;
                if (mouse.x < x0 - edgeGrab * 0.5f || mouse.x > x1 + edgeGrab * 0.5f) return;
                int const mode = !locked && std::abs(mouse.x - x0) <= edgeGrab   ? 1
                               : !locked && std::abs(mouse.x - x1) <= edgeGrab   ? 2
                                                                                 : 0;
                if (mode != 0 && mClipDragId.empty()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                bool const left  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                bool const right = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
                if (!left && !right) return;
                clickConsumed = true;
                if (replayLane) ctx.selection.select(state::editing::model::SelectedWorldActorSegment{id});
                else ctx.selection.select(state::editing::model::SelectedOverlay{id});
                if (right) {
                    float const anchorX = std::clamp(mouse.x, x0, x1);
                    if (replayLane) {
                        mSegmentMenuId   = id;
                        mSegmentMenuTick = std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks);
                        widgets::openAnchoredPopup("##timeline-segment-menu", {anchorX, barTop}, {anchorX, barBottom});
                    } else {
                        mOverlayMenuId = id;
                        widgets::openAnchoredPopup("##timeline-overlay-menu", {anchorX, barTop}, {anchorX, barBottom});
                    }
                    return;
                }
                // Replay segments play where they sit, so only their edges move; a click on one seeks there.
                if (locked && !replayLane) return;
                mClipDragId          = id;
                mClipDragReplay      = replayLane;
                mClipDragMode        = replayLane && (locked || mode == 0) ? -1 : mode;
                mClipDragStartX      = mouse.x;
                mClipDragOriginStart = mClipDragStart = start;
                mClipDragOriginEnd   = mClipDragEnd   = end;
                mClipDragMoved       = false;
            };
            auto const shown = [&](std::string const& id, int start, int end) {
                return mClipDragId == id && mClipDragMoved ? std::pair{mClipDragStart, mClipDragEnd} : std::pair{start, end};
            };

            if (replayLane) {
                auto const* selectedSegment = ctx.selection.getAs<state::editing::model::SelectedWorldActorSegment>();
                // A cut is the empty stretch between two segments; it is hatched so it reads as removed.
                int lastEnd = 0;
                for (auto const& segment : project->worldActor.segments) {
                    auto const [start, end] = shown(segment.id, segment.startTick, segment.endTick);
                    if (start > lastEnd) {
                        float const gapLeft  = std::max(scale.xAt(static_cast<float>(lastEnd)), layout.canvasLeft);
                        float const gapRight = std::min(scale.xAt(static_cast<float>(start)), layout.fullMax.x);
                        for (float hatch = gapLeft - (barBottom - barTop); hatch < gapRight; hatch += 8.0f) {
                            float const x0 = std::max(hatch, gapLeft);
                            float const x1 = std::min(hatch + (barBottom - barTop), gapRight);
                            if (x1 <= x0) continue;
                            drawList->AddLine(
                                {x0, barBottom - (x0 - hatch)},
                                {x1, barBottom - (x1 - hatch)},
                                theme::withAlpha(theme::kReplayLayer, 0x40)
                            );
                        }
                    }
                    lastEnd = std::max(lastEnd, end);
                    float const x0 = scale.xAt(static_cast<float>(start));
                    float const x1 = scale.xAt(static_cast<float>(end));
                    drawLaneBar(
                        drawList,
                        {x0, barTop},
                        {x1, barBottom},
                        segment.locked ? theme::kTrackDisabled : theme::kReplayLayer,
                        selectedSegment && selectedSegment->segmentId == segment.id,
                        false,
                        segment.locked ? ICON_LOCK : ICON_FILM,
                        formatTick(start).c_str(),
                        layout.canvasLeft,
                        layout.fullMax.x
                    );
                    barHit(x0, x1, segment.id, segment.startTick, segment.endTick, segment.locked);
                }
            } else if (auto const* item = overlays::find(*project, row.overlayId)) {
                auto const* selectedLayer = ctx.selection.getAs<state::editing::model::SelectedOverlay>();
                auto const [start, end]   = shown(item->id, item->startTick, item->endTick);
                float const x0            = scale.xAt(static_cast<float>(start));
                float const x1            = scale.xAt(static_cast<float>(end));
                drawLaneBar(
                    drawList,
                    {x0, barTop},
                    {x1, barBottom},
                    layerColor(item->kind),
                    selectedLayer && selectedLayer->overlayId == item->id,
                    !item->visible,
                    item->locked ? ICON_LOCK : overlays::icon(*item),
                    item->kind == state::editing::model::OverlayKind::Title && !item->title.text.empty()
                        ? item->title.text.c_str()
                        : item->name.c_str(),
                    layout.canvasLeft,
                    layout.fullMax.x
                );
                barHit(x0, x1, item->id, item->startTick, item->endTick, item->locked);
            }
            // The rest of the lane only seeks; a box select there would select nothing.
            if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && contains({layout.canvasLeft, barTop}, {layout.fullMax.x, barBottom}, mouse)) {
                clickConsumed = true;
                submitSeek(ctx, std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks));
            }
        }
        if (row.kind == TrackRowKind::Property) {
            drawPropertyLane(ctx, row, layout, scale, y, rowBottom, allowInput, clickConsumed);
        }
        // Keyed properties of a row that is not expanded show as small marks along its foot; the World row shows
        // every World key on its centre line, and a click there seeks.
        if (row.kind == TrackRowKind::World || (row.expandable && !row.expanded)) {
            state::editing::model::ValueChannels const* channels = nullptr;
            int                                         offset   = 0;
            if (row.kind == TrackRowKind::World) {
                channels = &project->world.channels;
            } else if (row.kind == TrackRowKind::Camera && row.cameraIndex >= 0
                       && row.cameraIndex < static_cast<int>(project->cameras.size())) {
                channels = &project->cameras[static_cast<size_t>(row.cameraIndex)].channels;
            } else if (auto const* item = row.kind == TrackRowKind::Overlay ? overlays::find(*project, row.overlayId)
                                                                            : nullptr) {
                channels = &item->channels;
                offset   = item->startTick;
            }
            bool const  world  = row.kind == TrackRowKind::World;
            float const markY  = world ? centerY : rowBottom - keyRadius * 0.7f - 2.0f;
            float const radius = world ? keyRadius : keyRadius * 0.75f;
            if (channels) {
                for (int tick : keyTicks(*channels, offset)) {
                    float const x = scale.xAt(static_cast<float>(tick));
                    if (x < layout.canvasLeft - radius || x > layout.fullMax.x + radius) continue;
                    drawKeyframeDiamond(drawList, x, markY, radius, theme::kKeyframe, theme::kKeyframeOutline);
                }
            }
            ImVec2 const mouse = ImGui::GetMousePos();
            if (world && allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && contains({layout.canvasLeft, y}, {layout.fullMax.x, rowBottom}, mouse)) {
                clickConsumed = true;
                submitSeek(ctx, std::clamp(scale.tickAt(mouse.x), 0, state.totalTicks));
            }
        }
        y = rowBottom;
    }

    drawTrackGrid(ctx, layout, scale);
    drawExportRange(ctx, layout, scale, allowInput);
    drawContextMenus(ctx);
    drawLayerMenus(ctx);
    drawChannelKeyMenu(ctx);

    // Shift snaps a drag onto nearby edit points; otherwise the whole-second grid applies when enabled.
    bool const shiftSnap  = ImGui::GetIO().KeyShift;
    auto const dragTarget = [&](std::function<bool(std::string const&, int)> const& skipKey) {
        int const raw = tickFromMouse();
        return shiftSnap ? snapToEditPoints(ctx, raw, scale.pixelsPerTick, true, true, skipKey, {}) : snapTick(raw);
    };

    // A property key follows the mouse while held and lands on release; a click without movement seeks to it.
    if (allowInput && !mChannelDragRow.empty()) {
        auto const& rows = mTrackTree.rows();
        auto const  row  = std::ranges::find(rows, mChannelDragRow, &TrackTreeRow::id);
        if (row == rows.end()) {
            mChannelDragRow.clear();
        } else {
            if (std::abs(ImGui::GetMousePos().x - mChannelDragStartX) >= 2.0f) mChannelDragMoved = true;
            if (mChannelDragMoved) {
                int const target   = std::clamp(dragTarget([](std::string const&, int) { return false; }), 0, state.totalTicks);
                mChannelDragTick   = target - mChannelDragOffset;
                float const guideX = scale.xAt(static_cast<float>(target));
                drawList->AddLine(
                    {guideX, layout.bodyTop},
                    {guideX, layout.bodyBottom},
                    theme::withAlpha(theme::kKeyframeSelected, 180),
                    1.0f
                );
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (!mChannelDragMoved) {
                    submitSeek(ctx, mChannelDragKey + mChannelDragOffset);
                } else if (mChannelDragTick != mChannelDragKey) {
                    int const         from     = mChannelDragKey;
                    int const         to       = mChannelDragTick;
                    auto const members = rowMembers(*row);
                    editChannels(ctx, row->owner, row->ownerId, [&](state::editing::model::ValueChannels& channels) {
                        for (auto const& name : members) {
                            auto& channel = channels[name];
                            auto  moved   = channel.extract(from);
                            if (!moved) continue;
                            channel.insert_or_assign(to, moved.mapped());
                        }
                    });
                }
                mChannelDragRow.clear();
                mChannelDragMoved = false;
            }
        }
    }

    if (allowInput && mGroupDrag) {
        if (ImGui::GetMousePos().x != mDraggingKeyframeStartMouseX) mDraggingKeyframeMoved = true;
        auto const& keys = ctx.selection.keyframes();
        int         delta = 0;
        if (mDraggingKeyframeMoved && !keys.empty()) {
            int const target =
                dragTarget([&](std::string const& id, int tick) { return ctx.selection.isKeyframeSelected(id, tick); });
            delta = target - mDraggingKeyframeStartTick;
            // Clamp the whole set into the replay, then refuse deltas that would land on a key outside the set.
            int minTick = std::numeric_limits<int>::max();
            int maxTick = std::numeric_limits<int>::min();
            for (auto const& key : keys) {
                minTick = std::min(minTick, key.tick);
                maxTick = std::max(maxTick, key.tick);
            }
            delta = std::clamp(delta, -minTick, state.totalTicks - maxTick);
            bool const collides = std::ranges::any_of(keys, [&](auto const& key) {
                auto const camera =
                    std::ranges::find(project->cameras, key.trackId, &state::editing::model::CameraEntity::id);
                if (camera == project->cameras.end()) return false;
                int const moved = key.tick + delta;
                return camera->keysByTick.contains(moved) && !ctx.selection.isKeyframeSelected(key.trackId, moved);
            });
            if (collides) delta = mGroupDragDelta;
        }
        mGroupDragDelta     = delta;
        float const markerX = scale.xAt(static_cast<float>(mDraggingKeyframeStartTick + delta));
        drawList->AddLine(
            {markerX, layout.bodyTop},
            {markerX, layout.bodyBottom},
            theme::withAlpha(theme::kKeyframeSelected, 180),
            1.0f
        );
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (mDraggingKeyframeMoved && delta != 0) {
                EditorAction action{EditorActionType::MoveCameraKeyframes};
                action.keyframeRefs = selectedKeyframeRefs(ctx);
                action.tick         = delta;
                ctx.submitAction(std::move(action));
                std::vector<state::editing::model::SelectedKeyframe> moved;
                for (auto const& key : keys) moved.push_back({key.trackId, key.tick + delta});
                queueKeyframeSelection(std::move(moved));
            } else if (!mDraggingKeyframeMoved) {
                ctx.selection.select(
                    state::editing::model::SelectedKeyframe{mDraggingKeyframeCameraId, mDraggingKeyframeStartTick}
                );
                submitSeek(ctx, mDraggingKeyframeStartTick);
            }
            mGroupDrag      = false;
            mGroupDragDelta = 0;
            mDraggingKeyframeCameraId.clear();
            mDraggingKeyframeStartTick = -1;
            mDraggingKeyframeMoved     = false;
        }
    } else if (allowInput && !mDraggingKeyframeCameraId.empty()) {
        // Any horizontal movement latches into a drag, so a plain click only seeks.
        if (ImGui::GetMousePos().x != mDraggingKeyframeStartMouseX) mDraggingKeyframeMoved = true;
        int const candidateTick =
            mDraggingKeyframeMoved
                ? boundedKeyframeTick(dragTarget([&](std::string const& id, int tick) {
                      return id == mDraggingKeyframeCameraId && tick == mDraggingKeyframeStartTick;
                  }))
                : mDraggingKeyframeStartTick;
        mDraggingKeyframeTick = candidateTick;
        float const markerX   = scale.xAt(static_cast<float>(candidateTick));
        drawList->AddLine(
            {markerX, layout.bodyTop},
            {markerX, layout.bodyBottom},
            theme::withAlpha(theme::kKeyframeSelected, 180),
            1.0f
        );
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (mDraggingKeyframeMoved) {
                if (candidateTick != mDraggingKeyframeStartTick) {
                    EditorAction action{EditorActionType::MoveCameraKeyframe};
                    action.id            = mDraggingKeyframeCameraId;
                    action.tick          = mDraggingKeyframeStartTick;
                    action.secondaryTick = candidateTick;
                    ctx.submitAction(std::move(action));
                    queueKeyframeSelection({{mDraggingKeyframeCameraId, candidateTick}});
                }
            } else {
                submitSeek(ctx, mDraggingKeyframeStartTick);
            }
            mDraggingKeyframeCameraId.clear();
            mDraggingKeyframeStartTick = -1;
            mDraggingKeyframeMoved     = false;
        }
    }

    if (allowInput && !mClipDragId.empty()) {
        if (std::abs(ImGui::GetMousePos().x - mClipDragStartX) >= 2.0f) mClipDragMoved = true;
        // A replay segment may only grow into the gaps beside it; a layer anywhere in the replay.
        int minTick = 0;
        int maxTick = state.totalTicks;
        if (mClipDragReplay) {
            auto const& segments = project->worldActor.segments;
            auto const  it       = std::ranges::find(segments, mClipDragId, &state::editing::model::WorldActorSegment::id);
            if (it == segments.end()) {
                mClipDragId.clear();
            } else {
                if (it != segments.begin()) minTick = std::prev(it)->endTick;
                if (std::next(it) != segments.end()) maxTick = std::next(it)->startTick;
            }
        } else if (!overlays::find(*project, mClipDragId)) {
            mClipDragId.clear();
        }
        if (!mClipDragId.empty() && mClipDragMoved && mClipDragMode >= 0) {
            int const target = dragTarget([](std::string const&, int) { return false; });
            int const length = mClipDragOriginEnd - mClipDragOriginStart;
            if (mClipDragMode == 0) {
                int const delta = target - std::clamp(scale.tickAt(mClipDragStartX), 0, state.totalTicks);
                mClipDragStart  = std::clamp(mClipDragOriginStart + delta, minTick, std::max(minTick, maxTick - length));
                mClipDragEnd    = mClipDragStart + length;
            } else if (mClipDragMode == 1) {
                mClipDragStart = std::clamp(target, minTick, mClipDragOriginEnd - 1);
            } else {
                mClipDragEnd = std::clamp(target, mClipDragOriginStart + 1, maxTick);
            }
            float const guideX = scale.xAt(static_cast<float>(mClipDragMode == 2 ? mClipDragEnd : mClipDragStart));
            drawList->AddLine(
                {guideX, layout.bodyTop},
                {guideX, layout.bodyBottom},
                theme::withAlpha(theme::kKeyframeSelected, 180),
                1.0f
            );
            ImGui::SetMouseCursor(mClipDragMode == 0 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_ResizeEW);
        }
        if (!mClipDragId.empty() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            bool const changed = mClipDragStart != mClipDragOriginStart || mClipDragEnd != mClipDragOriginEnd;
            if (!mClipDragMoved || mClipDragMode < 0) {
                submitSeek(ctx, std::clamp(scale.tickAt(mClipDragStartX), 0, state.totalTicks));
            } else if (changed && mClipDragReplay) {
                EditorAction action{EditorActionType::TrimWorldActor};
                action.id   = mClipDragId;
                action.tick = mClipDragStart;
                action.kind = mClipDragEnd;
                ctx.submitAction(std::move(action));
            } else if (changed) {
                auto item = *overlays::find(*project, mClipDragId);
                // A move carries the keys along; a start trim leaves them where they are on the timeline.
                if (mClipDragMode == 1) state::editing::model::setOverlayStart(item, mClipDragStart);
                else item.startTick = mClipDragStart;
                item.endTick = mClipDragEnd;
                overlays::commitItem(ctx, item);
            }
            mClipDragId.clear();
            mClipDragMoved = false;
        }
    }

    if (allowInput && !mPoseDragId.empty()) {
        auto const key = std::ranges::find(project->poseKeys, mPoseDragId, &state::editing::model::PoseKey::id);
        if (key == project->poseKeys.end()) {
            mPoseDragId.clear();
        } else {
            if (std::abs(ImGui::GetMousePos().x - mPoseDragStartX) >= 2.0f) mPoseDragMoved = true;
            if (mPoseDragMoved) {
                mPoseDragTick      = dragTarget([](std::string const&, int) { return false; });
                float const guideX = scale.xAt(static_cast<float>(mPoseDragTick));
                drawList->AddLine(
                    {guideX, layout.bodyTop},
                    {guideX, layout.bodyBottom},
                    theme::withAlpha(theme::kKeyframeSelected, 180),
                    1.0f
                );
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (!mPoseDragMoved) {
                    submitSeek(ctx, key->tick);
                } else if (mPoseDragTick != key->tick) {
                    EditorAction action{EditorActionType::SetPoseKeys};
                    action.poseKeys = project->poseKeys;
                    for (auto& moved : *action.poseKeys) {
                        if (moved.id == mPoseDragId) moved.tick = mPoseDragTick;
                    }
                    ctx.submitAction(std::move(action));
                }
                mPoseDragId.clear();
                mPoseDragMoved = false;
            }
        }
    }

    float const playheadX = std::clamp(scale.xAt(static_cast<float>(displayTick)), layout.canvasLeft, layout.fullMax.x);
    if (allowInput && !clickConsumed && mMarkerDragId.empty() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && std::abs(ImGui::GetMousePos().x - playheadX) <= 6.0f && ImGui::GetMousePos().y >= layout.rulerTop
        && ImGui::GetMousePos().y < layout.bodyBottom) {
        mDraggingPlayhead = true;
        clickConsumed     = true;
    }
    if (allowInput && mDraggingPlayhead) {
        displayTick = tickFromMouse();
        if (shiftSnap) displayTick = snapToEditPoints(ctx, displayTick, scale.pixelsPerTick, false, true, {}, {});
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            submitSeek(ctx, displayTick);
            mDraggingPlayhead = false;
        }
    }
    if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && ImGui::GetMousePos().x >= layout.canvasLeft && ImGui::GetMousePos().x <= layout.fullMax.x
        && ImGui::GetMousePos().y >= layout.bodyTop && ImGui::GetMousePos().y < layout.bodyBottom) {
        mBoxPending   = true;
        mBoxSelecting = false;
        mBoxAdditive  = ImGui::GetIO().KeyCtrl;
        mBoxStart     = ImGui::GetMousePos();
        mBoxClickCameraId.clear();
    }
    if (allowInput && mBoxPending) {
        if (mBoxSelecting) {
            drawList->AddRectFilled(boxMin, boxMax, theme::withAlpha(theme::kAccent, 0x30));
            drawList->AddRect(boxMin, boxMax, theme::withAlpha(theme::kAccent, 0xc0));
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (mBoxSelecting) {
                if (mBoxAdditive) {
                    std::vector<state::editing::model::SelectedKeyframe> merged = ctx.selection.keyframes();
                    for (auto const& hit : boxHits) {
                        if (!ctx.selection.isKeyframeSelected(hit.trackId, hit.tick)) merged.push_back(hit);
                    }
                    ctx.selection.setKeyframes(std::move(merged));
                } else if (!boxHits.empty()) {
                    ctx.selection.setKeyframes(std::move(boxHits));
                } else {
                    ctx.selection.clear();
                }
            } else if (mBoxAdditive) {
                // A Ctrl+click that never became a box leaves the selection alone.
            } else if (!mBoxClickCameraId.empty()) {
                ctx.selection.select(state::editing::model::SelectedCamera{mBoxClickCameraId});
                EditorAction action{EditorActionType::SetPreviewCamera};
                action.id = mBoxClickCameraId;
                ctx.submitAction(std::move(action));
            } else {
                submitSeek(ctx, std::clamp(scale.tickAt(mBoxStart.x), 0, state.totalTicks));
            }
            mBoxPending   = false;
            mBoxSelecting = false;
            mBoxClickCameraId.clear();
        }
    }

    drawMarkers(ctx, layout, scale);
    if (mSnapGuideTick >= 0) {
        float const guideX = scale.xAt(static_cast<float>(mSnapGuideTick));
        drawList->AddLine({guideX, layout.rulerTop}, {guideX, layout.bodyBottom}, theme::kIconHighlight, 1.5f);
    }

    float const visiblePlayheadX =
        std::clamp(scale.xAt(static_cast<float>(displayTick)), layout.canvasLeft, layout.fullMax.x);
    drawList
        ->AddLine({visiblePlayheadX, layout.rulerTop}, {visiblePlayheadX, layout.bodyBottom}, theme::kPlayhead, 1.0f);
    {
        std::string const badgeText = formatTick(displayTick);
        ImVec2 const      textSize  = ImGui::CalcTextSize(badgeText.c_str());
        float const       badgeW    = textSize.x + 10.0f;
        float const       badgeH    = metrics::ruler() - 5.0f;
        float const badgeX = std::clamp(visiblePlayheadX - badgeW * 0.5f, layout.canvasLeft, layout.fullMax.x - badgeW);
        float const badgeY = layout.rulerTop + 2.0f;
        drawList->AddRectFilled({badgeX, badgeY}, {badgeX + badgeW, badgeY + badgeH}, theme::kPlayhead, 2.0f);
        drawList
            ->AddText({badgeX + 5.0f, badgeY + (badgeH - textSize.y) * 0.5f}, theme::kPlayheadLabel, badgeText.c_str());
    }
    drawList->PopClipRect();

    if (maxScrollY > 0.0f) {
        float const trackHeight = std::max(1.0f, visibleTrackHeight);
        float const thumbHeight = std::max(20.0f, trackHeight * trackHeight / std::max(1.0f, trackContentHeight));
        float const thumbTop    = layout.bodyTop + (trackHeight - thumbHeight) * (mScrollY / maxScrollY);
        drawList->AddRectFilled(
            {layout.fullMax.x - 5.0f, thumbTop},
            {layout.fullMax.x - 2.0f, thumbTop + thumbHeight},
            theme::withAlpha(theme::kScrollThumb, 200),
            2.0f
        );
    }

    ImVec2 const wheelMouse = ImGui::GetMousePos();
    bool const   canvasHovered =
        contains({layout.canvasLeft, layout.rulerTop}, {layout.fullMax.x, layout.bodyBottom}, wheelMouse);
    bool const trackListHovered = wheelMouse.x >= fullMin.x && wheelMouse.x < layout.canvasLeft
                               && wheelMouse.y >= layout.bodyTop && wheelMouse.y < layout.bodyBottom;
    if (allowInput && (canvasHovered || trackListHovered) && ImGui::GetIO().MouseWheel != 0.0f) {
        float const wheel = ImGui::GetIO().MouseWheel;
        if (ImGui::GetIO().KeyAlt) {
            float const anchorX    = std::clamp(wheelMouse.x - layout.canvasLeft, 0.0f, layout.canvasWidth);
            float const anchorTick = scale.tickAtExact(layout.canvasLeft + anchorX);
            mZoomScale =
                std::clamp(mZoomScale * (wheel > 0.0f ? kZoomStep : 1.0f / kZoomStep), kMinZoomScale, kMaxZoomScale);
            float const nextPixelsPerTick = fitPixelsPerTick * mZoomScale;
            float const nextMaxScroll     = std::max(0.0f, state.totalTicks * nextPixelsPerTick - layout.canvasWidth);
            mScrollX                      = std::clamp(anchorTick * nextPixelsPerTick - anchorX, 0.0f, nextMaxScroll);
        } else if (canvasHovered && maxScroll > 0.0f) {
            mScrollX = std::clamp(mScrollX - wheel * 60.0f, 0.0f, maxScroll);
        } else {
            mScrollY = std::clamp(mScrollY - wheel * 45.0f, 0.0f, maxScrollY);
        }
    }

    drawRangeBar(ctx, layout, allowInput);
}

void TimelinePanel::editChannels(
    PanelContext const&                                                ctx,
    PropertyOwner                                                      owner,
    std::string const&                                                 ownerId,
    std::function<void(state::editing::model::ValueChannels&)> const& edit
) {
    auto const* project = ctx.project();
    if (!project) return;
    switch (owner) {
    case PropertyOwner::Camera: {
        auto const camera = std::ranges::find(project->cameras, ownerId, &state::editing::model::CameraEntity::id);
        if (camera == project->cameras.end() || camera->locked) return;
        auto properties = state::editing::model::cameraProperties(*camera);
        edit(properties.channels);
        state::editing::model::pruneChannels(properties.channels);
        EditorAction action{EditorActionType::SetCameraProperties};
        action.id               = ownerId;
        action.cameraProperties = std::move(properties);
        ctx.submitAction(std::move(action));
        break;
    }
    case PropertyOwner::Overlay: {
        auto const* stored = overlays::find(*project, ownerId);
        if (!stored || stored->locked) return;
        auto item = *stored;
        edit(item.channels);
        state::editing::model::pruneChannels(item.channels);
        overlays::commitItem(ctx, item);
        break;
    }
    case PropertyOwner::World: {
        auto world = project->world;
        edit(world.channels);
        state::editing::model::pruneChannels(world.channels);
        EditorAction action{EditorActionType::SetWorldSettings};
        action.worldSettings = std::move(world);
        ctx.submitAction(std::move(action));
        break;
    }
    }
}

void TimelinePanel::drawPropertyLane(
    PanelContext const&  ctx,
    TrackTreeRow const&  row,
    Layout const&        layout,
    TimelineScale const& scale,
    float                top,
    float                bottom,
    bool                 allowInput,
    bool&                clickConsumed
) {
    using state::editing::model::KeyEasing;
    using state::editing::model::ValueChannel;
    auto const* project = ctx.project();
    if (!project) return;
    auto const keyed = rowChannel(*project, row);
    if (!keyed.channel) return;
    auto* const drawList  = ImGui::GetWindowDrawList();
    float const keyRadius = metrics::keyframeRadius();
    ImU32 const color     = row.owner == PropertyOwner::World   ? theme::kKeyframe
                          : row.owner == PropertyOwner::Overlay ? layerColor(row.overlayKind)
                                                                : theme::trackColor(row.cameraIndex);

    // A key being dragged is drawn, and the curve follows it, where it would land.
    ValueChannel channel = *keyed.channel;
    bool const   dragged = mChannelDragRow == row.id && mChannelDragMoved;
    if (dragged) {
        if (auto moved = channel.extract(mChannelDragKey)) channel.insert_or_assign(mChannelDragTick, moved.mapped());
    }

    // The curve spans the keys' range of values with a little air above and below.
    auto const [lowest, highest] = std::ranges::minmax(channel | std::views::values | std::views::transform(&state::editing::model::ValueKey::value));
    float low  = lowest;
    float high = highest;
    if (high - low < 1e-4f) {
        low  -= 1.0f;
        high += 1.0f;
    }
    float const laneTop    = top + 5.0f;
    float const laneBottom = bottom - 5.0f;
    auto const  yOf        = [&](float value) {
        return laneBottom - (value - low) / (high - low) * (laneBottom - laneTop);
    };

    // A layer's keys only act while it is on screen; outside that stretch the lane is dimmed.
    float const spanLeft  = std::max(layout.canvasLeft, scale.xAt(0.0f));
    float const spanRight = std::min(layout.fullMax.x, scale.xAt(static_cast<float>(project->totalTicks)));
    if (row.owner == PropertyOwner::Overlay) {
        if (auto const* item = overlays::find(*project, row.ownerId)) {
            float const x0 = std::max(spanLeft, scale.xAt(static_cast<float>(item->startTick)));
            float const x1 = std::min(spanRight, scale.xAt(static_cast<float>(item->endTick)));
            if (x1 > x0) drawList->AddRectFilled({x0, top + 2.0f}, {x1, bottom - 2.0f}, theme::withAlpha(color, 0x18));
        }
    }
    drawList->AddLine({spanLeft, laneBottom}, {spanRight, laneBottom}, theme::withAlpha(theme::kLine, 0x80));

    std::vector<ImVec2> points;
    float const         step = 3.0f;
    for (float x = spanLeft; x <= spanRight + step * 0.5f; x += step) {
        float const  px    = std::min(x, spanRight);
        double const tick  = scale.tickAtExact(px) - keyed.offset;
        float const  value = state::editing::model::sampleChannel(channel, tick).value_or(low);
        points.push_back({px, yOf(value)});
    }
    if (points.size() >= 2) {
        drawList->AddPolyline(points.data(), static_cast<int>(points.size()), theme::withAlpha(color, 0xd0), 0, 1.5f);
    }

    // Keys: diamonds on the curve (squares hold), each draggable along the timeline.
    ImVec2 const mouse   = ImGui::GetMousePos();
    bool const   editable = !row.locked;
    for (auto const& [tick, key] : channel) {
        float const x = scale.xAt(static_cast<float>(tick + keyed.offset));
        if (x < layout.canvasLeft - keyRadius || x > layout.fullMax.x + keyRadius) continue;
        float const keyY     = std::clamp(yOf(key.value), top + keyRadius, bottom - keyRadius);
        bool const  active   = (dragged && tick == mChannelDragTick) || (mChannelMenuRow == row.id && mChannelMenuKey == tick);
        bool const  over     = std::abs(mouse.x - x) <= keyRadius + 3.0f && std::abs(mouse.y - keyY) <= keyRadius + 4.0f;
        ImU32 const fill     = active || over ? theme::kKeyframeSelected : theme::kKeyframe;
        ImU32 const outline  = active ? theme::kIconHighlight : theme::kKeyframeOutline;
        if (key.easing == KeyEasing::Hold) drawKeyframeSquare(drawList, x, keyY, keyRadius, fill, outline);
        else drawKeyframeDiamond(drawList, x, keyY, keyRadius, fill, outline);
        if (over && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            widgets::hoverTooltip(
                (formatTick(tick + keyed.offset) + "  " + std::format("{:.3g}", key.value) + "  ·  "
                 + keyframes::easingName(key.easing))
                    .c_str()
            );
        }
        if (!allowInput || clickConsumed || !over) continue;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            clickConsumed   = true;
            mChannelMenuRow = row.id;
            mChannelMenuKey = tick;
            widgets::openAnchoredPopup(
                "##timeline-channel-key-menu",
                {x - keyRadius, keyY - keyRadius},
                {x + keyRadius, keyY + keyRadius}
            );
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            clickConsumed = true;
            if (!editable) {
                submitSeek(ctx, tick + keyed.offset);
                continue;
            }
            mChannelDragRow    = row.id;
            mChannelDragKey    = tick;
            mChannelDragTick   = tick;
            mChannelDragOffset = keyed.offset;
            mChannelDragStartX = mouse.x;
            mChannelDragMoved  = false;
        }
    }
    // Elsewhere on the lane a click seeks, as on any other lane.
    if (allowInput && !clickConsumed && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && contains({layout.canvasLeft, top}, {layout.fullMax.x, bottom}, mouse)) {
        clickConsumed = true;
        submitSeek(ctx, std::clamp(scale.tickAt(mouse.x), 0, project->totalTicks));
    }
}

void TimelinePanel::drawChannelKeyMenu(PanelContext const& ctx) {
    if (!widgets::beginAnchoredPopup("##timeline-channel-key-menu")) return;
    auto const& rows = mTrackTree.rows();
    auto const  row  = std::ranges::find(rows, mChannelMenuRow, &TrackTreeRow::id);
    auto const* project = ctx.project();
    auto const  keyed   = row != rows.end() && project ? rowChannel(*project, *row) : RowChannel{};
    if (!keyed.channel || !keyed.channel->contains(mChannelMenuKey)) {
        ImGui::CloseCurrentPopup();
        widgets::endAnchoredPopup();
        return;
    }
    int const         tick     = mChannelMenuKey;
    auto const        current  = keyed.channel->at(tick).easing;
    auto const        members  = rowMembers(*row);
    ImGui::TextDisabled("%s  %s", row->name.c_str(), formatTick(tick + keyed.offset).c_str());
    ImGui::Separator();
    ImGui::BeginDisabled(row->locked);
    // How the value travels from this key to the next.
    for (int index = 0; index <= static_cast<int>(state::editing::model::KeyEasing::Hold); ++index) {
        auto const easing = static_cast<state::editing::model::KeyEasing>(index);
        if (ImGui::MenuItem(keyframes::easingName(easing).c_str(), nullptr, current == easing)) {
            editChannels(ctx, row->owner, row->ownerId, [&](state::editing::model::ValueChannels& channels) {
                for (auto const& name : members) {
                    if (auto channel = channels.find(name); channel != channels.end()) {
                        if (auto key = channel->second.find(tick); key != channel->second.end()) key->second.easing = easing;
                    }
                }
            });
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("playback.refactorEditor.keyframes.seekToKey"_tr().c_str())) submitSeek(ctx, tick + keyed.offset);
    if (ImGui::MenuItem("playback.refactorEditor.keyframes.deleteKey"_tr().c_str(), "Del")) {
        editChannels(ctx, row->owner, row->ownerId, [&](state::editing::model::ValueChannels& channels) {
            for (auto const& name : members) channels[name].erase(tick);
        });
        mChannelMenuRow.clear();
    }
    ImGui::EndDisabled();
    widgets::endAnchoredPopup();
}

} // namespace playback::editor::ui
