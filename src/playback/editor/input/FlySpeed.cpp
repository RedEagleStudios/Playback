#include "FlySpeed.h"

#include "playback/editor/input/EditorInput.h"
#include "playback/editor/ui/EditorTheme.h"

#include "ll/api/i18n/I18n.h"

#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/vanilla_components/MovementAbilitiesComponent.h"
#include "mc/deps/vanilla_components/MovementFloatAbilities.h"
#include "mc/entity/components/AbilitiesComponent.h"
#include "mc/world/actor/player/Abilities.h"
#include "mc/world/actor/player/AbilitiesIndex.h"
#include "mc/world/actor/player/Ability.h"
#include "mc/world/actor/player/LayeredAbilities.h"
#include "mc/world/actor/player/Player.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace playback::editor::input {

using namespace ll::i18n_literals;

namespace {

namespace theme   = ui::theme;
namespace metrics = ui::metrics;

// The multiplier is kStep raised to an integer level, so stepping back always lands on exactly 1x.
constexpr float kStep     = 1.25f;
constexpr int   kMinLevel = -13; // 0.055x
constexpr int   kMaxLevel = 13;  // 18.2x

constexpr float   kIndicatorSeconds = 1.5f;
constexpr float   kFadeSeconds      = 0.35f;
constexpr int64_t kNever            = std::numeric_limits<int64_t>::min();

// Written on the input thread, read on the client and render threads.
std::atomic<int>     gLevel{0};
std::atomic<int64_t> gLastChangeNanos{kNever};

int64_t nowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

float multiplierAt(int level) { return std::pow(kStep, static_cast<float>(level)); }

void setLevel(int level) {
    level = std::clamp(level, kMinLevel, kMaxLevel);
    gLevel.store(level, std::memory_order_release);
    gLastChangeNanos.store(nowNanos(), std::memory_order_release);
}

// One speed slot the game owns. The game (or a server abilities sync) may rewrite it at any time, so a value other
// than the one last written is taken as the game's new base rather than scaled twice.
struct TrackedSpeed {
    bool  known{};
    float base{};
    float written{};

    void apply(float& slot, float multiplier) {
        if (!known || slot != written) {
            base  = slot;
            known = true;
        }
        written = base * multiplier;
        slot    = written;
    }

    void restore(float& slot) {
        if (known && slot == written) slot = base;
        known = false;
    }
};

constexpr std::array<AbilitiesIndex, 2> kSpeedAbilities{AbilitiesIndex::FlySpeed, AbilitiesIndex::VerticalFlySpeed};
constexpr std::array<MovementFloatAbilities, 2> kMovementSpeeds{
    MovementFloatAbilities::FlySpeed,
    MovementFloatAbilities::VerticalFlySpeed,
};
constexpr size_t kLayerCount = static_cast<size_t>(AbilitiesLayer::Count);

// Client thread only.
Player*                                                gTrackedPlayer{};
std::array<std::array<TrackedSpeed, 2>, kLayerCount> gLayerSpeeds{};
std::array<TrackedSpeed, 2>                            gMovementSpeedSlots{};

void forgetTracking() {
    gLayerSpeeds        = {};
    gMovementSpeedSlots = {};
}

// Flight is driven by two copies of the speeds: the layered abilities (what the server syncs and what the game
// flattens) and the ECS MovementAbilitiesComponent the client's movement systems actually read each tick. The
// component is only refreshed from the layers when they change, so both are scaled; every layer that holds a float
// is scaled, so whichever one wins the flattening carries the multiplier.
template <class Fn>
void forEachSpeedSlot(Player& player, Fn&& fn) {
    auto& context = player.getEntityContext();
    if (auto* const abilities = context.tryGetComponent<AbilitiesComponent>().as_ptr()) {
        auto& layers = *abilities->mAbilities->mLayers;
        for (size_t layer = 0; layer < kLayerCount && layer < layers.size(); ++layer) {
            auto& entries = *layers[layer].mAbilities;
            for (size_t index = 0; index < kSpeedAbilities.size(); ++index) {
                auto& ability = entries[static_cast<size_t>(kSpeedAbilities[index])];
                auto& tracked = gLayerSpeeds[layer][index];
                if (ability.mType != Ability::Type::Float) {
                    tracked.known = false;
                    continue;
                }
                fn(tracked, ability.mValue->mFloatVal);
            }
        }
    }
    if (auto* const movement = context.tryGetComponent<MovementAbilitiesComponent>().as_ptr()) {
        auto& values = *movement->mFloatValues;
        for (size_t index = 0; index < kMovementSpeeds.size(); ++index) {
            fn(gMovementSpeedSlots[index], values[static_cast<size_t>(kMovementSpeeds[index])]);
        }
    }
}

ImU32 faded(ImU32 colour, float alpha) {
    auto const base = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff);
    return theme::withAlpha(colour, static_cast<int>(std::lround(base * alpha)));
}

} // namespace

