#include "DetailsPanel.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/EasingCurveEditor.h"
#include "playback/editor/ui/components/EasingPresetPicker.h"
#include "playback/editor/ui/components/PlayerPicker.h"
#include "playback/editor/ui/components/PropertyControls.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/exporting/ExportPlanCompiler.h"
#include "playback/integration/OptiPistonBridge.h"
#include "playback/keyframe/ClientCameraCapture.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

namespace playback::editor::ui {
using namespace playback::state;

using namespace ll::i18n_literals;

namespace {

template <typename T>
T const* findById(std::vector<T> const& values, std::string const& id) {
    auto const it = std::find_if(values.begin(), values.end(), [&id](T const& value) { return value.id == id; });
    return it == values.end() ? nullptr : &*it;
}

using widgets::formatTick;

std::string interpolationName(state::editing::model::CameraInterpolationType interpolation) {
    using Interpolation = state::editing::model::CameraInterpolationType;
    switch (interpolation) {
    case Interpolation::Linear:
        return "playback.refactorEditor.details.interpolationType.linear"_tr();
    case Interpolation::EaseIn:
        return "playback.refactorEditor.details.interpolationType.easeIn"_tr();
    case Interpolation::EaseOut:
        return "playback.refactorEditor.details.interpolationType.easeOut"_tr();
    case Interpolation::EaseInOut:
        return "playback.refactorEditor.details.interpolationType.easeInOut"_tr();
    case Interpolation::Hold:
        return "playback.refactorEditor.details.interpolationType.hold"_tr();
    case Interpolation::Hermite:
        return "playback.refactorEditor.details.interpolationType.hermite"_tr();
    case Interpolation::CubicBezier:
        return "playback.refactorEditor.details.interpolationType.cubicBezier"_tr();
    case Interpolation::Smooth:
    default:
        return "playback.refactorEditor.details.interpolationType.smooth"_tr();
    }
}

// The key's easing preset: only a CubicBezier key whose control points match one has it.
std::optional<std::size_t> keyEasingPreset(state::editing::model::CameraKeyframe const& key) {
    if (key.interpolationType != state::editing::model::CameraInterpolationType::CubicBezier) return std::nullopt;
    return state::editing::model::findEasingPreset(key.bezierCtrl1, key.bezierCtrl2);
}

std::string easingPresetPreview(state::editing::model::CameraKeyframe const& key) {
    if (key.interpolationType != state::editing::model::CameraInterpolationType::CubicBezier) {
        return "playback.refactorEditor.details.easingPresetNone"_tr();
    }
    if (auto const preset = keyEasingPreset(key))
        return easingPresetName(state::editing::model::EasingPresets[*preset]);
    return "playback.refactorEditor.details.easingPresetCustom"_tr();
}

// The keys' easing as picked in the combo: a preset is a Cubic Bezier curve; None goes back to Smooth, the default.
state::EditorAction easingAction(std::vector<state::editing::model::KeyframeRef> refs, EasingPick const& pick) {
    using state::editing::model::CameraInterpolationType;
    state::EditorAction action{state::EditorActionType::SetKeyframesInterpolation};
    action.keyframeRefs = std::move(refs);
    if (pick.preset) {
        action.kind           = static_cast<int>(CameraInterpolationType::CubicBezier);
        action.bezierControls = std::array{pick.preset->control1, pick.preset->control2};
    } else {
        action.kind = static_cast<int>(CameraInterpolationType::Smooth);
    }
    return action;
}

std::string categoryName(state::editing::model::SubActorCategory category) {
    using Category = state::editing::model::SubActorCategory;
    switch (category) {
    case Category::Players:
        return "playback.refactorEditor.details.categoryName.players"_tr();
    case Category::Creatures:
        return "playback.refactorEditor.details.categoryName.creatures"_tr();
    case Category::Entities:
        return "playback.refactorEditor.details.categoryName.entities"_tr();
    case Category::Default:
    default:
        return "playback.refactorEditor.details.categoryName.default"_tr();
    }
}

bool vectorInput(
    char const* id,
    float (&values)[3],
    std::array<char const*, 3> const& labels,
    std::array<ImVec4, 3> const*      colors,
    char const*                       format
) {
    bool edited = false;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {3.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4.0f, 3.0f});
    if (ImGui::BeginTable("##axes", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
        for (int axis = 0; axis < 3; ++axis) {
            ImGui::TableNextColumn();
            if (colors) ImGui::TextColored((*colors)[axis], "%s", labels[axis]);
            else ImGui::TextDisabled("%s", labels[axis]);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushID(axis);
            ImGui::InputFloat("##value", &values[axis], 0.0f, 0.0f, format);
            edited |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return edited;
}

} // namespace

void DetailsPanel::draw(PanelContext const& ctx) {
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    ImVec2 const avail  = ImGui::GetContentRegionAvail();
    float const  railW  = metrics::rail();
    drawRail(ctx, railW, avail.y);

    ImGui::SetCursorScreenPos({origin.x + railW, origin.y});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {theme::kPanelPadding, theme::kPanelPadding});
    ImGui::BeginChild("##InspectorBody", {std::max(1.0f, avail.x - railW), avail.y}, false);
    if (!ctx.state.project) {
        widgets::noActiveProjectPlaceholder();
    } else {
        switch (mPage) {
        case InspectorPage::Selection:
            drawSelectionPage(ctx);
            break;
        case InspectorPage::Export:
            drawExportPage(ctx);
            break;
        case InspectorPage::Preview:
            drawPreviewPage(ctx);
            break;
        case InspectorPage::Settings:
            drawSettingsPage(ctx);
            break;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void DetailsPanel::drawRail(PanelContext const& ctx, float width, float height) {
    ImVec2 const origin   = ImGui::GetCursorScreenPos();
    auto*        drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, theme::kRailBg);
    drawList->AddLine({origin.x + width, origin.y}, {origin.x + width, origin.y + height}, theme::kBorder);

    struct Entry {
        InspectorPage page;
        char const*   icon;
        std::string   tooltip;
        bool          enabled;
    };
    bool const       exportActive = exporting::isExportActive(ctx.state.exportStatus.state);
    std::array const entries{
        Entry{InspectorPage::Selection, ICON_PANEL_LEFT, "playback.refactorEditor.inspector.selection"_tr(), true},
        Entry{InspectorPage::Export,    ICON_EXPORT,     "playback.refactorEditor.inspector.export"_tr(),    true},
        Entry{InspectorPage::Preview,   ICON_EYE,        "playback.refactorEditor.inspector.preview"_tr(),   true},
        Entry{InspectorPage::Settings,  ICON_SETTINGS,   "playback.refactorEditor.inspector.settings"_tr(),  true},
    };

    float const button = metrics::iconButton();
    float       y      = origin.y + 4.0f;
    for (auto const& entry : entries) {
        bool const active = mPage == entry.page;
        ImGui::SetCursorScreenPos({origin.x + (width - button) * 0.5f, y});
        ImGui::PushID(static_cast<int>(entry.page));
        if (widgets::iconToggle("rail", entry.icon, entry.tooltip.c_str(), active, entry.enabled)) mPage = entry.page;
        ImGui::PopID();
        if (active) {
            drawList->AddRectFilled({origin.x, y + 3.0f}, {origin.x + 2.0f, y + button - 3.0f}, theme::kRailActive);
        }
        y += button + 2.0f;
    }
    (void)exportActive;
}

void DetailsPanel::drawSelectionPage(PanelContext const& ctx) {
    auto const  project   = ctx.state.project;
    auto const& selection = ctx.selection;
    std::string subject   = "playback.refactorEditor.details.noSelection"_tr();
    if (selection.getAs<state::editing::model::SelectedWorldActor>()) {
        subject = "playback.refactorEditor.details.worldActor"_tr();
    } else if (selection.getAs<state::editing::model::SelectedWorldActorSegment>()) {
        subject = "playback.refactorEditor.details.worldActorSegment"_tr();
    } else if (selection.getAs<state::editing::model::SelectedSubActor>()) {
        subject = "playback.refactorEditor.details.subActor"_tr();
    } else if (auto const* selectedCamera = selection.getAs<state::editing::model::SelectedCamera>()) {
        auto const* camera = findById(project->cameras, selectedCamera->cameraId);
        subject            = camera ? camera->name : "playback.refactorEditor.details.camera"_tr();
    } else if (selection.keyframeCount() > 1) {
        subject = "playback.refactorEditor.details.keyframesSelected"_tr(selection.keyframeCount());
    } else if (selection.getAs<state::editing::model::SelectedKeyframe>()) {
        subject = "playback.refactorEditor.details.cameraKeyframe"_tr();
    } else if (selection.getAs<state::editing::model::SelectedMarker>()) {
        subject = "playback.refactorEditor.details.marker"_tr();
    } else if (selection.getAs<state::editing::model::SelectedPoseKey>()) {
        subject = "playback.refactorEditor.details.poseKey"_tr();
    }
    property::beginInspector("playback.refactorEditor.inspector.selection"_tr(), subject);

    if (drawWorldActor(ctx)) return;
    if (drawWorldActorSegment(ctx)) return;
    if (drawSubActor(ctx)) return;
    if (drawCamera(ctx)) return;
    if (drawKeyframes(ctx)) return;
    if (drawKeyframe(ctx)) return;
    if (drawMarker(ctx)) return;
    if (drawPoseKey(ctx)) return;
    drawOverview(ctx);
}

bool DetailsPanel::drawWorldActor(PanelContext const& ctx) {
    auto const  project   = ctx.state.project;
    auto const& selection = ctx.selection;

    if (selection.getAs<state::editing::model::SelectedWorldActor>()) {
        if (property::beginSection("playback.refactorEditor.details.worldActor"_tr().c_str())) {
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.name"_tr(
                    project->worldActor.name.empty() ? "playback.refactorEditor.details.untitled"_tr()
                                                     : project->worldActor.name
                )
                    .c_str()
            );
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.total"_tr(formatTick(project->worldActor.totalTicks)).c_str()
            );
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.segments"_tr(project->worldActor.segments.size()).c_str()
            );
            for (auto const& segment : project->worldActor.segments) {
                if (ImGui::Selectable((formatTick(segment.startTick) + " - " + formatTick(segment.endTick)).c_str())) {
                    ctx.selection.select(state::editing::model::SelectedWorldActorSegment{segment.id});
                }
            }
            property::separator();
            if (property::beginSection("playback.refactorEditor.details.subActors"_tr().c_str())) {
                static constexpr std::array<state::editing::model::SubActorCategory, 4> categories{
                    state::editing::model::SubActorCategory::Default,
                    state::editing::model::SubActorCategory::Players,
                    state::editing::model::SubActorCategory::Creatures,
                    state::editing::model::SubActorCategory::Entities,
                };
                for (auto category : categories) {
                    auto const&  actors = project->worldActor.subActors;
                    size_t const count =
                        static_cast<size_t>(std::count_if(actors.begin(), actors.end(), [category](auto const& actor) {
                            return actor.category == category;
                        }));
                    std::string const header = categoryName(category) + " (" + std::to_string(count) + ")";
                    if (count > 0 && ImGui::CollapsingHeader(header.c_str())) {
                        size_t shown = 0;
                        for (auto const& actor : actors) {
                            if (actor.category != category) continue;
                            if (shown >= 100) {
                                ImGui::TextDisabled(
                                    "%s",
                                    "playback.refactorEditor.details.moreActors"_tr(count - shown).c_str()
                                );
                                break;
                            }
                            ++shown;
                            if (ImGui::Selectable(actor.name.empty() ? actor.id.c_str() : actor.name.c_str())) {
                                ctx.selection.select(state::editing::model::SelectedSubActor{actor.id});
                            }
                        }
                    }
                }
                property::endSection();
            }
            property::endSection();
        }
        return true;
    }
    return false;
}

