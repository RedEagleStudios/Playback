#include "playback/editor/graphics/PreviewFrameAspect.h"

#include "playback/editor/graphics/PreviewFrameBand.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/renderer/ViewportInfo.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>

namespace playback::editor::graphics {

namespace {

std::atomic<float> gRequestedAspect{std::numeric_limits<float>::quiet_NaN()};

std::mutex                      gBandMutex;
std::optional<PreviewFrameBand> gBand;

// Client thread only: the window's own viewport while Playback has letterboxed it, and the band it set.
std::optional<ViewportRect> gWindow;
std::optional<ViewportRect> gApplied;

void publishBand(std::optional<PreviewFrameBand> band) {
    std::scoped_lock lock(gBandMutex);
    gBand = band;
}

ViewportRect rectOf(mce::ViewportInfo const& info) {
    return {info.offset->x, info.offset->y, info.size->x, info.size->y};
}

void setViewport(ClientInstance& client, ViewportRect const& rect) {
    auto info      = client.getViewportInfo();
    info.offset->x = rect.x;
    info.offset->y = rect.y;
    info.size->x   = rect.width;
    info.size->y   = rect.height;
    client.setViewportInfo(info);
}

} // namespace

void requestPreviewFrameAspect(std::optional<float> aspect) {
    gRequestedAspect.store(aspect.value_or(std::numeric_limits<float>::quiet_NaN()), std::memory_order_release);
}

std::optional<PreviewFrameBand> previewFrameBand() {
    std::scoped_lock lock(gBandMutex);
    return gBand;
}

void applyPreviewFrameAspect(bool replayActive) {
    auto client = ll::service::getClientInstance();
    if (!client) return;
    auto const current = rectOf(client->getViewportInfo());
    // The game set a viewport of its own (the window was resized): that is the window's now.
    if (gApplied && current != *gApplied) {
        gApplied.reset();
        gWindow.reset();
        publishBand(std::nullopt);
    }
    float const aspect = gRequestedAspect.load(std::memory_order_acquire);
    if (!replayActive || std::isnan(aspect)) {
        restorePreviewFrameAspect();
        return;
    }
    auto const window = gWindow.value_or(current);
    auto const band   = letterboxViewport(window, aspect);
    if (!band) {
        restorePreviewFrameAspect();
        return;
    }
    if (gApplied != band) {
        setViewport(*client, *band);
        gWindow  = window;
        gApplied = band;
    }
    publishBand(
        PreviewFrameBand{
            aspect,
            (band->y - window.y) / window.height,
            (band->y + band->height - window.y) / window.height,
        }
    );
}

void restorePreviewFrameAspect() {
    if (!gApplied) return;
    auto client = ll::service::getClientInstance();
    if (client && gWindow && rectOf(client->getViewportInfo()) == *gApplied) setViewport(*client, *gWindow);
    gApplied.reset();
    gWindow.reset();
    publishBand(std::nullopt);
}

} // namespace playback::editor::graphics
