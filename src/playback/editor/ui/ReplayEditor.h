#pragma once

#include "playback/editor/graphics/ReplayUILayout.h"
#include "playback/editor/ui/PanelContext.h"
#include "playback/state/EditorAction.h"
#include "playback/state/EditorState.h"
#include "playback/state/editing/models/SelectionModel.h"

#include "EditorTheme.h"
#include "modes/EditMode.h"
#include "modes/RenderMode.h"
#include "panels/DetailsPanel.h"
#include "panels/StatusPanel.h"
#include "panels/TimelinePanel.h"
#include "panels/ViewportPanel.h"
#include "playback/editor/ui/components/Splitter.h"
#include "playback/editor/ui/menus/EditorMenuBar.h"
#include "playback/editor/ui/modes/ModeManager.h"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playback::editor::ui {

class ReplayEditor : public EditorCommands {
public:
    static ReplayEditor& getInstance();

    void initialize();
    void shutdown();

    void draw(playback::state::EditorState const& state, SubmitAction const& submit);

    void handleKeyboardShortcuts();
    void updateFollowExitTap();

    [[nodiscard]] playback::state::EditorState const&          state() const;
    [[nodiscard]] state::editing::model::SelectionModel const& selection() const { return mSelection; }
    state::editing::model::SelectionModel&                     selection() { return mSelection; }
    void                                                       submitAction(playback::state::EditorAction action) const;

    void               seekTo(int tick) override;
    void               seekRelative(int tickDelta) override;
    void               toggleViewportMaximized() override { mViewportMaximized = !mViewportMaximized; }
    [[nodiscard]] bool isViewportMaximized() const override { return mViewportMaximized; }
    void               toggleCameraPath() override;
    [[nodiscard]] bool isCameraPathVisible() const override { return mCameraPathVisible; }
    bool               deleteSelection() override;
    bool               addKeyframeAtPlayhead() override;
    bool               selectAllKeyframes() override;
    bool               copySelection() override;
    bool               pasteAtPlayhead() override;
    bool               addMarkerAtPlayhead() override;
    bool               selectCameraByIndex(std::size_t index);
    // Any thread: selects this keyframe at the start of the next frame, as clicking it would. Used by the control API.
    void requestKeyframeSelection(std::string trackId, int tick);

    [[nodiscard]] bool  isInfoOverlayVisible() const override { return mViewportPanel.isInfoOverlayVisible(); }
    void                setInfoOverlayVisible(bool visible) override { mViewportPanel.setInfoOverlayVisible(visible); }
    [[nodiscard]] bool  isAutoPreviewEnabled() const override { return mViewportPanel.isAutoPreviewEnabled(); }
    void                setAutoPreviewEnabled(bool enabled) override { mViewportPanel.setAutoPreviewEnabled(enabled); }
    [[nodiscard]] bool  isSnapEnabled() const override { return mTimelinePanel.isSnapEnabled(); }
    void                setSnapEnabled(bool enabled) override { mTimelinePanel.setSnapEnabled(enabled); }
    [[nodiscard]] float videoAspectRatio() const override { return mVideoAspectRatio; }
    void                setVideoAspectRatio(float ratio) override;
    void                openExportDialog() override;
    [[nodiscard]] int   exportStartTick() const override { return mMenuBar.exportStartTick(); }
    [[nodiscard]] int   exportEndTick() const override { return mMenuBar.exportEndTick(); }
    void                setExportStartTick(int tick) override { mMenuBar.setExportStartTick(tick); }
    void                setExportEndTick(int tick) override { mMenuBar.setExportEndTick(tick); }
    [[nodiscard]] UiScaleTier uiScaleTier() const override { return mUiScaleTier; }
    void                      setUiScaleTier(UiScaleTier tier) override;

    [[nodiscard]] bool  isHudElementShown(visuals::HudElement element) const override;
    void                setHudElementShown(visuals::HudElement element, bool shown) override;
    [[nodiscard]] bool  isPreviewAudioMuted() const override;
    void                setPreviewAudioMuted(bool muted) override;
    [[nodiscard]] float previewAudioVolume() const override;
    void                setPreviewAudioVolume(float volume, bool commit) override;
    [[nodiscard]] bool  isPreviewSilentWhilePaused() const override;
    void                setPreviewSilentWhilePaused(bool silent) override;

    [[nodiscard]] ViewShading                     viewShading() const override { return mViewShading; }
    void                                          setViewShading(ViewShading shading) override;
    [[nodiscard]] std::string const&              renderShaderPack() const override { return mRenderShaderPack; }
    void                                          setRenderShaderPack(std::string pack) override;
    [[nodiscard]] std::vector<std::string> const& shaderPacks() const override { return mShaderPacks; }

    void                      setGameTexture(ImTextureID texture) { mViewportPanel.setGameTexture(texture); }
    [[nodiscard]] ImTextureID gameTexture() const { return mViewportPanel.gameTexture(); }
    [[nodiscard]] Rect        viewportVideoRect() const { return mViewportPanel.videoRect(); }
    [[nodiscard]] Rect        viewportOverlayRect() const { return mViewportPanel.overlayRect(); }

private:
    ReplayEditor() = default;

    [[nodiscard]] PanelContext frameContext();

    bool mViewportMaximized{false};
    bool mCameraPathVisible{true};

    ModeManager&  mModeManager{ModeManager::getInstance()};
    EditorMenuBar mMenuBar;
    Splitter      mSplitter;

    ViewportPanel mViewportPanel;
    DetailsPanel  mDetailsPanel;
    TimelinePanel mTimelinePanel;
    StatusPanel   mStatusPanel;

    EditMode   mEditMode;
    RenderMode mRenderMode;

    playback::state::EditorState const*   mFrameState{};
    SubmitAction const*                   mSubmit{};
    state::editing::model::SelectionModel mSelection;
    std::mutex                                             mSelectionRequestMutex;
    std::optional<state::editing::model::SelectedKeyframe> mSelectionRequest;
    exporting::ExportState                mLastExportState{exporting::ExportState::Idle};

    bool  mFollowExitTapArmed{false};
    float mExitHoldSeconds{};

    float       mDetailsWidthRatio{0.28f};
    float       mTimelineHeightRatio{0.35f};
    float       mVideoAspectRatio{16.0f / 9.0f};
    UiScaleTier mUiScaleTier{UiScaleTier::Auto};

    ViewShading              mViewShading{ViewShading::Rendered};
    std::string              mRenderShaderPack;
    std::vector<std::string> mShaderPacks;
    // Whether the pack was ever picked (else Arke's own pack is taken when a replay opens), and whether this replay
    // has listed the packs and put the view in place; the latter resets when the replay closes.
    bool mRenderShaderPackChosen{};
    bool mViewShadingLive{};

    struct TimelineViewPreferences {
        float zoomScale{1.0f};
        float horizontalScroll{};
    };

    std::map<std::string, TimelineViewPreferences> mTimelineViewPreferences;
    std::string                                    mActiveReplayPath;

    void loadLayoutPreferences();
    void saveLayoutPreferences() const;
    void syncTimelineViewPreferences(std::string_view replayPath);
    void updateExitHold();
    // Puts the viewport's shading in place; while the export dialog is open or an export runs, the viewport shows the
    // export's look (project graphics, Rendered pack).
    void syncViewShading(bool inReplay, bool exportLook);

    friend class EditMode;
    friend class RenderMode;
};

} // namespace playback::editor::ui