float flySpeedMultiplier() { return multiplierAt(gLevel.load(std::memory_order_acquire)); }

void stepFlySpeed(int notches) {
    if (notches == 0) return;
    int current = gLevel.load(std::memory_order_acquire);
    int next{};
    do {
        next = std::clamp(current + notches, kMinLevel, kMaxLevel);
    } while (!gLevel.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire));
    // Also stamped at the limits, so the pill shows the user why the wheel stopped doing anything.
    gLastChangeNanos.store(nowNanos(), std::memory_order_release);
}

void resetFlySpeed() {
    if (gLevel.load(std::memory_order_acquire) != 0) setLevel(0);
}

void tickFlySpeed(Player* player, bool replayOpen) {
    if (!replayOpen || !player) {
        if (!gTrackedPlayer) return;
        // A different (or no) local player means the observer went with its world; there is nothing left to restore.
        if (player == gTrackedPlayer) {
            forEachSpeedSlot(*player, [](TrackedSpeed& tracked, float& slot) { tracked.restore(slot); });
        }
        gTrackedPlayer = nullptr;
        forgetTracking();
        resetFlySpeed();
        return;
    }

    if (player != gTrackedPlayer) {
        gTrackedPlayer = player;
        forgetTracking();
    }
    float const multiplier = flySpeedMultiplier();
    forEachSpeedSlot(*player, [multiplier](TrackedSpeed& tracked, float& slot) { tracked.apply(slot, multiplier); });
}

void drawFlySpeedIndicator(ImDrawList* drawList, ImVec2 videoMin, ImVec2 videoMax) {
    if (!drawList || videoMax.x <= videoMin.x || videoMax.y <= videoMin.y) return;

    float alpha = 1.0f;
    if (!isGameInputCaptured()) {
        int64_t const changed = gLastChangeNanos.load(std::memory_order_acquire);
        if (changed == kNever) return;
        float const since = static_cast<float>(nowNanos() - changed) * 1e-9f;
        if (since < 0.0f || since >= kIndicatorSeconds) return;
        alpha = std::clamp((kIndicatorSeconds - since) / kFadeSeconds, 0.0f, 1.0f);
    }

    int const         level = gLevel.load(std::memory_order_acquire);
    std::string const value = fmt::format("{:.2f}", multiplierAt(level));
    std::string const text  = "playback.refactorEditor.viewport.flySpeed"_tr(value);

    float const  font     = metrics::font();
    float const  scale    = metrics::scale();
    float const  padX     = font * 0.8f;
    float const  padY     = font * 0.35f;
    float const  barGap   = font * 0.3f;
    float const  barH     = std::max(2.0f, 2.0f * scale);
    ImVec2 const textSize = ImGui::CalcTextSize(text.c_str());
    float const  width    = textSize.x + padX * 2.0f;
    float const  height   = textSize.y + barGap + barH + padY * 2.0f;
    float const  centreX  = (videoMin.x + videoMax.x) * 0.5f;
    float const  bottom   = videoMax.y - font * 1.5f;
    ImVec2 const pillMin{std::floor(centreX - width * 0.5f), std::floor(bottom - height)};
    ImVec2 const pillMax{pillMin.x + std::ceil(width), pillMin.y + std::ceil(height)};
    float const  rounding = std::min(height * 0.5f, font * 0.6f);

    drawList->AddRectFilled(pillMin, pillMax, faded(theme::kOverlayBg, alpha), rounding);
    drawList->AddRect(pillMin, pillMax, faded(theme::kBorder, alpha), rounding, 0, std::max(1.0f, scale));
    drawList->AddText({pillMin.x + padX, pillMin.y + padY}, faded(theme::kText, alpha), text.c_str());

    // The bar is logarithmic, matching the steps: 1x sits in the middle.
    float const  barTop = pillMin.y + padY + textSize.y + barGap;
    ImVec2 const barMin{pillMin.x + padX, barTop};
    ImVec2 const barMax{pillMax.x - padX, barTop + barH};
    float const  position =
        static_cast<float>(level - kMinLevel) / static_cast<float>(kMaxLevel - kMinLevel) * (barMax.x - barMin.x);
    float const unity =
        static_cast<float>(-kMinLevel) / static_cast<float>(kMaxLevel - kMinLevel) * (barMax.x - barMin.x);
    drawList->AddRectFilled(barMin, barMax, faded(theme::kBorder, alpha), barH * 0.5f);
    drawList->AddRectFilled(barMin, {barMin.x + position, barMax.y}, faded(theme::kAccent, alpha), barH * 0.5f);
    drawList->AddLine(
        {barMin.x + unity, barMin.y - barH},
        {barMin.x + unity, barMax.y + barH},
        faded(theme::kText, alpha * 0.5f),
        std::max(1.0f, scale)
    );
}

} // namespace playback::editor::input
