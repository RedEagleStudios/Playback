#pragma once

#include "playback/editor/ui/PanelContext.h"
#include "playback/editor/ui/components/Animator.h"
#include "playback/editor/ui/components/TimelineScale.h"
#include "playback/editor/ui/components/TrackTreeModel.h"
#include "playback/editor/ui/components/VanillaPicker.h"
#include "playback/state/EditorAction.h"

#include "imgui.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playback::editor::ui {

class TimelinePanel {
public:
    void draw(PanelContext const& ctx, bool allowInput);

    void seekTo(PanelContext const& ctx, int tick);
    void seekRelative(PanelContext const& ctx, int tickDelta);
    void seekAdjacentEditPoint(PanelContext const& ctx, bool forward);
    bool addKeyframeAtPlayhead(PanelContext const& ctx);
    bool deleteSelection(PanelContext const& ctx);
    bool selectAllKeyframes(PanelContext const& ctx);
    // Copies the selected keyframes; paste keeps their spacing and lands the earliest one on the playhead.
    bool copySelection(PanelContext const& ctx);
    bool pasteAtPlayhead(PanelContext const& ctx);
    bool addMarkerAtPlayhead(PanelContext const& ctx);
    void seekAdjacentMarker(PanelContext const& ctx, bool forward);
    // A pose keyframe for the entity (a player UUID or "entity:" choice) at tick, selected once it exists. An empty
    // animation hands the entity back to its recorded animation.
    void addPoseKeyAt(
        PanelContext const& ctx,
        std::string const&  subject,
        std::string const&  label,
        int                 tick,
        std::string const&  animation
    );
    void zoomIn();
    void zoomOut();
    void resetZoom();

    [[nodiscard]] float trackListWidthRatio() const { return mTrackListWidthRatio; }
    [[nodiscard]] float zoomScale() const { return mZoomScale; }
    [[nodiscard]] float horizontalScroll() const { return mScrollX; }
    void                setViewPreferences(float trackListWidthRatio, float zoomScale, float horizontalScroll);

    [[nodiscard]] bool isSnapEnabled() const { return mSnapEnabled; }
    void               setSnapEnabled(bool enabled) { mSnapEnabled = enabled; }
    void               toggleSnap() { mSnapEnabled = !mSnapEnabled; }

private:
    struct Layout {
        ImVec2 fullMin{};
        ImVec2 fullMax{};
        float  titleBottom{};
        float  rulerTop{};
        float  headerHeight{};
        float  bodyTop{};
        float  bodyBottom{};
        float  listWidth{};
        float  canvasLeft{};
        float  canvasWidth{};
    };

    struct ClipboardKey {
        std::string                           cameraId;
        int                                   offset{};
        state::editing::model::CameraKeyframe pose;
    };

    void                      submitSeek(PanelContext const& ctx, int tick);
    [[nodiscard]] std::string resolveKeyframeTargetCamera(PanelContext const& ctx) const;
    // Single-camera clips go to `targetCameraId` when given; multi-camera clips return to their own tracks.
    bool pasteKeyframes(PanelContext const& ctx, int anchorTick, std::string const& targetCameraId);
    // Shift-drag snapping onto keyframes, markers, the playhead, in/out and segment edges within a few pixels.
    // The thing being dragged is excluded so it does not stick to its own position.
    int  snapToEditPoints(
        PanelContext const&                                 ctx,
        int                                                 tick,
        float                                               pixelsPerTick,
        bool                                                includePlayhead,
        bool                                                includeExportRange,
        std::function<bool(std::string const&, int)> const& skipKey,
        std::string_view                                    skipMarkerId
    );
    bool addMarkerAt(PanelContext const& ctx, int tick);
    // Selections that point at objects an queued action has not created yet; applied once they exist.
    void queueKeyframeSelection(std::vector<state::editing::model::SelectedKeyframe> keys);
    void queueMarkerSelection(std::string markerId);
    void queuePoseKeySelection(std::string poseKeyId);
    void queueOverlaySelection(std::string overlayId);
    // A title, shape or model layer from the playhead, selected once it exists. A model asks for its file first.
    // A model layer with `asset` set is that Minecraft block or item; without it, it asks for a .mimodel file.
    void addLayer(
        PanelContext const&                ctx,
        state::editing::model::OverlayKind kind,
        state::editing::model::ShapeKind   shape  = state::editing::model::ShapeKind::Rectangle,
        int                                points = 3,
        state::editing::model::BlurStyle   blur   = state::editing::model::BlurStyle::Gaussian,
        state::editing::model::LightStyle  light  = state::editing::model::LightStyle::Point,
        std::string const&                 asset  = {}
    );
    void applyPendingSelection(PanelContext const& ctx);
    [[nodiscard]] std::string markerAt(
        PanelContext const&  ctx,
        Layout const&        layout,
        TimelineScale const& scale,
        ImVec2               mouse
    ) const;
    void drawMarkers(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale);

    void drawTitleRow(PanelContext const& ctx, Layout const& layout, int displayTick, bool allowInput);
    void drawTrackHeaders(PanelContext const& ctx, Layout const& layout, bool allowInput);
    void drawRuler(
        PanelContext const&  ctx,
        Layout const&        layout,
        TimelineScale const& scale,
        int                  displayTick
    ); // Drawn after the track rows so the row bands do not cover it.
    void drawTrackGrid(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale);
    // In/out markers for the export range, drawn over the canvas and draggable by their handles.
    void drawExportRange(PanelContext const& ctx, Layout const& layout, TimelineScale const& scale, bool allowInput);