bool DetailsPanel::drawWorldActorSegment(PanelContext const& ctx) {
    auto const  project   = ctx.state.project;
    auto const& selection = ctx.selection;
    auto const  submit    = [&ctx](EditorAction action) { ctx.submitAction(std::move(action)); };

    if (auto const* selected = selection.getAs<state::editing::model::SelectedWorldActorSegment>()) {
        auto const* segment = findById(project->worldActor.segments, selected->segmentId);
        if (!segment) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.worldActorSegmentMissing"_tr().c_str());
            return true;
        }
        bool const isFirst = &project->worldActor.segments.front() == segment;
        bool const isLast  = &project->worldActor.segments.back() == segment;
        if (property::beginSection("playback.refactorEditor.details.worldActorSegment"_tr().c_str())) {
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.range"_tr(formatTick(segment->startTick), formatTick(segment->endTick))
                    .c_str()
            );
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.duration"_tr(segment->endTick - segment->startTick).c_str()
            );
            ImGui::TextUnformatted("playback.refactorEditor.details.sourceTick"_tr(segment->sourceTick).c_str());
            ImGui::BeginDisabled(segment->locked);
            int        startTick  = segment->startTick;
            int        endTick    = segment->endTick;
            auto const startLabel = "playback.refactorEditor.details.start"_tr();
            auto const endLabel   = "playback.refactorEditor.details.end"_tr();
            ImGui::BeginDisabled(isFirst);
            if (ImGui::InputInt(startLabel.c_str(), &startTick) && ImGui::IsItemDeactivatedAfterEdit()) {
                EditorAction action{EditorActionType::TrimWorldActor};
                action.id   = segment->id;
                action.tick = std::clamp(startTick, 1, endTick - 1);
                action.kind = endTick;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(isLast);
            if (ImGui::InputInt(endLabel.c_str(), &endTick) && ImGui::IsItemDeactivatedAfterEdit()) {
                EditorAction action{EditorActionType::TrimWorldActor};
                action.id   = segment->id;
                action.tick = startTick;
                action.kind = std::clamp(endTick, startTick + 1, project->totalTicks);
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            if (property::actionButton("playback.refactorEditor.details.splitAtPlayhead"_tr().c_str())) {
                EditorAction action{EditorActionType::SplitWorldActor};
                action.tick = ctx.state.currentTick;
                submit(std::move(action));
            }
            if (property::actionButton("playback.refactorEditor.details.rippleDelete"_tr().c_str())) {
                EditorAction action{EditorActionType::RippleDeleteWorldActorSegment};
                action.id = segment->id;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            if (segment->locked) {
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.segmentLocked"_tr().c_str());
            }
            property::endSection();
        }
        return true;
    }
    return false;
}

bool DetailsPanel::drawSubActor(PanelContext const& ctx) {
    auto const  project   = ctx.state.project;
    auto const& selection = ctx.selection;
    auto const  submit    = [&ctx](EditorAction action) { ctx.submitAction(std::move(action)); };

    if (auto const* selected = selection.getAs<state::editing::model::SelectedSubActor>()) {
        auto const* actor = findById(project->worldActor.subActors, selected->subActorId);
        if (!actor) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.subActorMissing"_tr().c_str());
            return true;
        }
        if (property::beginSection("playback.refactorEditor.details.subActor"_tr().c_str())) {
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.name"_tr(actor->name.empty() ? actor->id : actor->name).c_str()
            );
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.category"_tr(categoryName(actor->category)).c_str()
            );
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.positionValue"_tr(
                    actor->position.x,
                    actor->position.y,
                    actor->position.z
                )
                    .c_str()
            );
            ImGui::TextUnformatted(
                "playback.refactorEditor.details.rotationValue"_tr(actor->rotation.x, actor->rotation.y).c_str()
            );
            if (actor->boundCameraIds.empty()) {
                ImGui::TextUnformatted("playback.refactorEditor.details.noBoundCameras"_tr().c_str());
            } else {
                ImGui::TextUnformatted("playback.refactorEditor.details.boundCamerasLabel"_tr().c_str());
                for (auto const& cameraId : actor->boundCameraIds) {
                    auto const* camera = findById(project->cameras, cameraId);
                    ImGui::BulletText("%s", camera ? camera->name.c_str() : cameraId.c_str());
                }
            }
            property::separator();
            if (!actor->agentDetails.empty()) {
                if (property::beginSection("playback.refactorEditor.details.agentDetails"_tr().c_str())) {
                    auto updated = actor->agentDetails;
                    bool edited  = false;
                    for (auto& [key, value] : updated) {
                        char buf[256]{};
                        std::snprintf(buf, sizeof(buf), "%s", value.c_str());
                        ImGui::SetNextItemWidth(-1.0f);
                        if (ImGui::InputText(("##agent-" + key).c_str(), buf, sizeof(buf))) value = buf;
                        if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
                    }
                    if (edited) {
                        EditorAction action{EditorActionType::SetSubActorDetails};
                        action.id      = actor->id;
                        action.details = std::move(updated);
                        submit(std::move(action));
                    }
                    property::endSection();
                }
            }
            static char newDetailKey[64]{};
            auto const  newFieldHint = "playback.refactorEditor.details.newDetailField"_tr();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##agent-new-key", newFieldHint.c_str(), newDetailKey, sizeof(newDetailKey));
            if (property::actionButton("playback.refactorEditor.details.addDetailField"_tr().c_str())
                && newDetailKey[0] != '\0') {
                auto details          = actor->agentDetails;
                details[newDetailKey] = "";
                EditorAction action{EditorActionType::SetSubActorDetails};
                action.id      = actor->id;
                action.details = std::move(details);
                submit(std::move(action));
                newDetailKey[0] = '\0';
            }
            ImGui::Spacing();
            if (property::actionButton("playback.refactorEditor.details.createBindingCamera"_tr().c_str())) {
                EditorAction action{EditorActionType::CreateBindingCamera};
                action.id   = actor->id;
                action.name = "playback.refactorEditor.details.bindingCameraName"_tr(actor->name);
                submit(std::move(action));
            }
            property::endSection();
        }
        return true;
    }
    return false;
}

