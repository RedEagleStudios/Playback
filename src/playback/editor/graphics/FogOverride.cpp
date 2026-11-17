#include "FogOverride.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/client/world/level/fog/FogDistanceSetting.h"

#include <mutex>
#include <optional>
#include <utility>

namespace playback::editor::graphics {

namespace {

std::mutex                 gFogMutex;
std::optional<FogOverride> gFog;

std::optional<FogOverride> currentFog() {
    std::scoped_lock lock(gFogMutex);
    return gFog;
}

// What setupFog resolved for the frame the override replaced; put back before the next call so the game's own fog
// transitions never blend from the override. Render thread only.
std::optional<FogDistanceSetting> gResolvedFog;

// setupFog resolves this frame's distance fog into the camera, which the frame's draws then read; the override
// replaces the resolved fog for this frame only.
LL_TYPE_INSTANCE_HOOK(
    ReplayFogOverrideHook,
    ll::memory::HookPriority::Normal,
    LevelRendererPlayer,
    &LevelRendererPlayer::$setupFog,
    void,
    ::ScreenContext& screenContext,
    float const      skyIntensityOverride
) {
    auto& level    = *static_cast<LevelRendererPlayer*>(this);
    auto& resolved = *level.mCurrentDistanceFog;
    if (auto const game = std::exchange(gResolvedFog, std::nullopt)) resolved = *game;
    origin(screenContext, skyIntensityOverride);

    auto const fog = currentFog();
    if (!fog) return;
    // By now the frame reads the resolved fog in blocks, whatever its type says.
    float const renderDistance = level.mRenderDistance;
    gResolvedFog               = resolved;
    resolved.mStart            = fog->start * renderDistance;
    resolved.mEnd              = fog->end * renderDistance;
    resolved.mType             = FogDistanceSetting::DistanceType::Fixed;
}

} // namespace

void setFogOverride(std::optional<FogOverride> fog) {
    std::scoped_lock lock(gFogMutex);
    gFog = fog;
}

bool hookFogOverride(bool enable) {
    static bool installed = false;
    if (enable == installed) return true;
    if (enable) {
        installed = ReplayFogOverrideHook::hook() == 0;
        return installed;
    }
    installed = !ReplayFogOverrideHook::unhook();
    if (!installed) setFogOverride(std::nullopt);
    return !installed;
}

} // namespace playback::editor::graphics