    void drawContextMenus(PanelContext const& ctx);
    void drawLayerMenus(PanelContext const& ctx);
    // A Property row's lane: its value as a curve over the timeline and its keys, which drag, and right-click for
    // easing or delete.
    void drawPropertyLane(
        PanelContext const&  ctx,
        TrackTreeRow const&  row,
        Layout const&        layout,
        TimelineScale const& scale,
        float                top,
        float                bottom,
        bool                 allowInput,
        bool&                clickConsumed
    );
    void drawChannelKeyMenu(PanelContext const& ctx);
    // Edits the keyed channels of a camera, layer or the World as one undo step.
    void editChannels(
        PanelContext const&                                                ctx,
        PropertyOwner                                                      owner,
        std::string const&                                                 ownerId,
        std::function<void(state::editing::model::ValueChannels&)> const& edit
    );

    void drawRangeBar(PanelContext const& ctx, Layout const& layout, bool allowInput);
    void drawTitleLabel(PanelContext const& ctx, float height);
    void drawTransportGroup(PanelContext const& ctx, int displayTick, float width);

    TrackTreeModel mTrackTree;
    VanillaPicker  mVanillaPicker;
    Animator       mAnimator;
    float          mZoomScale{1.0f};
    float          mScrollX{};
    float          mScrollY{};
    float          mTrackListWidthRatio{0.30f};
    int            mPendingSeekTick{-1};
    int            mRulerDragTick{-1};
    std::string    mTrackSearch;
    bool           mSnapEnabled{true};
    bool           mDraggingPlayhead{};
    // Rows shown with their property rows, by row id.
    std::set<std::string> mExpandedRows;
    // Rows that had keyed properties last frame; one that newly has them is expanded.
    std::set<std::string> mExpandableRows;
    // A property key being dragged (its row id and tick, in the channel's own ticks) and its right-click menu.
    std::string mChannelDragRow;
    int         mChannelDragKey{};
    int         mChannelDragTick{};
    int         mChannelDragOffset{};
    float       mChannelDragStartX{};
    bool        mChannelDragMoved{};
    std::string mChannelMenuRow;
    int         mChannelMenuKey{};
    // The Stand-in page's mark being dragged on the Replay lane: 0 none, 1 In, 2 Out.
    int mStandInDrag{};
    // 0 none, 1 in marker, 2 out marker.
    int         mExportMarkerDrag{};
    std::string mDraggingKeyframeCameraId;
    float       mDraggingKeyframeStartMouseX{};
    int         mDraggingKeyframeStartTick{};
    int         mDraggingKeyframeTick{};
    bool        mDraggingKeyframeMoved{};
    // Range-bar drag: 0 none, 1 left handle, 2 right handle, 3 whole window.
    int   mRangeDragMode{};
    float mRangeDragOriginX{};
    float mRangeDragOriginScroll{};
    float mRangeDragOriginZoom{1.0f};
    // Right-click menus. The tick is captured when the menu opens so it does not follow the cursor.
    std::string                                           mKeyframeMenuCameraId;
    int                                                   mKeyframeMenuTick{-1};
    int                                                   mKeyframeMenuTickEdit{-1};
    bool                                                  mKeyframeMenuGroup{};
    std::vector<ClipboardKey>                             mKeyframeClipboard;
    std::string                                           mLaneMenuCameraId;
    int                                                   mLaneMenuTick{-1};
    std::string                                           mTrackMenuCameraId;
    int                                                   mRulerMenuTick{-1};
    // Inline rename field of the track menu; refilled whenever the menu targets another camera.
    std::string                                           mRenameCameraId;
    std::array<char, 128>                                 mRenameBuffer{};

    // Grabbing a key that is part of a multi-selection moves the whole set by one delta.
    bool mGroupDrag{};
    int  mGroupDragDelta{};
    // A press on empty canvas becomes a box select once it travels a few pixels; otherwise it is a click.
    bool        mBoxPending{};
    bool        mBoxSelecting{};
    bool        mBoxAdditive{};
    ImVec2      mBoxStart{};
    std::string mBoxClickCameraId;
    // Marker pins live in the ruler band: drag to move, right-click for the menu.
    std::string           mMarkerDragId;
    float                 mMarkerDragStartX{};
    int                   mMarkerDragTick{-1};
    bool                  mMarkerDragMoved{};
    std::string           mMarkerMenuId;
    std::string           mMarkerRenameId;
    std::array<char, 128> mMarkerRenameBuffer{};
    // Pose keyframes sit on one lane per entity: drag to move, right-click for the menu.
    std::string mPoseDragId;
    float       mPoseDragStartX{};
    int         mPoseDragTick{-1};
    bool        mPoseDragMoved{};
    std::string mPoseMenuKeyId;
    std::string mPoseLaneMenuSubject;
    std::string mPoseLaneMenuLabel;
    int         mPoseLaneMenuTick{-1};
    // Layer bars and replay segments: the body moves a layer (mode 0), an edge trims either (1 start, 2 end); -1 only
    // seeks on release.
    std::string mClipDragId;
    bool        mClipDragReplay{};
    int         mClipDragMode{};
    float       mClipDragStartX{};
    int         mClipDragOriginStart{};
    int         mClipDragOriginEnd{};
    int         mClipDragStart{};
    int         mClipDragEnd{};
    bool        mClipDragMoved{};
    std::string mOverlayMenuId;
    std::string mSegmentMenuId;
    int         mSegmentMenuTick{-1};
    int                   mSnapGuideTick{-1};

    std::vector<state::editing::model::SelectedKeyframe> mPendingKeySelection;
    std::string                                          mPendingMarkerSelection;
    std::string                                          mPendingPoseKeySelection;
    std::string                                          mPendingOverlaySelection;
    int                                                  mPendingSelectionFrames{};
};

} // namespace playback::editor::ui