bool DetailsPanel::drawCamera(PanelContext const& ctx) {
    auto const  project   = ctx.state.project;
    auto const& selection = ctx.selection;
    auto const  submit    = [&ctx](EditorAction action) { ctx.submitAction(std::move(action)); };

    if (auto const* selectedCamera = selection.getAs<state::editing::model::SelectedCamera>()) {
        auto const* camera = findById(project->cameras, selectedCamera->cameraId);
        if (!camera) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraMissing"_tr().c_str());
            return true;
        }
        if (property::beginSection("playback.refactorEditor.details.camera"_tr().c_str())) {
            property::textRow("playback.refactorEditor.details.nameLabel"_tr().c_str(), camera->name.c_str());
            std::string const keyframeCount = std::to_string(camera->keysByTick.size());
            property::textRow("playback.refactorEditor.details.keyframesLabel"_tr().c_str(), keyframeCount.c_str());
            bool       enabled      = camera->enabled;
            auto const enabledLabel = "playback.refactorEditor.details.enabled"_tr();
            if (ImGui::Checkbox(enabledLabel.c_str(), &enabled)) {
                EditorAction action{EditorActionType::SetCameraEnabled};
                action.id    = camera->id;
                action.value = enabled;
                submit(std::move(action));
            }
            bool locked = camera->locked;
            if (ImGui::Checkbox("playback.refactorEditor.details.locked"_tr().c_str(), &locked)) {
                EditorAction action{EditorActionType::SetCameraLocked};
                action.id    = camera->id;
                action.value = locked;
                submit(std::move(action));
            }
            ImGui::BeginDisabled(camera->locked);
            if (!camera->bindingEntityUuid.empty()) {
                property::separator();
                auto const* actor = findById(project->worldActor.subActors, camera->bindingEntityUuid);
                ImGui::TextUnformatted(
                    "playback.refactorEditor.details.boundTo"_tr(actor ? actor->name : camera->bindingEntityUuid)
                        .c_str()
                );
                if (property::actionButton("playback.refactorEditor.details.unbindCamera"_tr().c_str())) {
                    EditorAction action{EditorActionType::UnbindCamera};
                    action.id = camera->id;
                    submit(std::move(action));
                }
            }
            if (camera->kind != state::editing::model::CameraTrackKind::Keyframe) {
                // A person track follows each key's subject until its next key; the one under the playhead is shown.
                property::separator();
                int const  playhead  = ctx.state.currentTick;
                auto       governing = camera->keysByTick.upper_bound(playhead);
                if (governing != camera->keysByTick.begin()) --governing;
                std::string const subject =
                    governing == camera->keysByTick.end() ? std::string{} : governing->second.subject;
                std::string const atPlayhead = "playback.refactorEditor.details.trackPlayerAtPlayhead"_tr(
                    playerSubjectLabel(ctx.state.replayPlayers, subject)
                );
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.trackPlayer"_tr().c_str());
                ImGui::TextUnformatted(atPlayhead.c_str());
                PlayerPickerResult picked;
                if (drawPlayerList(
                        "##track-player-list",
                        PlayerPickerMode::SegmentSubject,
                        ctx.state.replayPlayers,
                        subject,
                        false,
                        picked
                    )
                    && picked.uuid != subject) {
                    if (!camera->keysByTick.contains(playhead)) {
                        EditorAction add{EditorActionType::AddCameraKeyframe};
                        add.id   = camera->id;
                        add.tick = playhead;
                        submit(std::move(add));
                    }
                    EditorAction action{EditorActionType::SetKeyframeSubject};
                    action.id          = camera->id;
                    action.tick        = playhead;
                    action.secondaryId = std::move(picked.uuid);
                    submit(std::move(action));
                }
                widgets::itemTooltip("playback.refactorEditor.details.trackPlayerHint"_tr().c_str());
            }
            property::separator();
            if (property::actionButton("playback.refactorEditor.details.addKeyframeAtPlayhead"_tr().c_str())) {
                EditorAction action{EditorActionType::AddCameraKeyframe};
                action.id   = camera->id;
                action.tick = ctx.state.currentTick;
                submit(std::move(action));
            }
            property::separator();
            if (property::actionButton("playback.refactorEditor.details.deleteCamera"_tr().c_str())) {
                EditorAction action{EditorActionType::DeleteCamera};
                action.id = camera->id;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            if (camera->locked) {
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraLocked"_tr().c_str());
            }
            property::endSection();
        }
        if (camera->kind == state::editing::model::CameraTrackKind::ThirdPerson) drawFollowSection(ctx, *camera);
        if (camera->kind == state::editing::model::CameraTrackKind::Keyframe) drawShakeSection(ctx, *camera);
        return true;
    }
    return false;
}

bool DetailsPanel::drawKeyframes(PanelContext const& ctx) {
    auto const& selection = ctx.selection;
    if (selection.keyframeCount() <= 1) return false;
    auto const project = ctx.state.project;

    std::vector<state::editing::model::KeyframeRef> refs;
    std::optional<int>                              shared;
    bool                                            mixed   = false;
    int                                             tracks  = 0;
    std::string                                     lastTrack;
    std::optional<std::string>                      sharedPresetPreview;
    std::optional<std::size_t>                      sharedPreset;
    bool                                            presetMixed = false;
    for (auto const& entry : selection.keyframes()) {
        refs.push_back({entry.trackId, entry.tick, std::nullopt});
        if (entry.trackId != lastTrack) {
            ++tracks;
            lastTrack = entry.trackId;
        }
        auto const* camera = findById(project->cameras, entry.trackId);
        if (!camera) continue;
        auto const key = camera->keysByTick.find(entry.tick);
        if (key == camera->keysByTick.end()) continue;
        int const type = static_cast<int>(key->second.interpolationType);
        if (!shared) shared = type;
        else if (*shared != type) mixed = true;
        auto const preview = easingPresetPreview(key->second);
        if (!sharedPresetPreview) {
            sharedPresetPreview = preview;
            sharedPreset        = keyEasingPreset(key->second);
        } else if (*sharedPresetPreview != preview) {
            presetMixed = true;
        }
    }

    if (property::beginSection("playback.refactorEditor.details.multiKeyframe"_tr().c_str())) {
        property::textRow(
            "playback.refactorEditor.details.keyframeCount"_tr().c_str(),
            std::to_string(selection.keyframeCount()).c_str()
        );
        property::textRow("playback.refactorEditor.details.trackCount"_tr().c_str(), std::to_string(tracks).c_str());
        property::separator();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.interpolation"_tr().c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        std::string const preview =
            mixed || !shared ? "playback.refactorEditor.details.mixed"_tr()
                             : interpolationName(static_cast<state::editing::model::CameraInterpolationType>(*shared));
        if (widgets::beginCombo("##batch-interpolation", preview.c_str())) {
            for (int index = 0; index < 8; ++index) {
                auto const value = static_cast<state::editing::model::CameraInterpolationType>(index);
                if (ImGui::Selectable(interpolationName(value).c_str(), !mixed && shared == index)) {
                    EditorAction action{EditorActionType::SetKeyframesInterpolation};
                    action.keyframeRefs = refs;
                    action.kind         = index;
                    ctx.submitAction(std::move(action));
                }
            }
            widgets::endCombo();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.easingPreset"_tr().c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        std::string const presetPreview =
            presetMixed || !sharedPresetPreview ? "playback.refactorEditor.details.mixed"_tr() : *sharedPresetPreview;
        bool const noneShared =
            !mixed && shared
            && *shared != static_cast<int>(state::editing::model::CameraInterpolationType::CubicBezier);
        if (auto const picked = drawEasingPresetCombo(
                "##batch-easing-preset",
                presetPreview.c_str(),
                presetMixed ? std::nullopt : sharedPreset,
                noneShared
            )) {
            ctx.submitAction(easingAction(refs, *picked));
        }
        property::separator();
        if (property::actionButton(
                "playback.refactorEditor.details.deleteKeyframes"_tr(selection.keyframeCount()).c_str()
            )) {
            EditorAction action{EditorActionType::DeleteCameraKeyframes};
            action.keyframeRefs = std::move(refs);
            ctx.submitAction(std::move(action));
            ctx.selection.clear();
        }
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.multiKeyframeHint"_tr().c_str());
        property::endSection();
    }
    return true;
}

bool DetailsPanel::drawKeyframe(PanelContext const& ctx) {
    auto const  project   = ctx.state.project;
    auto const& selection = ctx.selection;
    auto const  submit    = [&ctx](EditorAction action) { ctx.submitAction(std::move(action)); };

    if (auto const* selected = selection.getAs<state::editing::model::SelectedKeyframe>()) {
        auto const* camera = findById(project->cameras, selected->trackId);
        if (!camera) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeMissing"_tr().c_str());
            return true;
        }
        auto const key = camera->keysByTick.find(selected->tick);
        if (key == camera->keysByTick.end()) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeMissing"_tr().c_str());
            return true;
        }
        if (property::beginSection("playback.refactorEditor.details.cameraKeyframe"_tr().c_str())) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.camera"_tr().c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(camera->name.c_str());
            property::separator();
            ImGui::BeginDisabled(camera->locked);
            bool const      person       = camera->kind != state::editing::model::CameraTrackKind::Keyframe;
            int const       originalTick = key->first;
            int             tick         = originalTick;
            constexpr float labelWidth   = 72.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.0f, 5.0f});
            if (ImGui::BeginTable(
                    "##keyframe-properties",
                    2,
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
                )) {
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.tick"_tr().c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputInt("##tick", &tick, 0, 0);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    EditorAction action{EditorActionType::MoveCameraKeyframe};
                    action.id            = camera->id;
                    action.tick          = originalTick;
                    action.secondaryTick = tick;
                    submit(std::move(action));
                }

                // Person tracks only use their keys for range and subject; the view comes from the player.
                if (!person) {
                    float position[3] = {key->second.position.x, key->second.position.y, key->second.position.z};
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.position"_tr().c_str());
                    ImGui::TableNextColumn();
                    static constexpr std::array positionLabels{"X", "Y", "Z"};
                    static std::array const     positionColors{
                        ImGui::ColorConvertU32ToFloat4(theme::kAxisX),
                        ImGui::ColorConvertU32ToFloat4(theme::kAxisY),
                        ImGui::ColorConvertU32ToFloat4(theme::kAxisZ),
                    };
                    if (vectorInput("position", position, positionLabels, &positionColors, "%.3f")
                        && std::ranges::all_of(position, [](float value) { return std::isfinite(value); })) {
                        EditorAction action{EditorActionType::SetKeyframePosition};
                        action.id       = camera->id;
                        action.tick     = originalTick;
                        action.position = {position[0], position[1], position[2]};
                        submit(std::move(action));
                    }

                    float rotation[3] = {key->second.yaw, key->second.pitch, key->second.roll};
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.rotation"_tr().c_str());
                    ImGui::TableNextColumn();
                    static constexpr std::array rotationLabels{"Yaw", "Pitch", "Roll"};
                    if (vectorInput("rotation", rotation, rotationLabels, nullptr, "%.2f")
                        && std::ranges::all_of(rotation, [](float value) { return std::isfinite(value); })) {
                        EditorAction action{EditorActionType::SetKeyframeRotation};
                        action.id       = camera->id;
                        action.tick     = originalTick;
                        action.position = {rotation[0], rotation[1], rotation[2]};
                        submit(std::move(action));
                    }

                    float fov = key->second.fov;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("FOV");
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputFloat("##fov", &fov, 0.0f, 0.0f, "%.2f°");
                    if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(fov)) {
                        EditorAction action{EditorActionType::SetKeyframeFov};
                        action.id    = camera->id;
                        action.tick  = originalTick;
                        action.speed = std::clamp(fov, 1.0f, 179.0f);
                        submit(std::move(action));
                    }

                    // Strength and frequency go together so one edit is one undo step.
                    float      shake          = key->second.shake;
                    float      shakeFrequency = key->second.shakeFrequency;
                    auto const commitShake    = [&] {
                        if (!std::isfinite(shake) || !std::isfinite(shakeFrequency)) return;
                        EditorAction action{EditorActionType::SetKeyframeShake};
                        action.id             = camera->id;
                        action.tick           = originalTick;
                        action.speed          = shake;
                        action.shakeFrequency = shakeFrequency;
                        submit(std::move(action));
                    };
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.shakeStrength"_tr().c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputFloat("##shake", &shake, 0.0f, 0.0f, "%.2f");
                    if (ImGui::IsItemDeactivatedAfterEdit()) commitShake();
                    widgets::itemTooltip("playback.refactorEditor.details.shakeStrengthHint"_tr().c_str());

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.shakeFrequency"_tr().c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputFloat("##shake-frequency", &shakeFrequency, 0.0f, 0.0f, "%.2f Hz");
                    if (ImGui::IsItemDeactivatedAfterEdit()) commitShake();

                    int interpolation = static_cast<int>(key->second.interpolationType);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.interpolation"_tr().c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    if (widgets::beginCombo(
                            "##interpolation",
                            interpolationName(key->second.interpolationType).c_str()
                        )) {
                        for (int index = 0; index < 8; ++index) {
                            auto const value = static_cast<state::editing::model::CameraInterpolationType>(index);
                            if (ImGui::Selectable(interpolationName(value).c_str(), index == interpolation)) {
                                EditorAction action{EditorActionType::SetKeyframeInterpolation};
                                action.id   = camera->id;
                                action.tick = originalTick;
                                action.kind = index;
                                submit(std::move(action));
                            }
                        }
                        widgets::endCombo();
                    }

                    auto const preset = keyEasingPreset(key->second);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", "playback.refactorEditor.details.easingPreset"_tr().c_str());
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-1.0f);
                    if (auto const picked = drawEasingPresetCombo(
                            "##easing-preset",
                            easingPresetPreview(key->second).c_str(),
                            preset,
                            key->second.interpolationType != state::editing::model::CameraInterpolationType::CubicBezier
                        )) {
                        submit(easingAction(
                            {
                                {camera->id, originalTick, std::nullopt}
                        },
                            *picked
                        ));
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
            if (!person
                && key->second.interpolationType == state::editing::model::CameraInterpolationType::CubicBezier) {
                // A key's curve eases the move from it to the next key.
                property::separator();
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.easingCurve"_tr().c_str());
                if (auto const edit =
                        drawEasingCurveEditor("##easing-curve", key->second.bezierCtrl1, key->second.bezierCtrl2);
                    edit && edit->committed) {
                    EditorAction action{EditorActionType::SetKeyframesInterpolation};
                    action.keyframeRefs = {
                        {camera->id, originalTick, std::nullopt}
                    };
                    action.kind = static_cast<int>(state::editing::model::CameraInterpolationType::CubicBezier);
                    action.bezierControls = edit->controls;
                    submit(std::move(action));
                }
            }
            if (person) {
                // The subject holds from this key until the next key on the same track.
                property::separator();
                std::string const current = playerSubjectLabel(ctx.state.replayPlayers, key->second.subject);
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.keyframeSubject"_tr().c_str());
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(current.c_str());
                ImGui::PopTextWrapPos();
                PlayerPickerResult picked;
                if (drawPlayerList(
                        "##keyframe-subject-list",
                        PlayerPickerMode::SegmentSubject,
                        ctx.state.replayPlayers,
                        key->second.subject,
                        false,
                        picked
                    )
                    && picked.uuid != key->second.subject) {
                    EditorAction action{EditorActionType::SetKeyframeSubject};
                    action.id          = camera->id;
                    action.tick        = originalTick;
                    action.secondaryId = std::move(picked.uuid);
                    submit(std::move(action));
                }
            }
            property::separator();
            if (property::actionButton("playback.refactorEditor.details.deleteKeyframe"_tr().c_str())) {
                EditorAction action{EditorActionType::DeleteCameraKeyframe};
                action.id   = camera->id;
                action.tick = originalTick;
                submit(std::move(action));
            }
            ImGui::EndDisabled();
            if (camera->locked) {
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.cameraLocked"_tr().c_str());
            }
            property::endSection();
        }
        return true;
    }
    return false;
}

