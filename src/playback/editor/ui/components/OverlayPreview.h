#pragma once

#include "playback/overlay/OverlayRenderer.h"

#include "imgui.h"

#include <optional>
#include <vector>

struct ImGuiContext;

namespace playback::editor::ui {

// The project's title, shape and model layers drawn over the viewport picture, by the same renderer the export
// composites with, so the preview shows exactly what the export will. Re-rendered only when what it shows changes.
class OverlayPreview {
public:
    OverlayPreview() = default;
    ~OverlayPreview();
    OverlayPreview(OverlayPreview const&)            = delete;
    OverlayPreview& operator=(OverlayPreview const&) = delete;

    // `camera` maps NDC into the frame rectangle [frameMin, frameMax]; without it models are left out.
    void draw(
        ImDrawList*                                      drawList,
        std::vector<state::editing::model::OverlayItem> const& items,
        double                                           tick,
        ImVec2                                           frameMin,
        ImVec2                                           frameMax,
        std::optional<overlay::OverlayCamera> const&     camera
    );

private:
    [[nodiscard]] bool ensureTexture(uint32_t width, uint32_t height);
    void               upload();
    void               collectRetired();

    ImTextureData* mTexture{};
    // Outgrown textures, deleted once the backend has freed them.
    std::vector<ImTextureData*> mRetired;
    ImGuiContext*               mContext{};
    overlay::OverlayImage                           mImage;
    bool                                            mDrawn{};
    std::vector<state::editing::model::OverlayItem> mItems;
    double                                          mTick{-1.0};
    std::optional<::glm::dmat4>                     mViewProjection;
};

} // namespace playback::editor::ui
