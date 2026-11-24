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

enum class InspectorPage { Selection, StandIn, Export, Preview, Settings };

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
    // Hire a Stand-in: the player and the In/Out of the recording to take (marked on the Replay lane), then Hire.
    void drawStandInPage(PanelContext const& ctx);
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
    // A camera track's FOV and lens, and a keyframe track's shake amounts and seed (ShakeSettings).
    void drawLensSection(PanelContext const& ctx, state::editing::model::CameraEntity const& camera);

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
    // The page the rail showed last frame, so entering and leaving the Stand-in page can switch its marks on and off.
    InspectorPage        mShownPage{InspectorPage::Selection};
    // A hire on its way: the new layer's id and the hire serial before it; the last failure's reason.
    std::string          mHireId;
    int                  mHireSerial{};
    std::string          mHireError;
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
