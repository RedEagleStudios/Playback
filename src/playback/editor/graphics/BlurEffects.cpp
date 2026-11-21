#include "BlurEffects.h"

#include "playback/editor/graphics/CameraRenderHooks.h"

#include <algorithm>
#include <array>
#include <cmath>
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

// A colour temperature as linear RGB (Tanner Helland's fit of the Planckian locus, in sRGB, then linearised),
// scaled so 6500 K is white (1, 1, 1).
std::array<float, 3> kelvinColour(float kelvin) {
    auto const fit = [](double kelvinValue) {
        double const t = std::clamp(kelvinValue, 1000.0, 40000.0) / 100.0;
        double const r = t <= 66.0 ? 255.0 : 329.698727446 * std::pow(t - 60.0, -0.1332047592);
        double const g = t <= 66.0 ? 99.4708025861 * std::log(t) - 161.1195681661
                                   : 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
        double const b = t >= 66.0 ? 255.0 : t <= 19.0 ? 0.0 : 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
        auto const linear = [](double value) { return std::pow(std::clamp(value, 0.0, 255.0) / 255.0, 2.2); };
        return std::array{linear(r), linear(g), linear(b)};
    };
    static auto const white  = fit(6500.0);
    auto const        colour = fit(static_cast<double>(kelvin));
    return {
        static_cast<float>(colour[0] / white[0]),
        static_cast<float>(colour[1] / white[1]),
        static_cast<float>(colour[2] / white[2]),
    };
}

} // namespace

void setBlurLayers(std::vector<OverlayItem> layers) {
    std::erase_if(layers, [](OverlayItem const& item) {
        return item.kind != OverlayKind::Blur && item.kind != OverlayKind::Light;
    });
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
        float const mix  = std::clamp(item.transform.opacity, 0.0f, 1.0f) * fade;
        if (mix <= 0.0f) continue;
        if (item.kind == OverlayKind::Light) {
            auto const& light = item.light;
            if (light.level == 0.0f) continue;
            BlurPass pass;
            pass.light      = true;
            pass.lightStyle = light.style;
            pass.centreX    = item.transform.x;
            pass.centreY    = item.transform.y;
            pass.width      = std::max(0.0f, item.transform.width);
            pass.height     = std::max(0.0f, item.transform.height);
            pass.rotation   = item.transform.rotation;
            pass.mix        = mix;
            pass.level      = std::clamp(light.level, -1.0f, 20.0f);
            pass.hardness   = std::clamp(light.hardness, 0.0f, 1.0f);
            pass.spread     = std::clamp(light.spread, 0.5f, 89.0f);
            // The gel is picked as an sRGB colour; it multiplies the temperature's colour in linear light.
            auto const temperature = kelvinColour(light.kelvin);
            for (size_t channel = 0; channel < 3; ++channel) {
                float const gel      = std::clamp(light.filter[channel], 0.0f, 1.0f);
                pass.colour[channel] = temperature[channel] * std::pow(gel, 2.2f);
            }
            passes.push_back(pass);
            continue;
        }
        auto const& blur   = item.blur;
        bool const  radial = blur.style == state::editing::model::BlurStyle::Radial;
        float const amount = std::clamp(blur.amount, 0.0f, radial ? 1.0f : 0.5f);
        if (amount <= 0.0f) continue;
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
