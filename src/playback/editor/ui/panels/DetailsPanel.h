#pragma once

#include "playback/editor/ui/PanelContext.h"
#include "playback/editor/ui/panels/LayerInspector.h"
#include "playback/state/editing/models/CameraEntity.h"
#include "playback/state/editing/models/WorldSettings.h"

#include <array>
#include <map>
#include <optional>
#include <string>

namespace playback::editor::ui {

enum class InspectorPage { Selection, Export, Preview, Settings };

class DetailsPanel {
public:
    void draw(PanelContext const& ctx);

    [[nodiscard]] InspectorPage page() const { return mPage; }
    void                        setPage(InspectorPage page) { mPage = page; }

private:
    void drawRail(PanelContext const& ctx, float width, float height);
    void drawSelectionPage(PanelContext const& ctx);
    void drawExportPage(PanelContext const& ctx);
    void drawPreviewPage(PanelContext const& ctx);
    void drawSettingsPage(PanelContext const& ctx);
    void drawOptiPistonSection();
    void drawWorldSection(PanelContext const& ctx);
    // The Rendered pack's own options, laid out by its screens; values are saved in the project.
    void drawShaderSection(PanelContext const& ctx);
    void drawShaderScreen(
        PanelContext const&                       ctx,
        integration::ShaderPackInfo const&        pack,
        std::string const&                        screen,
        std::map<std::string, std::string> const& projectValues,
        int                                       depth
    );
    // Create template / Select template: the pack's values saved as `<name>.txt` and applied back to the project.
    void drawShaderTemplates(
        PanelContext const&                       ctx,
        integration::ShaderPackInfo const&        pack,
        std::string const&                        packName,
        std::map<std::string, std::string> const& projectValues
    );
    // A third-person track's framing (FollowSettings).
    void drawFollowSection(PanelContext const& ctx, state::editing::model::CameraEntity const& camera);
    // A keyframe track's shake amounts and seed (ShakeSettings).
    void drawShakeSection(PanelContext const& ctx, state::editing::model::CameraEntity const& camera);

    // Each returns true when it owns the current selection and has drawn it.
    bool drawWorldActor(PanelContext const& ctx);
    bool drawWorldActorSegment(PanelContext const& ctx);
    bool drawSubActor(PanelContext const& ctx);
    bool drawCamera(PanelContext const& ctx);
    bool drawKeyframes(PanelContext const& ctx);
    bool drawKeyframe(PanelContext const& ctx);
    bool drawMarker(PanelContext const& ctx);
    // A pose keyframe: its entity, pose, empty hands and tick.
    bool drawPoseKey(PanelContext const& ctx);
    void drawOverview(PanelContext const& ctx);

    InspectorPage        mPage{InspectorPage::Selection};
    LayerInspector       mLayerInspector;
    std::optional<float> mPistonDurationDraft;
    // The World section's settings while a slider or colour is dragged; committed as one undo step on release.
    std::optional<state::editing::model::WorldSettings> mWorldDraft;
    // The selected third-person track's follow settings while a slider is dragged, by track id.
    std::optional<std::pair<std::string, state::editing::model::FollowSettings>> mFollowDraft;
    // The selected keyframe track's FOV and shake properties while a field is held, by track id.
    std::optional<std::pair<std::string, state::editing::model::CameraProperties>> mCameraDraft;
    // A shader option slider's value index while it is dragged, by option name; the pack reloads on release.
    std::optional<std::pair<std::string, int>> mShaderSliderDraft;
    // The Create template dialog's name field.
    std::array<char, 128> mShaderTemplateName{};
    // The label field edits a local copy so typing is not overwritten by the project each frame.
    std::string           mMarkerLabelId;
    std::array<char, 128> mMarkerLabelBuffer{};
    bool                  mMarkerLabelEditing{};
};

} // namespace playback::editor::ui
