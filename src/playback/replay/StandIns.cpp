#include "StandIns.h"

#include "StandInSkinImage.h"

#include "playback/action/Action.h"
#include "playback/io/AsyncReplaySaver.h"
#include "playback/record/Recorder.h"

#include "mc/deps/core/container/Blob.h"
#include "mc/deps/core/image/Image.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/deps/json/Value.h"
#include "mc/deps/shared_types/persona/ArmSizeType.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/ActorEventPacket.h"
#include "mc/network/packet/AddPlayerPacket.h"
#include "mc/network/packet/AnimateEntityPacket.h"
#include "mc/network/packet/AnimatePacket.h"
#include "mc/network/packet/ChangeDimensionPacket.h"
#include "mc/network/packet/MobArmorEquipmentPacket.h"
#include "mc/network/packet/MobEquipmentPacket.h"
#include "mc/network/packet/PlayerListPacket.h"
#include "mc/network/packet/PlayerListPacketPayload.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/network/packet/SetActorDataPacket.h"
#include "mc/network/packet/UpdateBlockPacket.h"
#include "mc/network/packet/UpdateBlockSyncedPacket.h"
#include "mc/network/packet/UpdateSubChunkBlocksPacket.h"
#include "mc/network/packet/UpdateSubChunkNetworkBlockInfo.h"
#include "mc/network/packet/cerealize/core/SerializationMode.h"
#include "mc/platform/UUID.h"
#include "mc/resources/MinEngineVersion.h"
#include "mc/world/actor/ActorDataIDs.h"
#include "mc/world/actor/ActorFlags.h"
#include "mc/world/actor/DataItem.h"
#include "mc/world/actor/player/SerializedSkinRef.h"
#include "mc/world/actor/player/TrustedSkinFlag.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <numbers>
#include <stdexcept>
#include <unordered_set>

