#pragma once

#include "playback/replay/MediaDecoder.h"
#include "playback/state/editing/models/OverlayItem.h"

#include "mc/deps/core/math/Vec3.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace playback::replay {

// The furniture add-on's screens (Red Eagle Studios, namespace re_furn). Each is a client entity that draws its model
// with one texture its entity data picks: a TV by its variant (0 off, 1-5 a looping clip; the stand and hanging models
// of a size share their textures), a monitor by its skin_id (its colour, 0-4), with a black 'off' plate over the screen
// while its re_furn:turned_on property is false.
enum class ScreenGroup : uint8_t { MediumTv, BigTv, Monitor, WideMonitor };

struct ScreenType {
    std::string_view entity;
    ScreenGroup      group;
    // Under playback.refactorEditor.screen.kinds.
    std::string_view labelKey;
};

[[nodiscard]] ScreenType const* screenTypeOf(std::string_view entityType);

// "mediumTv", "bigTv", "monitor" or "wideMonitor".
[[nodiscard]] std::string_view screenGroupKey(ScreenGroup group);

// TVs pick their texture by variant, monitors by skin_id.
[[nodiscard]] constexpr bool isTvGroup(ScreenGroup group) noexcept {
    return group == ScreenGroup::MediumTv || group == ScreenGroup::BigTv;
}

// The values of variant (TVs) or skin_id (monitors) whose texture a layer may take, most preferred first. A TV's 0 is
// its 'off' picture, which stays its own.
[[nodiscard]] std::span<int const> screenSlots(ScreenGroup group);

// The size a picture for the group's screens is decoded at: the screen's own shape, at the resolution its texture is
// replaced with.
[[nodiscard]] std::pair<uint32_t, uint32_t> screenPictureSize(ScreenGroup group);

// The add-on's screen textures in the game's TextureGroup, replaced with layer pictures and put back. Client thread.
class ScreenTextures {
public:
    ScreenTextures();
    ~ScreenTextures();
    ScreenTextures(ScreenTextures const&)            = delete;
    ScreenTextures& operator=(ScreenTextures const&) = delete;

    // Asks the game for the slot's texture, so it is loaded before the first screen draws with it.
    void prepare(ScreenGroup group, int slot);
    // Shows `picture` (screenPictureSize) on every screen drawing with the slot, at `brightness` percent. Uploads only
    // when the picture or brightness changed. False while the slot's texture cannot be replaced (not loaded yet).
    bool show(ScreenGroup group, int slot, std::shared_ptr<MediaFrame const> const& picture, float brightness);
    // Puts the slot's own picture back.
    void restore(ScreenGroup group, int slot);
    void restoreAll();

private:
    struct Slot;
    Slot& slot(ScreenGroup group, int index);
    // Lights a replaced TV texture's MER map evenly, so the channel's own outline does not shade the picture.
    void flattenMer(Slot& entry);

    std::map<std::pair<ScreenGroup, int>, std::unique_ptr<Slot>> mSlots;
};

// A Screen layer showing now, on the screen `target` (its recorded unique id) through the texture of `slot`.
struct ScreenShowing {
    int64_t                                               target{};
    ScreenGroup                                           group{};
    int                                                   slot{-1};
    // The media as decoded for this screen; replaced when the file or the fit changes.
    std::string                                           file;
    state::editing::model::ScreenFit                      fit{};
    std::unique_ptr<VideoStream>                          video;
    std::shared_future<std::shared_ptr<MediaFrame const>> still;
    // The screen's entity data is held on the slot (from the first picture on).
    bool                                                  holding{};
};

// A screen whose variant or skin_id (and power) a layer holds on its slot, and what the recording says they are, put
// back when the layer lets go. Recorded changes that arrive meanwhile are noted here and not applied.
struct ScreenHold {
    uint64_t                              runtimeId{};
    ScreenGroup                           group{};
    int                                   slot{};
    std::optional<int>                    recordedVariant;
    std::optional<int>                    recordedSkin;
    std::optional<bool>                   recordedOn;
    // Monitors: re_furn:turned_on's index in the entity's synced properties.
    std::optional<uint32_t>               onIndex;
    std::chrono::steady_clock::time_point sentAt{};
    int                                   sends{};
};

// A screen entity the replay world has had, for the editor's target list.
struct ScreenSighting {
    ScreenType const* type{};
    ::Vec3            position{};
    bool              present{};
    // The variant (TVs) or skin_id (monitors) it shows as recorded; -1 for none (a TV or monitor that is off).
    int               shownSlot{-1};
};

struct ScreenRuntime {
    std::vector<state::editing::model::OverlayItem> layers;
    // By layer id.
    std::unordered_map<std::string, ScreenShowing>  showing;
    // By the screen's unique id.
    std::unordered_map<int64_t, ScreenHold>         held;
    std::unordered_map<int64_t, ScreenSighting>     seen;
    // The slots the layers showed on last frame.
    std::set<std::pair<ScreenGroup, int>>           slotsShown;
    ScreenTextures                                  textures;
    // Warnings already logged.
    std::unordered_set<std::string>                 warned;
};

} // namespace playback::replay
