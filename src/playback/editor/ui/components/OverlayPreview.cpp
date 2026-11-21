#include "OverlayPreview.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace playback::editor::ui {

namespace {

[[nodiscard]] bool hasActiveModel(std::vector<state::editing::model::OverlayItem> const& items, double tick) {
    return std::ranges::any_of(items, [tick](auto const& item) {
        return item.kind == state::editing::model::OverlayKind::Model && item.activeAt(tick);
    });
}

} // namespace

OverlayPreview::~OverlayPreview() {
    // A texture the live context still lists is handed back; one from a context that is gone is only freed.
    bool const live = mContext && ImGui::GetCurrentContext() == mContext;
    if (mTexture) mRetired.push_back(mTexture);
    for (auto* texture : mRetired) {
        if (live) ImGui::UnregisterUserTexture(texture);
        IM_DELETE(texture);
    }
}

void OverlayPreview::collectRetired() {
    std::erase_if(mRetired, [](ImTextureData* texture) {
        if (texture->Status != ImTextureStatus_Destroyed) return false;
        ImGui::UnregisterUserTexture(texture);
        IM_DELETE(texture);
        return true;
    });
}

bool OverlayPreview::ensureTexture(uint32_t width, uint32_t height) {
    // The editor's ImGui context can be rebuilt with the renderer; textures registered with the old one are dropped.
    if (mContext && ImGui::GetCurrentContext() != mContext) {
        if (mTexture) mRetired.push_back(mTexture);
        for (auto* texture : mRetired) IM_DELETE(texture);
        mRetired.clear();
        mTexture = nullptr;
        mContext = nullptr;
    }
    collectRetired();
    if (mTexture && mTexture->Width >= static_cast<int>(width) && mTexture->Height >= static_cast<int>(height)) {
        return true;
    }
    if (mTexture) {
        // Growing replaces the texture; the backend frees the old one, which is deleted once it reports so.
        mTexture->DestroyPixels();
        mTexture->SetStatus(ImTextureStatus_WantDestroy);
        mRetired.push_back(mTexture);
        mTexture = nullptr;
    }
    // Sized to the whole display once, so dragging a splitter does not churn GPU textures.
    ImVec2 const display = ImGui::GetIO().DisplaySize;
    int const    w       = std::max(static_cast<int>(width), static_cast<int>(std::ceil(display.x)));
    int const    h       = std::max(static_cast<int>(height), static_cast<int>(std::ceil(display.y)));
    if (w <= 0 || h <= 0) return false;
    mTexture = IM_NEW(ImTextureData)();
    mTexture->Create(ImTextureFormat_RGBA32, w, h);
    std::memset(mTexture->GetPixels(), 0, static_cast<size_t>(mTexture->GetSizeInBytes()));
    mContext = ImGui::GetCurrentContext();
    ImGui::RegisterUserTexture(mTexture);
    return true;
}

void OverlayPreview::upload() {
    auto* pixels = static_cast<unsigned char*>(mTexture->GetPixels());
    int const pitch    = mTexture->GetPitch();
    size_t const row   = static_cast<size_t>(mImage.width) * 4;
    for (uint32_t y = 0; y < mImage.height; ++y) {
        std::memcpy(pixels + static_cast<size_t>(y) * pitch, mImage.rgba.data() + y * row, row);
    }
    ImTextureRect const rect{0, 0, static_cast<unsigned short>(mImage.width), static_cast<unsigned short>(mImage.height)};
    // A texture still waiting to be created takes the whole buffer anyway.
    if (mTexture->Status == ImTextureStatus_WantCreate) return;
    mTexture->Updates.clear();
    mTexture->Updates.push_back(rect);
    mTexture->UpdateRect = rect;
    mTexture->UsedRect   = {0, 0, static_cast<unsigned short>(mTexture->Width), static_cast<unsigned short>(mTexture->Height)};
    mTexture->SetStatus(ImTextureStatus_WantUpdates);
}

void OverlayPreview::draw(
    ImDrawList*                                            drawList,
    std::vector<state::editing::model::OverlayItem> const& items,
    double                                                 tick,
    ImVec2                                                 frameMin,
    ImVec2                                                 frameMax,
    std::optional<overlay::OverlayCamera> const&           camera
) {
    if (!overlay::hasActiveOverlays(items, tick)) {
        mDrawn = false;
        mItems.clear();
        return;
    }
    auto const width  = static_cast<uint32_t>(std::max(0.0f, std::round(frameMax.x - frameMin.x)));
    auto const height = static_cast<uint32_t>(std::max(0.0f, std::round(frameMax.y - frameMin.y)));
    if (width < 2 || height < 2 || !ensureTexture(width, height)) return;

    // Models follow the camera, so a flying view re-renders them every frame; flat layers only when they change.
    bool const                  models = camera && hasActiveModel(items, tick);
    std::optional<::glm::dmat4> viewProjection;
    if (models) viewProjection = camera->viewProjection;
    bool const changed = items != mItems || tick != mTick || width != mImage.width || height != mImage.height
                      || viewProjection != mViewProjection;
    if (changed) {
        mItems          = items;
        mTick           = tick;
        mViewProjection = viewProjection;
        mDrawn          = overlay::renderOverlays(items, tick, width, height, models ? camera : std::nullopt, mImage);
        if (mDrawn) upload();
    }
    if (!mDrawn) return;
    float const u = static_cast<float>(mImage.width) / static_cast<float>(mTexture->Width);
    float const v = static_cast<float>(mImage.height) / static_cast<float>(mTexture->Height);
    drawList->AddImage(
        mTexture->GetTexRef(),
        frameMin,
        {frameMin.x + static_cast<float>(mImage.width), frameMin.y + static_cast<float>(mImage.height)},
        {0.0f, 0.0f},
        {u, v}
    );
}

} // namespace playback::editor::ui
