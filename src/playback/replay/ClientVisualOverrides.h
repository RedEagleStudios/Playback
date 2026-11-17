#pragma once

#include "playback/state/editing/models/WorldSettings.h"

#include <memory>
#include <optional>
#include <vector>

class ResourcePackStack;

namespace playback::replay {

// The project's graphics mode and global resource packs, put on the viewer's client for a replay and taken off again
// afterwards. Client thread only. global_resource_packs.json is never written. The graphics mode is set without
// saving, but the game may write its options file while the override is on, so a crash mid-replay can leave the
// project's mode in the viewer's options.
class ClientVisualOverrides {
public:
    ClientVisualOverrides();
    ~ClientVisualOverrides();

    ClientVisualOverrides(ClientVisualOverrides const&)            = delete;
    ClientVisualOverrides& operator=(ClientVisualOverrides const&) = delete;

    // nullopt: the viewer's own graphics mode. Follows the project live.
    void requestGraphics(std::optional<state::editing::model::GraphicsStyle> graphics) { mGraphics = graphics; }

    // Before the replay world loads: these packs, top priority first, replace the viewer's global packs, and the
    // world's load picks them up. Swapping them in a loaded world would need a resource reload, so they hold for the
    // whole replay. nullopt keeps the viewer's own.
    void useGlobalPacks(std::optional<std::vector<state::editing::model::GlobalPack>> const& packs);

    // Makes the client's graphics mode match the request.
    void apply();

    // The viewer's own graphics mode and global packs back.
    void restore();

    // The installed resource packs a project can use as global packs, read on the first apply after a restore.
    [[nodiscard]] std::shared_ptr<std::vector<state::editing::model::GlobalPack> const> installedPacks() const {
        return mInstalledPacks;
    }

private:
    void restoreGraphics();
    void restoreGlobalPacks();
    void readInstalledPacks();

    std::optional<state::editing::model::GraphicsStyle> mGraphics;
    std::optional<int>                                  mSavedGraphics;
    // The viewer's own global packs while the project's are in place.
    std::unique_ptr<ResourcePackStack>                                    mSavedGlobalStack;
    std::shared_ptr<std::vector<state::editing::model::GlobalPack> const> mInstalledPacks;
};

} // namespace playback::replay
