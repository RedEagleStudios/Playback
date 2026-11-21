#pragma once

#include "playback/state/editing/models/OverlayItem.h"

#include "mc/deps/core/math/Vec3.h"
#include "mc/platform/UUID.h"
#include "mc/world/level/BlockPos.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class DataItem;
class Packet;
class SerializedSkinRef;
enum class MinecraftPacketIds : int;

namespace playback::io {
class ReplayReader;
}

// Stand-ins: timeline layers that play a recorded player's actions again as another character. This holds what the
// replay engine reads from the recording for them; ReplaySession spawns and drives the characters.
namespace playback::replay {

// Stand-ins take ids from ranges no recording uses, next to the recorded player's sentinels
// (record::RecordedPlayerUniqueIdValue, record::RecordedPlayerRuntimeIdValue): they never meet a recorded actor's id and
// never raise the client's runtime-id allocator.
inline constexpr uint32_t StandInIdCount       = 65536;
inline constexpr int64_t  StandInUniqueIdBase  = std::numeric_limits<int64_t>::max() - 1024 - StandInIdCount;
inline constexpr uint64_t StandInRuntimeIdBase = (uint64_t{1} << 62) + (uint64_t{1} << 32);

[[nodiscard]] constexpr bool isStandInUniqueId(int64_t raw) noexcept {
    return raw >= StandInUniqueIdBase && raw < StandInUniqueIdBase + static_cast<int64_t>(StandInIdCount);
}

// Where the player was at the end of one recorded tick. The position is at the eyes, as players are recorded.
struct StandInPose {
    Vec3  position{};
    float pitch{};
    float yaw{};
    float headYaw{};
    float bodyYaw{};
    bool  onGround{};
};

// A recorded packet that animates or dresses the player (swing, held item, armour, data flags, actor event, animation),
// kept as recorded and pointed at the Stand-in when it plays.
struct StandInPacket {
    int                tick{};
    MinecraftPacketIds id{};
    std::string        payload;
    // SetActorData: the kept data items it carries (StandInData bits). PlayerEquipment: its container id.
    int detail{};
};

// SetActorData items a Stand-in takes from its source; everything else (name, size, properties) is the source's own.
enum StandInData : int { StandInFlags = 1, StandInPlayerFlags = 2, StandInBedPosition = 4, StandInFlags2 = 8 };

// One block the player changed (within reach of them), as recorded.
struct StandInBlock {
    int      tick{};
    BlockPos pos{};
    uint32_t layer{};
    uint32_t runtimeId{};
    uint8_t  flags{};
};

// One recorded player's actions over the whole recording, by replay tick.
struct StandInSource {
    std::string label;
    // Every runtime id the recording gave the player; their packets name them by these.
    std::vector<uint64_t> runtimeIds;
    // A recorded AddPlayer of the player: a player Stand-in is spawned from it.
    std::string addPlayer;
    // Per tick: the pose (carried over ticks without movement), whether the player was in the world, its dimension.
    std::vector<StandInPose> poses;
    std::vector<uint8_t>     present;
    std::vector<int>         dimensions;
    // Tick ascending.
    std::vector<StandInPacket> packets;
    std::vector<StandInBlock>  blocks;
    int                        firstTick{-1};
    int                        lastTick{-1};