bool DetailsPanel::drawMarker(PanelContext const& ctx) {
    auto const  project   = ctx.state.project;
    auto const& selection = ctx.selection;

    if (auto const* selected = selection.getAs<state::editing::model::SelectedMarker>()) {
        auto const* marker = findById(project->markers, selected->markerId);
        if (!marker) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.details.markerMissing"_tr().c_str());
            return true;
        }
        if (property::beginSection("playback.refactorEditor.details.marker"_tr().c_str())) {
            if (mMarkerLabelId != marker->id || !mMarkerLabelEditing) {
                mMarkerLabelId     = marker->id;
                mMarkerLabelBuffer = {};
                marker->label.copy(mMarkerLabelBuffer.data(), mMarkerLabelBuffer.size() - 1);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4.0f, 5.0f});
            if (ImGui::BeginTable(
                    "##marker-properties",
                    2,
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
                )) {
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, 72.0f);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.nameLabel"_tr().c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##marker-label", mMarkerLabelBuffer.data(), mMarkerLabelBuffer.size());
                mMarkerLabelEditing = ImGui::IsItemActive();
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    std::string label = mMarkerLabelBuffer.data();
                    if (!label.empty() && label != marker->label) {
                        EditorAction action{EditorActionType::RenameMarker};
                        action.id   = marker->id;
                        action.name = std::move(label);
                        ctx.submitAction(std::move(action));
                    }
                }

                int tick = marker->tick;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", "playback.refactorEditor.details.tick"_tr().c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputInt("##marker-tick", &tick, 0, 0);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    tick = std::clamp(tick, 0, ctx.state.totalTicks);
                    if (tick != marker->tick) {
                        EditorAction action{EditorActionType::MoveMarker};
                        action.id   = marker->id;
                        action.tick = tick;
                        ctx.submitAction(std::move(action));
                    }
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", formatTick(marker->tick).c_str());
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
            property::separator();
            if (property::actionButton("playback.refactorEditor.timeline.seekToMarker"_tr().c_str())) {
                ctx.commands.seekTo(marker->tick);
            }
            if (property::actionButton("playback.refactorEditor.details.deleteMarker"_tr().c_str())) {
                EditorAction action{EditorActionType::DeleteMarker};
                action.id = marker->id;
                ctx.submitAction(std::move(action));
                ctx.selection.clear();
            }
            property::endSection();
        }
        return true;
    }
    return false;
}

void DetailsPanel::drawOverview(PanelContext const& ctx) {
    auto const project = ctx.state.project;
    auto const submit  = [&ctx](EditorAction action) { ctx.submitAction(std::move(action)); };

    if (property::beginSection("playback.refactorEditor.details.overview"_tr().c_str())) {
        ImGui::TextDisabled("%s", "playback.refactorEditor.details.selectionHint"_tr().c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(
            "playback.refactorEditor.details.name"_tr(
                project->worldActor.name.empty() ? "playback.refactorEditor.details.untitled"_tr()
                                                 : project->worldActor.name
            )
                .c_str()
        );
        ImGui::TextUnformatted("playback.refactorEditor.details.total"_tr(formatTick(project->totalTicks)).c_str());
        ImGui::TextUnformatted("playback.refactorEditor.details.cameras"_tr(project->cameras.size()).c_str());
        ImGui::TextUnformatted(
            "playback.refactorEditor.details.segments"_tr(project->worldActor.segments.size()).c_str()
        );
        ImGui::Spacing();
        if (property::actionButton("playback.refactorEditor.details.addFreeCamera"_tr().c_str())) {
            submit({EditorActionType::AddFreeCamera});
        }
        property::endSection();
    }
}

void DetailsPanel::drawExportPage(PanelContext const& ctx) {
    auto const& state        = ctx.state;
    auto const& status       = state.exportStatus;
    bool const  exportActive = exporting::isExportActive(status.state);

    property::beginInspector(
        "playback.refactorEditor.inspector.export"_tr(),
        state.capabilities.ffmpegVideoExport ? "playback.refactorEditor.export.mp4"_tr()
                                             : "playback.refactorEditor.export.pngSequence"_tr()
    );

    if (property::beginSection("playback.refactorEditor.export.output"_tr().c_str())) {
        property::textRow(
            "playback.refactorEditor.export.frameRate"_tr().c_str(),
            (std::to_string(static_cast<int>(state.project->fps)) + " FPS").c_str()
        );
        property::textRow(
            "playback.refactorEditor.export.timeline"_tr().c_str(),
            (formatTick(0) + " - " + formatTick(state.totalTicks)).c_str()
        );
        if (!state.capabilities.ffmpegVideoExport) {
            ImGui::TextDisabled("%s  %s", ICON_INFO, "playback.refactorEditor.export.ffmpegUnavailable"_tr().c_str());
        }
        if (property::actionButton(
                "playback.refactorEditor.inspector.openExportDialog"_tr().c_str(),
                state.capabilities.videoExport && !exportActive && state.totalTicks > 0
            )) {
            ctx.commands.openExportDialog();
        }
        property::endSection();
    }

    if (property::beginSection("playback.refactorEditor.inspector.exportStatus"_tr().c_str())) {
        if (status.state == exporting::ExportState::Idle) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.export.state.idle"_tr().c_str());
        } else {
            if (status.totalFrames > 0) {
                auto const completed =
                    std::min(status.totalFrames, std::max(status.submittedFrames, status.writtenFrames));
                float const fraction = static_cast<float>(completed) / static_cast<float>(status.totalFrames);
                ImGui::ProgressBar(fraction, {-1.0f, ImGui::GetFontSize() + 4.0f});
            }
            if (!status.message.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(status.message.c_str());
                ImGui::PopTextWrapPos();
            }
            if (property::actionButton("playback.refactorEditor.export.cancel"_tr().c_str(), exportActive)) {
                ctx.submitAction({EditorActionType::CancelExport});
            }
        }
        property::endSection();
    }
}

