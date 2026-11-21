#pragma once

#include "playback/editor/ui/PanelContext.h"
#include "playback/editor/ui/components/VanillaPicker.h"
#include "playback/state/editing/models/OverlayItem.h"

#include <array>
#include <optional>
#include <string>

namespace playback::editor::ui {

// The inspector for a selected layer (title, shape, model, blur or light): its timing, look and placement.
class LayerInspector {
public:
    // The selected layer's name for the inspector header; empty when no layer is selected.
    [[nodiscard]] static std::string subject(PanelContext const& ctx);
    // True when it owns the current selection and has drawn it.
    bool draw(PanelContext const& ctx);

private:
    void drawLayerSection(state::editing::model::OverlayItem& item, int totalTicks);
    void drawTitleSection(state::editing::model::OverlayItem& item);
    void drawShapeSection(state::editing::model::OverlayItem& item);
    void drawTransformSection(state::editing::model::OverlayItem& item);
    void drawModelSection(state::editing::model::OverlayItem& item);
    void drawBlurSection(state::editing::model::OverlayItem& item);
    void drawLightSection(state::editing::model::OverlayItem& item);
    void drawStandInSection(PanelContext const& ctx, state::editing::model::OverlayItem& item);
    void drawScreenSection(state::editing::model::OverlayItem& item);

    // A keyable number of the layer: its diamond keys it at the playhead, which is mLocalTick into the layer.
    void keyRow(
        state::editing::model::OverlayItem& item,
        char const*                         id,
        char const*                         icon,
        std::string const&                  text,
        char const*                         name,
        float                               low,
        float                               high,
        char const*                         format,
        float                               displayScale = 1.0f
    );
    // Sets a property at the playhead by keying it there, as an edit in its row does.
    void setKeyed(state::editing::model::OverlayItem& item, char const* name, float value);
    // The layer while a control is held: shown live, committed as one undo step on release.
    std::optional<state::editing::model::OverlayItem> mDraft;
    // Text fields edit local buffers so typing is not overwritten by the project each frame.
    std::string            mBufferItemId;
    int                                               mLocalTick{};
    // A key arrow's request to move the playhead, in the layer's ticks.
    std::optional<int>                                mSeek;
    std::array<char, 128>  mNameBuffer{};
    std::array<char, 2048> mTextBuffer{};
    std::array<char, 64>   mFontFilter{};
    std::string            mModelError;
    std::string            mModelErrorFile;
    VanillaPicker          mVanillaPicker;
    std::array<char, 64>   mNameTagBuffer{};
    std::array<char, 96>   mMobBuffer{};
    // The item id typed into an open slot list.
    std::array<char, 96>   mGearBuffer{};
};

} // namespace playback::editor::ui