    [[nodiscard]] bool presentAt(int tick) const noexcept {
        return tick >= 0 && static_cast<size_t>(tick) < present.size() && present[static_cast<size_t>(tick)] != 0;
    }
};

// Who the recording names, from its player lists: each player's last list entry (packet payload), name and the ticks
// they are in the world, by UUID. The recorder is also under the empty subject.
struct StandInRoster {
    std::string                                         recorderUuid;
    std::unordered_map<std::string, std::string>        playerLists;
    std::unordered_map<std::string, std::string>        names;
    std::unordered_map<std::string, std::pair<int, int>> spans;
};

// The recording as the scans read it: its chunk readers, the first tick of each chunk (one more entry: the end) and
// the dimension each chunk starts in.
struct StandInRecording {
    std::vector<std::unique_ptr<io::ReplayReader>> const& readers;
    std::vector<int>                                      chunkStarts;
    std::vector<int>                                      chunkDimensions;
};

[[nodiscard]] StandInRoster scanStandInRoster(StandInRecording const& recording);

// Reads every chunk for `subject` (a UUID, empty for the recorder). Null with `error` set (a sentence) when the
// recording has no such player.
[[nodiscard]] std::unique_ptr<StandInSource> buildStandInSource(
    StandInRecording const& recording,
    StandInRoster const&    roster,
    std::string const&      subject,
    std::string&            error
);

// Where a Stand-in plays: about `pivot` (where the source stood at the layer's sourceTick) turned `turn` degrees, then
// moved by `offset`.
struct StandInPlacement {
    Vec3  pivot{};
    Vec3  offset{};
    float turn{};
};

[[nodiscard]] StandInPose placeStandInPose(StandInPose pose, StandInPlacement const& placement);

// Blocks turn only in quarter turns; any other turn moves them by the offset alone.
[[nodiscard]] BlockPos placeStandInBlock(BlockPos const& pos, StandInPlacement const& placement);

// The recorded packet decoded and pointed at the Stand-in: the source's runtime ids become `target` and other actors'
// are dropped; a SetActorData keeps only its StandInData items (flags alone for a mob) with the name-tag flags set by
// `showName`. Null when nothing is left to send.
[[nodiscard]] std::shared_ptr<Packet> retargetStandInPacket(
    StandInPacket const& recorded,
    StandInSource const& source,
    uint64_t             target,
    bool                 mob,
    bool                 showName
);

[[nodiscard]] std::shared_ptr<Packet> decodeStandInPacket(MinecraftPacketIds id, std::string_view payload);

// Keeps the actor data items a Stand-in takes from its source (StandInData; flags alone for a mob) and sets its name-tag
// flags by `showName`.
void keepStandInData(std::vector<std::unique_ptr<DataItem>>& items, bool mob, bool showName);

// The skin a SkinFile Stand-in wears, from a skin PNG, on the classic or slim model. Null with `error` set.
[[nodiscard]] std::unique_ptr<SerializedSkinRef>
loadStandInSkin(std::string const& file, bool slim, std::string& error);

// One Stand-in character in the replay world.
struct StandInActor {
    int64_t  uniqueId{};
    uint64_t runtimeId{};
    // This spawn's player-list UUID; a new one each spawn so the client never keeps an old skin under it.
    mce::UUID   uuid;
    bool        spawned{};
    bool        mob{};
    // The world refused it (an unknown mob type); not tried again until the layer's look changes.
    bool failed{};
};

// What the session keeps for Stand-ins between ticks.
struct StandInRuntime {
    // The project's Stand-in layers, in layer order, as last given.
    std::vector<state::editing::model::OverlayItem> layers;
    // The layers as the driver last handled them, by id.
    std::unordered_map<std::string, state::editing::model::OverlayItem> applied;
    std::unordered_map<std::string, StandInActor>                       actors;
    // By subject; null when the recording has no such player (the reason is in sourceErrors).
    std::unordered_map<std::string, std::unique_ptr<StandInSource>> sources;
    std::unordered_map<std::string, std::string>                    sourceErrors;
    std::optional<StandInRoster>                                    roster;
    // Skin files by path and model; null when the file could not be used.
    std::unordered_map<std::string, std::unique_ptr<SerializedSkinRef>> fileSkins;
    // Item ids a layer chose that this game does not know, each logged once.
    std::unordered_set<std::string> unknownItems;
    uint32_t                                                            nextId{};
    bool                                                                layersChanged{};
    // Every Stand-in is placed and dressed again at the next update (after a seek or a snapshot).
    bool resync{};
    // A snapshot replaced the world: every Stand-in's blocks up to now go in again.
    bool blocksPending{};
    // A Stand-in's blocks changed after some were placed: the world is rebuilt from this tick once edits settle.
    std::optional<int> rebuildFrom;
    int                rebuildQuietTicks{};
    // Read by the seek that rebuilds: its snapshot is taken from no later than this tick.
    std::optional<int> seekRewindFrom;

    ~StandInRuntime();
};

} // namespace playback::replay