void DetailsPanel::drawPreviewPage(PanelContext const& ctx) {
    property::beginInspector(
        "playback.refactorEditor.inspector.preview"_tr(),
        "playback.refactorEditor.inspector.previewSubtitle"_tr()
    );
    auto const hint = [](std::string const& text) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", text.c_str());
        ImGui::PopTextWrapPos();
    };

    // An export renders what these say, so they stay put while one is running.
    ImGui::BeginDisabled(exporting::isExportActive(ctx.state.exportStatus.state));

    if (property::beginSection("playback.refactorEditor.inspector.hud"_tr().c_str())) {
        struct Toggle {
            visuals::HudElement element;
            std::string         label;
        };
        std::array const toggles{
            Toggle{visuals::HudElement::Chat,       "playback.refactorEditor.inspector.hudChat"_tr()      },
            Toggle{visuals::HudElement::BossBar,    "playback.refactorEditor.inspector.hudBossBar"_tr()   },
            Toggle{visuals::HudElement::Title,      "playback.refactorEditor.inspector.hudTitle"_tr()     },
            Toggle{visuals::HudElement::Scoreboard, "playback.refactorEditor.inspector.hudScoreboard"_tr()},
            Toggle{visuals::HudElement::ActionBar,  "playback.refactorEditor.inspector.hudActionBar"_tr() },
            Toggle{visuals::HudElement::Hotbar,     "playback.refactorEditor.inspector.hudHotbar"_tr()    },
        };
        for (auto const& toggle : toggles) {
            bool shown = ctx.commands.isHudElementShown(toggle.element);
            ImGui::PushID(static_cast<int>(toggle.element));
            if (ImGui::Checkbox((toggle.label + "##set-hud").c_str(), &shown))
                ctx.commands.setHudElementShown(toggle.element, shown);
            ImGui::PopID();
            if (toggle.element == visuals::HudElement::Hotbar)
                widgets::itemTooltip("playback.refactorEditor.inspector.hudHotbarHint"_tr().c_str());
        }
        hint("playback.refactorEditor.inspector.hudHint"_tr());
        property::endSection();
    }

    if (property::beginSection("playback.refactorEditor.inspector.previewAudio"_tr().c_str())) {
        bool muted = ctx.commands.isPreviewAudioMuted();
        if (ImGui::Checkbox(("playback.refactorEditor.inspector.previewMute"_tr() + "##set-mute").c_str(), &muted))
            ctx.commands.setPreviewAudioMuted(muted);

        ImGui::BeginDisabled(muted);
        ImGui::TextUnformatted("playback.refactorEditor.inspector.previewVolume"_tr().c_str());
        float percent = ctx.commands.previewAudioVolume() * 100.0f;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##set-preview-volume", &percent, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
            ctx.commands.setPreviewAudioVolume(percent / 100.0f, false);
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.commands.setPreviewAudioVolume(percent / 100.0f, true);
        ImGui::EndDisabled();

        bool silent = ctx.commands.isPreviewSilentWhilePaused();
        if (ImGui::Checkbox(
                ("playback.refactorEditor.inspector.previewSilentPaused"_tr() + "##set-silent-paused").c_str(),
                &silent
            ))
            ctx.commands.setPreviewSilentWhilePaused(silent);
        widgets::itemTooltip("playback.refactorEditor.inspector.previewSilentPausedHint"_tr().c_str());
        hint("playback.refactorEditor.inspector.previewAudioHint"_tr());
        property::endSection();
    }

    drawWorldSection(ctx);
    drawShaderSection(ctx);

    ImGui::EndDisabled();
}

void DetailsPanel::drawSettingsPage(PanelContext const& ctx) {
    property::beginInspector(
        "playback.refactorEditor.inspector.settings"_tr(),
        "playback.refactorEditor.inspector.settingsSubtitle"_tr()
    );

    if (property::beginSection("playback.refactorEditor.inspector.uiScale"_tr().c_str())) {
        struct Tier {
            UiScaleTier tier;
            std::string label;
        };
        std::array const tiers{
            Tier{UiScaleTier::Auto,   "playback.refactorEditor.inspector.uiScaleAuto"_tr()  },
            Tier{UiScaleTier::Small,  "playback.refactorEditor.inspector.uiScaleSmall"_tr() },
            Tier{UiScaleTier::Medium, "playback.refactorEditor.inspector.uiScaleMedium"_tr()},
            Tier{UiScaleTier::Large,  "playback.refactorEditor.inspector.uiScaleLarge"_tr() },
            Tier{UiScaleTier::Huge,   "playback.refactorEditor.inspector.uiScaleHuge"_tr()  },
        };
        UiScaleTier const current = ctx.commands.uiScaleTier();
        for (auto const& entry : tiers) {
            if (ImGui::RadioButton(entry.label.c_str(), current == entry.tier)) ctx.commands.setUiScaleTier(entry.tier);
        }
        if (current == UiScaleTier::Auto) {
            ImGui::TextDisabled(
                "%s",
                "playback.refactorEditor.inspector.uiScaleAutoHint"_tr(
                    static_cast<int>(calculateReplayUIScale(current, ImGui::GetIO().DisplaySize.y) * 100.0f)
                )
                    .c_str()
            );
        }
        property::endSection();
    }

    if (property::beginSection("playback.refactorEditor.inspector.viewSettings"_tr().c_str())) {
        bool infoOverlay = ctx.commands.isInfoOverlayVisible();
        if (ImGui::Checkbox(("playback.refactorEditor.viewport.infoOverlay"_tr() + "##set-info").c_str(), &infoOverlay))
            ctx.commands.setInfoOverlayVisible(infoOverlay);
        bool autoPreview = ctx.commands.isAutoPreviewEnabled();
        if (ImGui::Checkbox(("playback.refactorEditor.viewport.autoPreview"_tr() + "##set-auto").c_str(), &autoPreview))
            ctx.commands.setAutoPreviewEnabled(autoPreview);
        bool cameraPath = ctx.commands.isCameraPathVisible();
        if (ImGui::Checkbox(("playback.refactorEditor.menu.cameraPath"_tr() + "##set-path").c_str(), &cameraPath))
            ctx.commands.toggleCameraPath();
        bool snap = ctx.commands.isSnapEnabled();
        if (ImGui::Checkbox(("playback.refactorEditor.timeline.snap"_tr() + "##set-snap").c_str(), &snap))
            ctx.commands.setSnapEnabled(snap);
        property::endSection();
    }

    drawOptiPistonSection();

    if (property::beginSection("playback.refactorEditor.viewport.aspectTooltip"_tr().c_str())) {
        struct Preset {
            char const* label;
            float       ratio;
        };
        static constexpr std::array presets{
            Preset{"16:9",   16.0f / 9.0f},
            Preset{"21:9",   21.0f / 9.0f},
            Preset{"4:3",    4.0f / 3.0f },
            Preset{"1:1",    1.0f        },
            Preset{"9:16",   9.0f / 16.0f},
            Preset{"2.39:1", 2.39f       },
        };
        float const current = ctx.commands.videoAspectRatio();
        for (auto const& preset : presets) {
            if (ImGui::RadioButton(preset.label, std::abs(current - preset.ratio) < 0.01f))
                ctx.commands.setVideoAspectRatio(preset.ratio);
        }
        property::endSection();
    }
}

