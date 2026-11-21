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
    void               addPoseKeyAtPlayhead(std::string const& subject, std::string const& label) override;
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
    void                exportStillFrame() override;
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
    [[nodiscard]] integration::ShaderPackInfo const* renderShaderPackInfo() const override {
        return mShaderPackInfo ? &*mShaderPackInfo : nullptr;
    }
    [[nodiscard]] std::map<std::string, std::string> const& userShaderOptions() const override {
        return mUserShaderOptions;
    }

    void                      setGameTexture(ImTextureID texture) { mViewportPanel.setGameTexture(texture); }
    [[nodiscard]] ImTextureID gameTexture() const { return mViewportPanel.gameTexture(); }
    [[nodiscard]] Rect        viewportVideoRect() const { return mViewportPanel.videoRect(); }
    [[nodiscard]] Rect        viewportOverlayRect() const { return mViewportPanel.overlayRect(); }

    // Every Present, whether or not the editor draws: puts the viewport's shading in place while a replay is open, and
    // gives Arke the user's own setting back once it closes. While the export dialog is open or an export runs, the
    // viewport shows the export's look (project graphics, Rendered pack). Render thread.
    void syncViewShading(playback::state::EditorState const& state);

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
    // Playback's Flat pack as installed in Arke when this replay opened; empty without Arke, where Flat falls back to
    // Simple graphics at full brightness.
    std::string mFlatShaderPack;
    // Whether the pack was ever picked (else Arke's own pack is taken when a replay opens), and whether this replay
    // has listed the packs and put the view in place; the latter resets when the replay closes.
    bool mRenderShaderPackChosen{};
    bool mViewShadingLive{};
    // The Rendered pack's options, read when the pack or the replay changes (mShaderPackInfoFor names the pack read),
    // and the user's own values for them.
    std::optional<integration::ShaderPackInfo> mShaderPackInfo;
    std::optional<std::string>                 mShaderPackInfoFor;
    std::map<std::string, std::string>         mUserShaderOptions;

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
    // The project's option values for the Rendered pack, with the World fog override mapped onto the pack's options.
    [[nodiscard]] std::map<std::string, std::string> shaderOptionsFor(state::editing::model::WorldSettings const& world
    ) const;

    friend class EditMode;
    friend class RenderMode;
};

} // namespace playback::editor::ui
