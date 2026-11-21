#include "BlurEffects.h"

#include "playback/editor/graphics/CameraRenderHooks.h"

#include <algorithm>
#include <memory>
#include <mutex>

namespace playback::editor::graphics {
namespace {

using state::editing::model::OverlayItem;
using state::editing::model::OverlayKind;

std::mutex gMutex;
// Swapped whole, so a frame copies a pointer rather than the layers.
std::shared_ptr<std::vector<OverlayItem> const> gLayers;
double                                          gFallbackTick{};
float                                           gPreviewAspect{16.0f / 9.0f};
std::optional<float>                            gExportAspect;

} // namespace

void setBlurLayers(std::vector<OverlayItem> layers) {
    std::erase_if(layers, [](OverlayItem const& item) { return item.kind != OverlayKind::Blur; });
    auto shared = layers.empty() ? nullptr : std::make_shared<std::vector<OverlayItem> const>(std::move(layers));
    std::scoped_lock lock(gMutex);
    gLayers = std::move(shared);
}

void setBlurFallbackTick(double tick) {
    std::scoped_lock lock(gMutex);
    gFallbackTick = tick;
}

void setBlurPreviewAspect(float aspect) {
    if (!(aspect > 0.0f)) return;
    std::scoped_lock lock(gMutex);
    gPreviewAspect = aspect;
}

void setBlurExportAspect(std::optional<float> aspect) {
    std::scoped_lock lock(gMutex);
    gExportAspect = aspect && *aspect > 0.0f ? aspect : std::nullopt;
}

std::vector<BlurPass> currentBlurPasses() {
    std::shared_ptr<std::vector<OverlayItem> const> layers;
    double                                          tick{};
    {
        std::scoped_lock lock(gMutex);
        layers = gLayers;
        tick   = gFallbackTick;
    }
    if (!layers) return {};
    if (auto const projection = currentRenderCameraProjection(); projection && projection->sampleTime) {
        tick = static_cast<double>(projection->sampleTime->value());
    }

    std::vector<BlurPass> passes;
    for (auto const& layer : *layers) {
        float const fade = layer.fadeAt(tick);
        if (fade <= 0.0f) continue;
        auto const  item = state::editing::model::overlayAt(layer, tick);
        auto const& blur = item.blur;
        float const mix  = std::clamp(item.transform.opacity, 0.0f, 1.0f) * fade;
        bool const  radial = blur.style == state::editing::model::BlurStyle::Radial;
        float const amount = std::clamp(blur.amount, 0.0f, radial ? 1.0f : 0.5f);
        if (mix <= 0.0f || amount <= 0.0f) continue;
        passes.push_back({
            blur.style,
            amount,
            blur.angle,
            item.transform.x,
            item.transform.y,
            std::max(0.0f, item.transform.width),
            std::max(0.0f, item.transform.height),
            item.transform.rotation,
            std::max(0.0f, blur.feather),
            blur.area,
            blur.invert,
            mix,
        });
    }
    return passes;
}

float blurFrameAspect() {
    std::scoped_lock lock(gMutex);
    return gExportAspect.value_or(gPreviewAspect);
}

} // namespace playback::editor::graphics