void DetailsPanel::drawWorldSection(PanelContext const& ctx) {
    using editing::model::WorldSettings;
    if (!ctx.state.project) return;
    if (!property::beginSection("playback.refactorEditor.inspector.world"_tr().c_str())) return;

    auto const submit = [&ctx](EditorActionType type, std::optional<WorldSettings> settings) {
        EditorAction action{type};
        action.worldSettings = settings;
        ctx.submitAction(std::move(action));
    };
    // A dragged slider or colour shows live and becomes one undo step once no control is active any more.
    WorldSettings world  = mWorldDraft.value_or(ctx.state.project->world);
    auto const    commit = [&](WorldSettings const& edited) {
        mWorldDraft.reset();
        submit(EditorActionType::SetWorldSettings, edited);
    };
    auto const drag = [&](bool changed, WorldSettings const& edited) {
        if (!changed) return;
        mWorldDraft = edited;
        submit(EditorActionType::PreviewWorldSettings, edited);
    };

    if (ImGui::Checkbox(
            ("playback.refactorEditor.inspector.worldOverrideTime"_tr() + "##set-override-time").c_str(),
            &world.overrideTime
        ))
        commit(world);

    ImGui::BeginDisabled(!world.overrideTime);
    int timeOfDay = world.timeOfDay;
    // Minecraft's day starts at 06:00: 1000 ticks are one hour.
    int const  minutes = (timeOfDay * 60 / 1000 + 6 * 60) % (24 * 60);
    std::array clock{'0', '0', ':', '0', '0', '\0'};
    std::snprintf(clock.data(), clock.size(), "%02d:%02d", minutes / 60, minutes % 60);
    ImGui::SetNextItemWidth(-1.0f);
    bool changed = ImGui::SliderInt(
        "##set-time-of-day",
        &timeOfDay,
        0,
        WorldSettings::TicksPerDay - 1,
        clock.data(),
        ImGuiSliderFlags_AlwaysClamp
    );
    auto edited      = world;
    edited.timeOfDay = timeOfDay;
    drag(changed, edited);

    if (ImGui::Checkbox(
            ("playback.refactorEditor.inspector.worldFreezeTime"_tr() + "##set-freeze-time").c_str(),
            &world.freezeTime
        ))
        commit(world);
    widgets::itemTooltip("playback.refactorEditor.inspector.worldFreezeTimeHint"_tr().c_str());
    ImGui::EndDisabled();

    if (ImGui::Checkbox(
            ("playback.refactorEditor.inspector.worldHideClouds"_tr() + "##set-hide-clouds").c_str(),
            &world.hideClouds
        ))
        commit(world);

    property::separator();
    if (ImGui::Checkbox(
            ("playback.refactorEditor.inspector.worldOverrideFog"_tr() + "##set-override-fog").c_str(),
            &world.overrideFog
        ))
        commit(world);
    ImGui::BeginDisabled(!world.overrideFog);
    // Shown in percent of the render distance; stored as fractions.
    float const maxPercent = WorldSettings::MaxFogDistance * 100.0f;
    float       start      = world.fogStart * 100.0f;
    float       end        = world.fogEnd * 100.0f;
    ImGui::TextDisabled("%s", "playback.refactorEditor.inspector.worldFogStart"_tr().c_str());
    ImGui::SetNextItemWidth(-1.0f);
    changed = ImGui::SliderFloat("##set-fog-start", &start, 0.0f, maxPercent, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    widgets::itemTooltip("playback.refactorEditor.inspector.worldFogDistanceHint"_tr().c_str());
    edited          = world;
    edited.fogStart = start / 100.0f;
    edited.fogEnd   = std::max(edited.fogEnd, edited.fogStart);
    drag(changed, edited);
    ImGui::TextDisabled("%s", "playback.refactorEditor.inspector.worldFogEnd"_tr().c_str());
    ImGui::SetNextItemWidth(-1.0f);
    changed = ImGui::SliderFloat("##set-fog-end", &end, 0.0f, maxPercent, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    widgets::itemTooltip("playback.refactorEditor.inspector.worldFogDistanceHint"_tr().c_str());
    edited          = world;
    edited.fogEnd   = end / 100.0f;
    edited.fogStart = std::min(edited.fogStart, edited.fogEnd);
    drag(changed, edited);

    if (ImGui::Checkbox(
            ("playback.refactorEditor.inspector.worldOverrideFogColor"_tr() + "##set-override-fog-color").c_str(),
            &world.overrideFogColor
        ))
        commit(world);
    widgets::itemTooltip("playback.refactorEditor.inspector.worldFogColorHint"_tr().c_str());
    ImGui::BeginDisabled(!world.overrideFogColor);
    auto color = world.fogColor;
    ImGui::SetNextItemWidth(-1.0f);
    changed = ImGui::ColorEdit3("##set-fog-color", color.data(), ImGuiColorEditFlags_Float);
    edited          = world;
    edited.fogColor = color;
    drag(changed, edited);
    ImGui::EndDisabled();

    ImGui::EndDisabled();

    property::separator();
    if (ImGui::Checkbox(
            ("playback.refactorEditor.inspector.worldOverrideWeather"_tr() + "##set-override-weather").c_str(),
            &world.overrideWeather
        ))
        commit(world);
    ImGui::BeginDisabled(!world.overrideWeather);
    // Indexed by WeatherStyle.
    std::array const weatherStyles{
        "playback.refactorEditor.inspector.worldWeatherClear"_tr(),
        "playback.refactorEditor.inspector.worldWeatherRain"_tr(),
        "playback.refactorEditor.inspector.worldWeatherThunder"_tr(),
    };
    auto const currentWeather = static_cast<std::size_t>(world.weather);
    ImGui::SetNextItemWidth(-1.0f);
    if (widgets::beginCombo("##set-weather", weatherStyles[currentWeather].c_str())) {
        for (std::size_t index = 0; index < weatherStyles.size(); ++index) {
            if (ImGui::Selectable(weatherStyles[index].c_str(), index == currentWeather)) {
                world.weather = static_cast<editing::model::WeatherStyle>(index);
                commit(world);
            }
        }
        widgets::endCombo();
    }
    widgets::itemTooltip("playback.refactorEditor.inspector.worldWeatherHint"_tr().c_str());
    ImGui::EndDisabled();

    property::separator();
    if (ImGui::Checkbox(
            ("playback.refactorEditor.inspector.worldOverrideFov"_tr() + "##set-override-fov").c_str(),
            &world.overrideFov
        ))
        commit(world);
    widgets::itemTooltip("playback.refactorEditor.inspector.worldFovHint"_tr().c_str());
    ImGui::BeginDisabled(!world.overrideFov);
    float fov = world.fov;
    ImGui::SetNextItemWidth(-1.0f);
    changed = ImGui::SliderFloat(
        "##set-fov",
        &fov,
        WorldSettings::MinFov,
        WorldSettings::MaxFov,
        "%.0f°",
        ImGuiSliderFlags_AlwaysClamp
    );
    edited     = world;
    edited.fov = fov;
    drag(changed, edited);
    ImGui::EndDisabled();

    property::separator();
    if (ImGui::Checkbox(
            ("playback.refactorEditor.inspector.worldOverrideGraphics"_tr() + "##set-override-graphics").c_str(),
            &world.overrideGraphics
        ))
        commit(world);
    ImGui::BeginDisabled(!world.overrideGraphics);
    // Indexed by GraphicsStyle.
    std::array const graphicsStyles{
        "playback.refactorEditor.inspector.worldGraphicsSimple"_tr(),
        "playback.refactorEditor.inspector.worldGraphicsFancy"_tr(),
        "playback.refactorEditor.inspector.worldGraphicsVibrant"_tr(),
    };
    auto const currentStyle = static_cast<std::size_t>(world.graphics);
    ImGui::SetNextItemWidth(-1.0f);
    if (widgets::beginCombo("##set-graphics", graphicsStyles[currentStyle].c_str())) {
        for (std::size_t index = 0; index < graphicsStyles.size(); ++index) {
            if (ImGui::Selectable(graphicsStyles[index].c_str(), index == currentStyle)) {
                world.graphics = static_cast<editing::model::GraphicsStyle>(index);
                commit(world);
            }
        }
        widgets::endCombo();
    }
    widgets::itemTooltip("playback.refactorEditor.inspector.worldGraphicsHint"_tr().c_str());
    ImGui::EndDisabled();

    property::separator();
    if (ImGui::Checkbox(
            ("playback.refactorEditor.inspector.worldOverrideGlobalPacks"_tr() + "##set-override-global-packs").c_str(),
            &world.overrideGlobalPacks
        ))
        commit(world);
    ImGui::BeginDisabled(!world.overrideGlobalPacks);
    {
        auto const& installed   = ctx.state.installedGlobalPacks;
        auto const  isInstalled = [&](editing::model::GlobalPack const& pack) {
            return installed && std::ranges::any_of(*installed, [&](auto const& entry) {
                       return entry.id == pack.id && entry.version == pack.version;
                   });
        };
        std::optional<WorldSettings> packEdit;
        if (world.globalPacks.empty()) {
            ImGui::TextDisabled("%s", "playback.refactorEditor.inspector.worldGlobalPacksEmpty"_tr().c_str());
        }
        // Top priority first, as the game lists global packs.
        for (std::size_t index = 0; index < world.globalPacks.size(); ++index) {
            auto const& pack = world.globalPacks[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::BeginDisabled(index == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
                packEdit = world;
                std::swap(packEdit->globalPacks[index], packEdit->globalPacks[index - 1]);
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::BeginDisabled(index + 1 == world.globalPacks.size());
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
                packEdit = world;
                std::swap(packEdit->globalPacks[index], packEdit->globalPacks[index + 1]);
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::Button(ICON_CLOSE "##remove")) {
                packEdit = world;
                packEdit->globalPacks.erase(packEdit->globalPacks.begin() + static_cast<std::ptrdiff_t>(index));
            }
            widgets::itemTooltip("playback.refactorEditor.inspector.worldGlobalPackRemove"_tr().c_str());
            ImGui::SameLine();
            if (isInstalled(pack)) {
                ImGui::Text("%s  %s", pack.name.c_str(), pack.version.c_str());
            } else {
                ImGui::TextDisabled(
                    "%s  %s (%s)",
                    pack.name.c_str(),
                    pack.version.c_str(),
                    "playback.refactorEditor.inspector.worldGlobalPackMissing"_tr().c_str()
                );
            }
            ImGui::PopID();
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (widgets::beginCombo(
                "##add-global-pack",
                "playback.refactorEditor.inspector.worldGlobalPacksAdd"_tr().c_str()
            )) {
            bool listed = false;
            if (installed) {
                for (auto const& pack : *installed) {
                    if (std::ranges::find(world.globalPacks, pack) != world.globalPacks.end()) continue;
                    listed = true;
                    if (ImGui::Selectable((pack.name + "  " + pack.version + "##" + pack.id + pack.version).c_str())) {
                        packEdit = world;
                        packEdit->globalPacks.push_back(pack);
                    }
                }
            }
            if (!listed) {
                ImGui::TextDisabled(
                    "%s",
                    "playback.refactorEditor.inspector.worldGlobalPacksNoneInstalled"_tr().c_str()
                );
            }
            widgets::endCombo();
        }
        if (packEdit) commit(*packEdit);
    }
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "playback.refactorEditor.inspector.worldGlobalPacksHint"_tr().c_str());
    ImGui::PopTextWrapPos();

    property::separator();
    if (ImGui::Checkbox(
            ("playback.refactorEditor.inspector.worldOverrideColor"_tr() + "##set-override-color").c_str(),
            &world.overrideColor
        ))
        commit(world);
    ImGui::BeginDisabled(!world.overrideColor);
    auto const colorSlider = [&](std::string const& label,
                                 char const*        id,
                                 float WorldSettings::* field,
                                 float                  low,
                                 float                  high,
                                 char const*            format) {
        ImGui::TextDisabled("%s", label.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        float      value   = world.*field;
        bool const changed = ImGui::SliderFloat(id, &value, low, high, format, ImGuiSliderFlags_AlwaysClamp);
        auto       edited  = world;
        edited.*field      = value;
        drag(changed, edited);
    };
    colorSlider(
        "playback.refactorEditor.inspector.worldExposure"_tr(),
        "##set-exposure",
        &WorldSettings::exposure,
        -WorldSettings::MaxExposure,
        WorldSettings::MaxExposure,
        "%+.2f EV"
    );
    colorSlider(
        "playback.refactorEditor.inspector.worldTemperature"_tr(),
        "##set-temperature",
        &WorldSettings::temperature,
        -1.0f,
        1.0f,
        "%+.2f"
    );
    colorSlider(
        "playback.refactorEditor.inspector.worldTint"_tr(),
        "##set-tint",
        &WorldSettings::tint,
        -1.0f,
        1.0f,
        "%+.2f"
    );
    widgets::itemTooltip("playback.refactorEditor.inspector.worldColorHint"_tr().c_str());
    ImGui::EndDisabled();

    if (mWorldDraft && !ImGui::IsAnyItemActive()) commit(*mWorldDraft);

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "playback.refactorEditor.inspector.worldHint"_tr().c_str());
    ImGui::PopTextWrapPos();
    property::endSection();
}

bool DetailsPanel::drawPoseKey(PanelContext const& ctx) {
    using editing::model::PoseKey;
    auto const* selected = ctx.selection.getAs<state::editing::model::SelectedPoseKey>();
    if (!selected || !ctx.state.project) return false;
    auto const& keys  = ctx.state.project->poseKeys;
    auto const  found = std::ranges::find(keys, selected->poseKeyId, &PoseKey::id);
    if (found == keys.end()) return false;
    auto const& key = *found;

    auto const commit = [&](auto&& change) {
        auto edited = keys;
        change(*std::ranges::find(edited, key.id, &PoseKey::id));
        EditorAction action{EditorActionType::SetPoseKeys};
        action.poseKeys = std::move(edited);
        ctx.submitAction(std::move(action));
    };

    if (!property::beginSection("playback.refactorEditor.details.poseKey"_tr().c_str())) return true;

    // The entity: any player or mob in the replay, as the viewport's right-click menu lists them.
    ImGui::TextDisabled("%s", "playback.refactorEditor.details.poseEntity"_tr().c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##pose-entity", key.subjectLabel.empty() ? key.subject.c_str() : key.subjectLabel.c_str())) {
        for (auto const& entity : replay::ReplaySession::getInstance().replayEntities()) {
            if (entity.recorder || entity.choice.empty()) continue;
            if (ImGui::Selectable(entity.label.c_str(), entity.choice == key.subject) && entity.choice != key.subject) {
                commit([&](PoseKey& edited) {
                    edited.subject      = entity.choice;
                    edited.subjectLabel = entity.label;
                });
            }
        }
        ImGui::EndCombo();
    }

    // The pose: the bundled one, back to the recorded animation, or any animation a loaded pack defines.
    ImGui::TextDisabled("%s", "playback.refactorEditor.details.posePose"_tr().c_str());
    struct Preset {
        std::string_view animation;
        std::string      label;
    };
    std::array const presets{
        Preset{PoseKey::DefaultAnimation, "playback.refactorEditor.details.poseLeanCrossedArms"_tr()},
        Preset{{},                        "playback.refactorEditor.details.poseInGame"_tr()         },
    };
    auto const preset = std::ranges::find(presets, std::string_view{key.animation}, &Preset::animation);
    std::string const current =
        preset != presets.end() ? preset->label : "playback.refactorEditor.details.poseCustom"_tr();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##pose-preset", current.c_str())) {
        for (auto const& entry : presets) {
            if (ImGui::Selectable(entry.label.c_str(), entry.animation == key.animation)
                && entry.animation != key.animation) {
                commit([&](PoseKey& edited) { edited.animation = entry.animation; });
            }
        }
        ImGui::EndCombo();
    }
    std::array<char, 256> animation{};
    std::snprintf(animation.data(), animation.size(), "%s", key.animation.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint(
            "##pose-animation",
            "playback.refactorEditor.details.poseAnimationHint"_tr().c_str(),
            animation.data(),
            animation.size(),
            ImGuiInputTextFlags_EnterReturnsTrue
        )) {
        commit([&](PoseKey& edited) { edited.animation = animation.data(); });
    }
    widgets::itemTooltip("playback.refactorEditor.details.poseAnimationTooltip"_tr().c_str());

    ImGui::BeginDisabled(key.animation.empty());
    bool hideItems = key.hideItems;
    if (ImGui::Checkbox("playback.refactorEditor.details.poseEmptyHands"_tr().c_str(), &hideItems)) {
        commit([&](PoseKey& edited) { edited.hideItems = hideItems; });
    }
    ImGui::EndDisabled();

    property::separator();
    int tick = key.tick;
    if (ImGui::InputInt(
            "playback.refactorEditor.details.poseTick"_tr().c_str(),
            &tick,
            1,
            20,
            ImGuiInputTextFlags_EnterReturnsTrue
        )) {
        commit([&](PoseKey& edited) { edited.tick = std::clamp(tick, 0, ctx.state.totalTicks); });
    }
    if (property::actionButton("playback.refactorEditor.details.poseMoveToPlayhead"_tr().c_str())) {
        commit([&](PoseKey& edited) { edited.tick = ctx.state.currentTick; });
    }
    if (property::actionButton("playback.refactorEditor.details.poseDelete"_tr().c_str())) {
        auto edited = keys;
        std::erase_if(edited, [&](PoseKey const& entry) { return entry.id == key.id; });
        EditorAction action{EditorActionType::SetPoseKeys};
        action.poseKeys = std::move(edited);
        ctx.submitAction(std::move(action));
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "playback.refactorEditor.details.poseHint"_tr().c_str());
    ImGui::PopTextWrapPos();
    property::endSection();
    return true;
}

void DetailsPanel::drawFollowSection(PanelContext const& ctx, state::editing::model::CameraEntity const& camera) {
    using editing::model::FollowSettings;
    using editing::model::FollowStyle;
    if (!property::beginSection("playback.refactorEditor.details.follow"_tr().c_str())) return;

    bool const     draft  = mFollowDraft && mFollowDraft->first == camera.id;
    FollowSettings follow = draft ? mFollowDraft->second : camera.follow;
    auto const     commit = [&](FollowSettings const& edited) {
        mFollowDraft.reset();
        EditorAction action{EditorActionType::SetCameraFollow};
        action.id             = camera.id;
        action.followSettings = edited;
        ctx.submitAction(std::move(action));
    };
    // A slider keeps its value here while dragged and becomes one undo step once no control is active any more.
    auto const slider =
        [&](std::string const& label, char const* id, float& value, float low, float high, char const* format) {
            ImGui::TextDisabled("%s", label.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat(id, &value, low, high, format, ImGuiSliderFlags_AlwaysClamp)) {
                mFollowDraft = {camera.id, follow};
            }
        };

    ImGui::BeginDisabled(camera.locked);
    std::array const styles{
        "playback.refactorEditor.details.followStyleBehind"_tr(),
        "playback.refactorEditor.details.followStyleShoulder"_tr(),
        "playback.refactorEditor.details.followStyleOrbit"_tr(),
        "playback.refactorEditor.details.followStyleTripod"_tr(),
    };
    auto const current = static_cast<std::size_t>(follow.style);
    ImGui::TextDisabled("%s", "playback.refactorEditor.details.followStyle"_tr().c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (widgets::beginCombo("##follow-style", styles[current].c_str())) {
        for (std::size_t index = 0; index < styles.size(); ++index) {
            if (ImGui::Selectable(styles[index].c_str(), index == current)) {
                follow.style = static_cast<FollowStyle>(index);
                commit(follow);
            }
        }
        widgets::endCombo();
    }

    if (follow.style != FollowStyle::Tripod) {
        slider(
            "playback.refactorEditor.details.followDistance"_tr(),
            "##follow-distance",
            follow.distance,
            0.0f,
            FollowSettings::MaxDistance,
            "%.1f"
        );
        slider(
            "playback.refactorEditor.details.followHeight"_tr(),
            "##follow-height",
            follow.height,
            -FollowSettings::MaxHeight,
            FollowSettings::MaxHeight,
            "%.1f"
        );
    }
    if (follow.style == FollowStyle::Shoulder) {
        slider(
            "playback.refactorEditor.details.followSide"_tr(),
            "##follow-side",
            follow.side,
            -FollowSettings::MaxSide,
            FollowSettings::MaxSide,
            "%.2f"
        );
        widgets::itemTooltip("playback.refactorEditor.details.followSideHint"_tr().c_str());
    }
    if (follow.style == FollowStyle::Orbit) {
        slider(
            "playback.refactorEditor.details.followOrbitSpeed"_tr(),
            "##follow-orbit-speed",
            follow.orbitSpeed,
            -FollowSettings::MaxOrbitSpeed,
            FollowSettings::MaxOrbitSpeed,
            "%.0f°/s"
        );
        slider(
            "playback.refactorEditor.details.followOrbitStart"_tr(),
            "##follow-orbit-start",
            follow.orbitStart,
            -180.0f,
            180.0f,
            "%.0f°"
        );
        widgets::itemTooltip("playback.refactorEditor.details.followOrbitHint"_tr().c_str());
    }
    if (follow.style == FollowStyle::Tripod) {
        std::string const where = follow.hasTripod ? "playback.refactorEditor.details.followTripodAt"_tr(
                                                         static_cast<int>(std::floor(follow.tripod.x)),
                                                         static_cast<int>(std::floor(follow.tripod.y)),
                                                         static_cast<int>(std::floor(follow.tripod.z))
                                                     )
                                                   : "playback.refactorEditor.details.followTripodUnset"_tr();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", where.c_str());
        ImGui::PopTextWrapPos();
        if (property::actionButton("playback.refactorEditor.details.followTripodSet"_tr().c_str())) {
            if (auto const view = keyframe::captureClientCamera()) {
                follow.tripod    = {view->x, view->y, view->z};
                follow.hasTripod = true;
                commit(follow);
            }
        }
        widgets::itemTooltip("playback.refactorEditor.details.followTripodHint"_tr().c_str());
    }
    slider(
        "playback.refactorEditor.details.followSmoothing"_tr(),
        "##follow-smoothing",
        follow.smoothing,
        0.0f,
        FollowSettings::MaxSmoothing,
        "%.2f s"
    );
    widgets::itemTooltip("playback.refactorEditor.details.followSmoothingHint"_tr().c_str());
    ImGui::EndDisabled();

    if (mFollowDraft && mFollowDraft->first == camera.id && !ImGui::IsAnyItemActive()) commit(mFollowDraft->second);
    property::endSection();
}

void DetailsPanel::drawShakeSection(PanelContext const& ctx, state::editing::model::CameraEntity const& camera) {
    using editing::model::ShakeSettings;
    if (!property::beginSection("playback.refactorEditor.details.shake"_tr().c_str())) return;

    bool const    draft  = mShakeDraft && mShakeDraft->first == camera.id;
    ShakeSettings shake  = draft ? mShakeDraft->second : camera.shake;
    auto const    commit = [&](ShakeSettings const& edited) {
        mShakeDraft.reset();
        EditorAction action{EditorActionType::SetCameraShake};
        action.id            = camera.id;
        action.shakeSettings = edited;
        ctx.submitAction(std::move(action));
    };
    // A slider keeps its value here while dragged and becomes one undo step once no control is active any more.
    auto const slider =
        [&](std::string const& label, char const* id, float& value, float high, char const* format) {
            ImGui::TextDisabled("%s", label.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat(id, &value, 0.0f, high, format, ImGuiSliderFlags_AlwaysClamp)) {
                mShakeDraft = {camera.id, shake};
            }
        };

    ImGui::BeginDisabled(camera.locked);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "playback.refactorEditor.details.shakeHint"_tr().c_str());
    ImGui::PopTextWrapPos();
    slider(
        "playback.refactorEditor.details.shakePosition"_tr(),
        "##shake-position",
        shake.positionAmount,
        ShakeSettings::MaxPositionAmount,
        "%.2f"
    );
    slider(
        "playback.refactorEditor.details.shakeRotation"_tr(),
        "##shake-rotation",
        shake.rotationAmount,
        ShakeSettings::MaxRotationAmount,
        "%.1f°"
    );
    slider(
        "playback.refactorEditor.details.shakeRoll"_tr(),
        "##shake-roll",
        shake.rollAmount,
        ShakeSettings::MaxRotationAmount,
        "%.1f°"
    );

    ImGui::TextDisabled("%s", "playback.refactorEditor.details.shakeSeed"_tr().c_str());
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputScalar("##shake-seed", ImGuiDataType_U32, &shake.seed);
    if (ImGui::IsItemActive()) mShakeDraft = {camera.id, shake};
    widgets::itemTooltip("playback.refactorEditor.details.shakeSeedHint"_tr().c_str());
    if (property::actionButton("playback.refactorEditor.details.shakeNewSeed"_tr().c_str())) {
        shake.seed = editing::model::randomShakeSeed();
        commit(shake);
    }
    ImGui::EndDisabled();

    if (mShakeDraft && mShakeDraft->first == camera.id && !ImGui::IsAnyItemActive()) commit(mShakeDraft->second);
    property::endSection();
}

void DetailsPanel::drawOptiPistonSection() {
    // Absent OptiPiston means no section at all, not a disabled one.
    auto const piston = integration::optiPistonSettings();
    if (!piston) {
        mPistonDurationDraft.reset();
        return;
    }
    if (!property::beginSection("playback.refactorEditor.inspector.optiPiston"_tr().c_str())) return;

    bool enabled = piston->enabled;
    if (ImGui::Checkbox(("playback.refactorEditor.inspector.pistonEnabled"_tr() + "##set-piston").c_str(), &enabled))
        integration::setOptiPistonEnabled(enabled);

    ImGui::BeginDisabled(!piston->enabled);
    ImGui::TextUnformatted("playback.refactorEditor.inspector.pistonDuration"_tr().c_str());
    // Held locally while dragging so the config is written once, on release.
    float duration = mPistonDurationDraft.value_or(piston->durationTicks);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat(
        "##set-piston-duration",
        &duration,
        piston->minDurationTicks,
        piston->maxDurationTicks,
        "%.2f gt",
        ImGuiSliderFlags_AlwaysClamp
    );
    if (ImGui::IsItemActive()) mPistonDurationDraft = duration;
    if (ImGui::IsItemDeactivatedAfterEdit()) integration::setOptiPistonDuration(duration);
    if (!ImGui::IsItemActive()) mPistonDurationDraft.reset();
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", "playback.refactorEditor.inspector.optiPistonHint"_tr().c_str());
    property::endSection();
}

void DetailsPanel::drawShaderSection(PanelContext const& ctx) {
    if (!ctx.state.project) return;
    auto const& packName = ctx.commands.renderShaderPack();
    auto const* pack     = ctx.commands.renderShaderPackInfo();
    if (packName.empty() || !pack) return;
    if (!property::beginSection("playback.refactorEditor.inspector.shader"_tr().c_str())) return;

    ImGui::TextDisabled("%s", packName.c_str());
    auto const&                               world   = ctx.state.project->world;
    auto const                                project = world.shaderOptions.find(packName);
    static std::map<std::string, std::string> const none;
    auto const& projectValues = project != world.shaderOptions.end() ? project->second : none;
    if (!projectValues.empty()
        && property::actionButton("playback.refactorEditor.inspector.shaderReset"_tr().c_str())) {
        auto edited = world;
        edited.shaderOptions.erase(packName);
        EditorAction action{EditorActionType::SetWorldSettings};
        action.worldSettings = std::move(edited);
        ctx.submitAction(std::move(action));
    }
    drawShaderScreen(ctx, *pack, "", projectValues, 0);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "playback.refactorEditor.inspector.shaderHint"_tr().c_str());
    ImGui::PopTextWrapPos();
    property::endSection();
}

void DetailsPanel::drawShaderScreen(
    PanelContext const&                       ctx,
    integration::ShaderPackInfo const&        pack,
    std::string const&                        screen,
    std::map<std::string, std::string> const& projectValues,
    int                                       depth
) {
    auto const entries = pack.screens.find(screen);
    // A pack's menu nests a few levels at most; this stops a screen that lists itself.
    if (entries == pack.screens.end() || depth > 8) return;
    auto const& packName = ctx.commands.renderShaderPack();
    auto const& user     = ctx.commands.userShaderOptions();
    // Playback's value for one option; nullopt hands it back to the user's own.
    auto const set = [&](std::string const& name, std::optional<std::string> value) {
        auto  edited = ctx.state.project->world;
        auto& values = edited.shaderOptions[packName];
        if (value) values.insert_or_assign(name, std::move(*value));
        else values.erase(name);
        if (values.empty()) edited.shaderOptions.erase(packName);
        EditorAction action{EditorActionType::SetWorldSettings};
        action.worldSettings = std::move(edited);
        ctx.submitAction(std::move(action));
    };

    for (auto const& entry : entries->second) {
        if (entry.size() > 2 && entry.front() == '[' && entry.back() == ']') {
            auto const sub = entry.substr(1, entry.size() - 2);
            if (ImGui::TreeNode((pack.text("screen." + sub, sub) + "##shader-screen-" + sub).c_str())) {
                drawShaderScreen(ctx, pack, sub, projectValues, depth + 1);
                ImGui::TreePop();
            }
            continue;
        }
        auto const* option = pack.option(entry);
        if (!option) continue;

        auto const projectValue = projectValues.find(entry);
        auto const userValue    = user.find(entry);
        bool const ours         = projectValue != projectValues.end();
        auto const value        = ours                     ? projectValue->second
                                : userValue != user.end() ? userValue->second
                                                          : option->defaultValue;
        // Options the project sets are marked; right-clicking one hands it back to the user's own value.
        auto const label   = (ours ? "* " : "") + pack.text("option." + entry, entry);
        auto const comment = pack.text("option." + entry + ".comment", "");
        ImGui::PushID(entry.c_str());
        if (option->toggle) {
            bool on = value == "true";
            if (ImGui::Checkbox(label.c_str(), &on)) set(entry, on ? "true" : "false");
        } else {
            ImGui::TextDisabled("%s", label.c_str());
            auto const current = std::ranges::find(option->values, value);
            int index = current != option->values.end() ? static_cast<int>(current - option->values.begin()) : 0;
            if (mShaderSliderDraft && mShaderSliderDraft->first == entry) index = mShaderSliderDraft->second;
            auto const shown = pack.text("value." + entry + "." + option->values[index], option->values[index]);
            ImGui::SetNextItemWidth(-1.0f);
            if (option->slider) {
                // ImGui reads the format as printf; the value's own text is shown as it is.
                std::string format;
                for (char ch : shown) format += ch == '%' ? std::string("%%") : std::string(1, ch);
                int const last = static_cast<int>(option->values.size()) - 1;
                if (ImGui::SliderInt("##value", &index, 0, last, format.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
                    mShaderSliderDraft = {entry, index};
                }
                if (ImGui::IsItemDeactivated()) {
                    if (mShaderSliderDraft && mShaderSliderDraft->first == entry
                        && option->values[mShaderSliderDraft->second] != value) {
                        set(entry, option->values[mShaderSliderDraft->second]);
                    }
                    mShaderSliderDraft.reset();
                }
            } else if (widgets::beginCombo("##value", shown.c_str())) {
                for (auto const& candidate : option->values) {
                    auto const text = pack.text("value." + entry + "." + candidate, candidate);
                    if (ImGui::Selectable(text.c_str(), candidate == value) && candidate != value) {
                        set(entry, candidate);
                    }
                }
                widgets::endCombo();
            }
        }
        if (!comment.empty()) widgets::itemTooltip(comment.c_str());
        if (ours && ImGui::BeginPopupContextItem("##reset")) {
            if (ImGui::MenuItem("playback.refactorEditor.inspector.shaderResetOption"_tr().c_str())) {
                set(entry, std::nullopt);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

} // namespace playback::editor::ui