namespace playback::replay {
using namespace playback::action;
using playback::io::PlaybackBuffer;

namespace {

// How far from the player a changed block counts as theirs: a little past reach, measured from mid-body.
constexpr float StandInBlockReach = 8.0f;

struct RecordedGamePacket {
    MinecraftPacketIds id{};
    std::string_view   payload;
};

RecordedGamePacket splitGamePacket(std::string_view bytes) {
    // Only the id is copied to read it: chunk packets ride in these actions too.
    std::string    head(bytes.substr(0, std::min<size_t>(bytes.size(), 10)));
    PlaybackBuffer data(head);
    auto const     id = static_cast<MinecraftPacketIds>(data.getVarInt().value());
    return {id, bytes.substr(static_cast<size_t>(data.mReadPointer))};
}

float decodeRotationByte(uchar value) { return static_cast<float>(static_cast<schar>(value)) * (360.0f / 256.0f); }

// Each sample of a move_entities action, in both of its layouts, as ReplaySession::handleMoveEntities reads them.
template <class Visit>
void forEachMoveSample(std::string_view bytes, Visit&& visit) {
    std::string    copy(bytes);
    PlaybackBuffer data(copy);
    auto const     markerOrCount = data.getVarInt().value();
    if (markerOrCount == 0) {
        auto const count = data.getVarInt().value();
        for (int index = 0; index < count; ++index) {
            int64_t const id = data.getVarInt64().value();
            StandInPose   pose;
            pose.position = Vec3{data.getFloat().value(), data.getFloat().value(), data.getFloat().value()};
            pose.pitch    = data.getFloat().value();
            pose.yaw      = data.getFloat().value();
            pose.headYaw  = data.getFloat().value();
            pose.bodyYaw  = data.getFloat().value();
            pose.onGround = data.getBool().value();
            visit(id, pose);
        }
        return;
    }
    for (int index = 0; index < markerOrCount; ++index) {
        int64_t const id       = data.getVarInt64().value();
        bool const    isPlayer = data.getBool().value();
        StandInPose   pose;
        pose.position = Vec3{data.getFloat().value(), data.getFloat().value(), data.getFloat().value()};
        if (isPlayer) {
            pose.pitch    = data.getFloat().value();
            pose.yaw      = data.getFloat().value();
            pose.headYaw  = data.getFloat().value();
            pose.bodyYaw  = pose.yaw;
            pose.onGround = data.getBool().value();
        } else {
            auto const header = data.getByte().value();
            pose.pitch        = decodeRotationByte(data.getByte().value());
            pose.yaw          = decodeRotationByte(data.getByte().value());
            pose.headYaw      = decodeRotationByte(data.getByte().value());
            pose.bodyYaw      = decodeRotationByte(data.getByte().value());
            pose.onGround     = (header & 1) != 0;
        }
        visit(id, pose);
    }
}

StandInPose addPlayerPose(AddPlayerPacket const& add) {
    StandInPose pose;
    pose.position = *add.mPos;
    pose.pitch    = add.mRot->x;
    pose.yaw      = add.mRot->y;
    pose.headYaw  = add.mYHeadRot;
    pose.bodyYaw  = add.mRot->y;
    pose.onGround = true;
    return pose;
}

bool namesRuntimeId(Packet const& packet, std::unordered_set<uint64_t> const& ids) {
    switch (packet.getId()) {
    case MinecraftPacketIds::Animate:
        return ids.contains(static_cast<AnimatePacket const&>(packet).mRuntimeId->rawID);
    case MinecraftPacketIds::PlayerEquipment:
        return ids.contains(static_cast<MobEquipmentPacket const&>(packet).mRuntimeId->rawID);
    case MinecraftPacketIds::MobArmorEquipment:
        return ids.contains(static_cast<MobArmorEquipmentPacket const&>(packet).mRuntimeId->rawID);
    case MinecraftPacketIds::SetActorData:
        return ids.contains(static_cast<SetActorDataPacket const&>(packet).mId->rawID);
    case MinecraftPacketIds::ActorEvent:
        return ids.contains(static_cast<ActorEventPacket const&>(packet).mRuntimeId->rawID);
    case MinecraftPacketIds::TriggerAnimation: {
        auto const& targets = *static_cast<AnimateEntityPacket const&>(packet).mRuntimeIds;
        return std::ranges::any_of(targets, [&](ActorRuntimeID id) { return ids.contains(id.rawID); });
    }
    default:
        return false;
    }
}

int standInDataBit(ushort id) {
    switch (static_cast<ActorDataIDs>(id)) {
    case ActorDataIDs::Reserved0:
        return StandInFlags;
    case ActorDataIDs::PlayerFlags:
        return StandInPlayerFlags;
    case ActorDataIDs::BedPosition:
        return StandInBedPosition;
    case ActorDataIDs::Reserved092:
        return StandInFlags2;
    default:
        return 0;
    }
}

int standInDataMask(SetActorDataPacket const& packet) {
    int mask = 0;
    for (auto const& item : *packet.mPackedItems) {
        if (item && item->getId() <= 0xFF) mask |= standInDataBit(item->getId());
    }
    return mask;
}

template <class Visit>
void forEachChangedBlock(Packet const& packet, Visit&& visit) {
    switch (packet.getId()) {
    case MinecraftPacketIds::UpdateBlock: {
        auto const& update = static_cast<UpdateBlockPacket const&>(packet);
        visit(*update.mPos, static_cast<uint32_t>(update.mLayer), static_cast<uint32_t>(update.mRuntimeId), static_cast<uint8_t>(update.mUpdateFlags));
        break;
    }
    case MinecraftPacketIds::UpdateBlockSynced: {
        auto const& update = static_cast<UpdateBlockSyncedPacket const&>(packet);
        visit(*update.mPos, static_cast<uint32_t>(update.mLayer), static_cast<uint32_t>(update.mRuntimeId), static_cast<uint8_t>(update.mUpdateFlags));
        break;
    }
    case MinecraftPacketIds::UpdateSubChunkBlocks: {
        auto const& changed = *static_cast<UpdateSubChunkBlocksPacket const&>(packet).mBlocksChanged;
        for (auto const& info : *changed.mStandards) {
            visit(*info.mPos, 0u, static_cast<uint32_t>(info.mRuntimeId), static_cast<uint8_t>(info.mUpdateFlags));
        }
        for (auto const& info : *changed.mExtras) {
            visit(*info.mPos, 1u, static_cast<uint32_t>(info.mRuntimeId), static_cast<uint8_t>(info.mUpdateFlags));
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

StandInRuntime::~StandInRuntime() = default;

std::shared_ptr<Packet> decodeStandInPacket(MinecraftPacketIds id, std::string_view payload) {
    auto packet = MinecraftPackets::createPacket(id);
    if (!packet) return nullptr;
    // As in ReplaySession::applyGamePacket: the factory's mode can disagree with the one the recorder wrote.
    if (id == MinecraftPacketIds::MobArmorEquipment) {
        packet->setSerializationMode(SerializationMode::SemanticSideBySideLogOnMismatch);
    }
    ReadOnlyBinaryStream stream(payload, false);
    if (!static_cast<bool>(packet->read(stream)) || !static_cast<bool>(stream.ensureReadCompleted())) return nullptr;
    return packet;
}

StandInRoster scanStandInRoster(StandInRecording const& recording) {
    StandInRoster roster;
    // A player is named by UUID in the player list, by unique id in AddPlayer (nil UUIDs in old recordings) and
    // RemoveActor. The recorder is the empty subject.
    std::unordered_map<int64_t, std::string> uuidByUniqueId;
    std::unordered_set<std::string>          inWorld;
    auto const                               enter = [&](std::string const& uuid, int tick) {
        inWorld.insert(uuid);
        auto const [span, added] = roster.spans.try_emplace(uuid, tick, tick);
        if (!added) {
            span->second.first  = std::min(span->second.first, tick);
            span->second.second = std::max(span->second.second, tick);
        }
    };
    auto const leave = [&](std::string const& uuid, int lastTick) {
        if (!inWorld.erase(uuid)) return;
        auto& span  = roster.spans[uuid];
        span.second = std::max({span.second, span.first, lastTick});
    };

    for (size_t chunk = 0; chunk < recording.chunkDimensions.size(); ++chunk) {
        int const start = recording.chunkStarts[chunk];
        int const end   = recording.chunkStarts[chunk + 1];
        // Each snapshot adds the players it has again.
        inWorld.clear();
        recording.readers[chunk]->scanActions([&](Action const& action, std::string_view bytes, int offset, bool) {
            int const tick = start + offset;
            if (action.name == ActionCreateLocalPlayer::getInstance().name) {
                auto const packet = decodeStandInPacket(MinecraftPacketIds::AddPlayer, bytes);
                if (!packet) return;
                auto const& add = static_cast<AddPlayerPacket const&>(*packet);
                if (*add.mUuid != mce::UUID{}) roster.recorderUuid = add.mUuid->asString();
                enter({}, tick);
                return;
            }
            if (action.name != ActionGamePacket::getInstance().name) return;
            auto const [id, payload] = splitGamePacket(bytes);
            switch (id) {
            case MinecraftPacketIds::PlayerList: {
                auto const packet = decodeStandInPacket(id, payload);
                if (!packet) return;
                for (auto const& entry : *static_cast<PlayerListPacket const&>(*packet).mEntries) {
                    auto const* added = std::get_if<PlayerListPacketPayload::AddEntry>(&entry);
                    if (!added || *added->mUUID == mce::UUID{}) continue;
                    auto uuid = added->mUUID->asString();
                    uuidByUniqueId.insert_or_assign(added->mId->rawID, uuid);
                    roster.names.insert_or_assign(uuid, *added->mName);
                    roster.playerLists.insert_or_assign(std::move(uuid), std::string(payload));
                }
                return;
            }
            case MinecraftPacketIds::AddPlayer: {
                auto const packet = decodeStandInPacket(id, payload);
                if (!packet) return;
                auto const& add      = static_cast<AddPlayerPacket const&>(*packet);
                auto const  uniqueId = add.mAbilitiesData->mTargetPlayer->rawID;
                std::string uuid;
                if (*add.mUuid != mce::UUID{}) {
                    uuid = add.mUuid->asString();
                } else if (auto const listed = uuidByUniqueId.find(uniqueId); listed != uuidByUniqueId.end()) {
                    uuid = listed->second;
                }
                if (uuid.empty()) return;
                uuidByUniqueId.insert_or_assign(uniqueId, uuid);
                roster.names.try_emplace(uuid, *add.mName);
                enter(uuid, tick);
                return;
            }
            case MinecraftPacketIds::RemoveActor: {
                auto const packet = decodeStandInPacket(id, payload);
                if (!packet) return;
                auto const uniqueId = static_cast<RemoveActorPacket const&>(*packet).mEntityId->rawID;
                if (uniqueId == record::RecordedPlayerUniqueIdValue) {
                    leave({}, tick - 1);
                } else if (auto const listed = uuidByUniqueId.find(uniqueId); listed != uuidByUniqueId.end()) {
                    leave(listed->second, tick - 1);
                }
                return;
            }
            default:
                return;
            }
        });
        for (auto const& uuid : std::vector(inWorld.begin(), inWorld.end())) leave(uuid, end - 1);
    }
    if (!roster.recorderUuid.empty()) {
        if (auto const recorder = roster.spans.find(std::string{}); recorder != roster.spans.end()) {
            roster.spans.insert_or_assign(roster.recorderUuid, recorder->second);
        }
    }
    return roster;
}

std::unique_ptr<StandInSource> buildStandInSource(
    StandInRecording const& recording,
    StandInRoster const&    roster,
    std::string const&      subject,
    std::string&            error
) {
    bool const recorder = subject.empty() || subject == roster.recorderUuid;
    int const  total    = recording.chunkStarts.empty() ? 0 : std::max(0, recording.chunkStarts.back());
    auto       source   = std::make_unique<StandInSource>();
    auto const named    = roster.names.find(recorder ? roster.recorderUuid : subject);
    source->label       = named != roster.names.end() && !named->second.empty() ? named->second
                        : recorder                                              ? std::string{"The recording player"}
                                                                                : subject;
    size_t const ticks  = static_cast<size_t>(total) + 1;
    source->poses.resize(ticks);
    source->present.assign(ticks, 0);
    source->dimensions.assign(ticks, 0);

    std::unordered_set<int64_t>  uniqueIds;
    std::unordered_set<uint64_t> runtimeIds;
    if (recorder) {
        uniqueIds.insert(record::RecordedPlayerUniqueIdValue);
        runtimeIds.insert(record::RecordedPlayerRuntimeIdValue);
    }
    // The unique ids the player list gives the subject, for AddPlayers recorded without a UUID.
    std::unordered_set<int64_t> listedIds;
    StandInPose                 pose;
    bool                        posed     = false;
    bool                        inWorld   = false;
    bool                        added     = false;
    int                         dimension = 0;
    int                         written   = -1;
    // The state at the end of each tick up to `last`, carried over ticks nothing changed.
    auto const writeThrough = [&](int last) {
        last = std::min(last, total);
        for (int tick = written + 1; tick <= last; ++tick) {
            auto const index           = static_cast<size_t>(tick);
            source->poses[index]       = pose;
            source->present[index]     = inWorld && posed ? 1 : 0;
            source->dimensions[index]  = dimension;
        }
        written = std::max(written, last);
    };
    auto const enter = [&](AddPlayerPacket const& add, std::string_view payload) {
        uniqueIds.insert(add.mAbilitiesData->mTargetPlayer->rawID);
        runtimeIds.insert(add.mRuntimeId->rawID);
        if (source->addPlayer.empty()) source->addPlayer = std::string(payload);
        pose    = addPlayerPose(add);
        posed   = true;
        inWorld = true;
        added   = true;
    };
    auto const withinReach = [&](BlockPos const& block) {
        float const dx = static_cast<float>(block.x) + 0.5f - pose.position.x;
        float const dy = static_cast<float>(block.y) + 0.5f - (pose.position.y - 0.9f);
        float const dz = static_cast<float>(block.z) + 0.5f - pose.position.z;
        return dx * dx + dy * dy + dz * dz <= StandInBlockReach * StandInBlockReach;
    };

    try {
        for (size_t chunk = 0; chunk < recording.chunkDimensions.size(); ++chunk) {
            int const start = recording.chunkStarts[chunk];
            int const end   = recording.chunkStarts[chunk + 1];
            writeThrough(start - 1);
            // Each snapshot adds the players it has again.
            inWorld   = false;
            dimension = recording.chunkDimensions[chunk];
            recording.readers[chunk]->scanActions([&](Action const& action, std::string_view bytes, int offset, bool) {
                int const tick = start + offset;
                writeThrough(tick - 1);
                if (action.name == ActionCreateLocalPlayer::getInstance().name) {
                    if (!recorder) return;
                    if (auto const packet = decodeStandInPacket(MinecraftPacketIds::AddPlayer, bytes)) {
                        enter(static_cast<AddPlayerPacket const&>(*packet), bytes);
                    }
                    return;
                }
                if (action.name == ActionMoveEntities::getInstance().name) {
                    if (uniqueIds.empty()) return;
                    forEachMoveSample(bytes, [&](int64_t id, StandInPose const& sample) {
                        if (!uniqueIds.contains(id)) return;
                        pose  = sample;
                        posed = true;
                    });
                    return;
                }
                if (action.name != ActionGamePacket::getInstance().name) return;
                auto const [id, payload] = splitGamePacket(bytes);
                switch (id) {
                case MinecraftPacketIds::PlayerList: {
                    if (recorder) return;
                    auto const packet = decodeStandInPacket(id, payload);
                    if (!packet) return;
                    for (auto const& entry : *static_cast<PlayerListPacket const&>(*packet).mEntries) {
                        auto const* listed = std::get_if<PlayerListPacketPayload::AddEntry>(&entry);
                        if (listed && listed->mUUID->asString() == subject) listedIds.insert(listed->mId->rawID);
                    }
                    return;
                }
                case MinecraftPacketIds::AddPlayer: {
                    // The recorder arrives by CreateLocalPlayer.
                    if (recorder) return;
                    auto const packet = decodeStandInPacket(id, payload);
                    if (!packet) return;
                    auto const& add = static_cast<AddPlayerPacket const&>(*packet);
                    bool const  ours =
                        add.mUuid->asString() == subject
                        || (*add.mUuid == mce::UUID{} && listedIds.contains(add.mAbilitiesData->mTargetPlayer->rawID));
                    if (ours) enter(add, payload);
                    return;
                }
                case MinecraftPacketIds::RemoveActor: {
                    if (!inWorld) return;
                    auto const packet = decodeStandInPacket(id, payload);
                    if (packet && uniqueIds.contains(static_cast<RemoveActorPacket const&>(*packet).mEntityId->rawID)) {
                        inWorld = false;
                    }
                    return;
                }
                case MinecraftPacketIds::ChangeDimension: {
                    if (auto const packet = decodeStandInPacket(id, payload)) {
                        dimension = static_cast<ChangeDimensionPacket const&>(*packet).mDimensionId->mValue;
                    }
                    return;
                }
                case MinecraftPacketIds::Animate:
                case MinecraftPacketIds::PlayerEquipment:
                case MinecraftPacketIds::MobArmorEquipment:
                case MinecraftPacketIds::SetActorData:
                case MinecraftPacketIds::ActorEvent:
                case MinecraftPacketIds::TriggerAnimation: {
                    if (runtimeIds.empty()) return;
                    auto const packet = decodeStandInPacket(id, payload);
                    if (!packet || !namesRuntimeId(*packet, runtimeIds)) return;
                    int detail = 0;
                    if (id == MinecraftPacketIds::SetActorData) {
                        detail = standInDataMask(static_cast<SetActorDataPacket const&>(*packet));
                        if (detail == 0) return;
                    } else if (id == MinecraftPacketIds::PlayerEquipment) {
                        detail = static_cast<int>(static_cast<MobEquipmentPacket const&>(*packet).mContainerId);
                    }
                    source->packets.push_back({tick, id, std::string(payload), detail});
                    return;
                }
                case MinecraftPacketIds::UpdateBlock:
                case MinecraftPacketIds::UpdateBlockSynced:
                case MinecraftPacketIds::UpdateSubChunkBlocks: {
                    if (!inWorld || !posed) return;
                    auto const packet = decodeStandInPacket(id, payload);
                    if (!packet) return;
                    forEachChangedBlock(*packet, [&](BlockPos const& pos, uint32_t layer, uint32_t runtimeId, uint8_t flags) {
                        if (withinReach(pos)) source->blocks.push_back({tick, pos, layer, runtimeId, flags});
                    });
                    return;
                }
                default:
                    return;
                }
            });
            writeThrough(end - 1);
        }
        writeThrough(total);
    } catch (std::exception const& e) {
        error = std::format("The recording could not be read for {}: {}", source->label, e.what());
        return nullptr;
    }

    if (!added) {
        error = std::format("The recording has no player {}.", source->label);
        return nullptr;
    }
    auto const first = std::ranges::find(source->present, uint8_t{1});
    if (first == source->present.end()) {
        error = std::format("{} is never in the world in this recording.", source->label);
        return nullptr;
    }
    source->firstTick = static_cast<int>(first - source->present.begin());
    source->lastTick  = static_cast<int>(source->present.rend() - std::ranges::find(source->present.rbegin(), source->present.rend(), uint8_t{1})) - 1;
    source->runtimeIds.assign(runtimeIds.begin(), runtimeIds.end());
    return source;
}

StandInPose placeStandInPose(StandInPose pose, StandInPlacement const& placement) {
    // Turned as yaw turns: a positive turn is clockwise seen from above, the way a player turns right.
    float const radians = placement.turn * std::numbers::pi_v<float> / 180.0f;
    float const cosine  = std::cos(radians);
    float const sine    = std::sin(radians);
    float const dx      = pose.position.x - placement.pivot.x;
    float const dz      = pose.position.z - placement.pivot.z;
    pose.position       = Vec3{
        placement.pivot.x + dx * cosine - dz * sine + placement.offset.x,
        pose.position.y + placement.offset.y,
        placement.pivot.z + dx * sine + dz * cosine + placement.offset.z,
    };
    pose.yaw     += placement.turn;
    pose.headYaw += placement.turn;
    pose.bodyYaw += placement.turn;
    return pose;
}

BlockPos placeStandInBlock(BlockPos const& pos, StandInPlacement const& placement) {
    // A block grid only turns by quarters; within a degree of one counts as it.
    float const quarters = placement.turn / 90.0f;
    float const nearest  = std::round(quarters);
    int const   quarter  = std::abs(quarters - nearest) * 90.0f <= 1.0f ? ((static_cast<int>(nearest) % 4) + 4) % 4 : 0;
    static constexpr int Cosine[4]{1, 0, -1, 0};
    static constexpr int Sine[4]{0, 1, 0, -1};
    float const          dx = static_cast<float>(pos.x) + 0.5f - placement.pivot.x;
    float const          dz = static_cast<float>(pos.z) + 0.5f - placement.pivot.z;
    float const          x  = placement.pivot.x + dx * Cosine[quarter] - dz * Sine[quarter] + placement.offset.x;
    float const          z  = placement.pivot.z + dx * Sine[quarter] + dz * Cosine[quarter] + placement.offset.z;
    float const          y  = static_cast<float>(pos.y) + 0.5f + placement.offset.y;
    return BlockPos{
        static_cast<int>(std::floor(x)),
        static_cast<int>(std::floor(y)),
        static_cast<int>(std::floor(z)),
    };
}

std::shared_ptr<Packet> retargetStandInPacket(
    StandInPacket const& recorded,
    StandInSource const& source,
    uint64_t             target,
    bool                 mob,
    bool                 showName
) {
    auto packet = decodeStandInPacket(recorded.id, recorded.payload);
    if (!packet) return nullptr;
    ActorRuntimeID const standIn{target};
    switch (recorded.id) {
    case MinecraftPacketIds::Animate:
        static_cast<AnimatePacket&>(*packet).mRuntimeId = standIn;
        break;
    case MinecraftPacketIds::PlayerEquipment:
        static_cast<MobEquipmentPacket&>(*packet).mRuntimeId = standIn;
        break;
    case MinecraftPacketIds::MobArmorEquipment:
        static_cast<MobArmorEquipmentPacket&>(*packet).mRuntimeId = standIn;
        break;
    case MinecraftPacketIds::ActorEvent:
        static_cast<ActorEventPacket&>(*packet).mRuntimeId = standIn;
        break;
    case MinecraftPacketIds::TriggerAnimation: {
        // An animation played on several actors at once plays on the Stand-in alone.
        auto& targets = *static_cast<AnimateEntityPacket&>(*packet).mRuntimeIds;
        std::erase_if(targets, [&](ActorRuntimeID id) {
            return std::ranges::find(source.runtimeIds, id.rawID) == source.runtimeIds.end();
        });
        if (targets.empty()) return nullptr;
        for (auto& id : targets) id = standIn;
        break;
    }
    case MinecraftPacketIds::SetActorData: {
        auto& data = static_cast<SetActorDataPacket&>(*packet);
        data.mId   = standIn;
        keepStandInData(*data.mPackedItems, mob, showName);
        if (data.mPackedItems->empty()) return nullptr;
        // A player's entity properties mean nothing on another entity type.
        if (mob) {
            data.mSynchedProperties->mIntEntries->clear();
            data.mSynchedProperties->mFloatEntries->clear();
        }
        break;
    }
    default:
        return nullptr;
    }
    return packet;
}

void keepStandInData(std::vector<std::unique_ptr<DataItem>>& items, bool mob, bool showName) {
    std::erase_if(items, [&](std::unique_ptr<DataItem> const& item) {
        if (!item || item->getId() > 0xFF) return true;
        int const bit = standInDataBit(item->getId());
        return bit == 0 || (mob && bit != StandInFlags && bit != StandInFlags2);
    });
    // Whether its name tag shows is the Stand-in's own: shown while it has one.
    constexpr int64_t NameFlags = (int64_t{1} << static_cast<int>(ActorFlags::CanShowName))
                                | (int64_t{1} << static_cast<int>(ActorFlags::AlwaysShowName));
    for (auto& item : items) {
        if (item->getId() != static_cast<ushort>(ActorDataIDs::Reserved0)) continue;
        if (auto flags = item->getData<int64>()) {
            int64_t const value = *flags;
            item->setData(static_cast<int64>(showName ? value | NameFlags : value & ~NameFlags));
        }
    }
}

std::unique_ptr<SerializedSkinRef> loadStandInSkin(std::string const& file, bool slim, std::string& error) {
    auto const*      begin = reinterpret_cast<char8_t const*>(file.data());
    StandInSkinImage image;
    if (!loadStandInSkinImage(std::filesystem::path(std::u8string(begin, begin + file.size())), image, error)) {
        return nullptr;
    }
    mce::Image skin(image.width, image.height, mce::ImageFormat::RGBA8Unorm, mce::ImageUsage::SRGB);
    skin.mImageBytes = mce::Blob(image.rgba.data(), image.rgba.size());
    mce::Image const  cape;
    ushort const      width    = static_cast<ushort>(image.width);
    ushort const      height   = static_cast<ushort>(image.height);
    ushort const      noCape   = 0;
    std::string const id       = std::format("playback.standin.{:016x}.{}", std::hash<std::string>{}(file), slim ? "slim" : "wide");
    // The built-in player models; their UVs fit 128x128 skins as well as 64x64 ones.
    std::string const geometry = slim ? "geometry.humanoid.customSlim" : "geometry.humanoid.custom";
    auto              ref      = std::make_unique<SerializedSkinRef>(
        id,
        std::format(R"({{"geometry":{{"default":"{}"}}}})", geometry),
        skin,
        width,
        height,
        cape,
        noCape,
        noCape,
        Json::Value{},
        MinEngineVersion::fromString("1.0.0"),
        std::string{},
        std::string{},
        true,
        false,
        false,
        false,
        false,
        false
    );
    if (!ref->isValid()) {
        error = "The skin image could not be made into a skin.";
        return nullptr;
    }
    ref->setArmSize(slim ? SharedTypes::persona::ArmSizeType::Slim : SharedTypes::persona::ArmSizeType::Wide);
    ref->setFullId(id);
    ref->setIsTrustedSkinFlag(TrustedSkinFlag::True);
    return ref;
}

} // namespace playback::replay
