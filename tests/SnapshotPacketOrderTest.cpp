#include "playback/replay/SnapshotPacketOrder.h"

#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

enum class PacketId { AddPlayer, AddActor, SetActorLink, TriggerAnimation, MobEquipment };

using Packets = std::vector<std::pair<PacketId, std::string>>;

std::vector<std::string> payloads(Packets const& packets) {
    std::vector<std::string> result;
    for (auto const& [id, payload] : packets) result.push_back(payload);
    return result;
}

} // namespace

using playback::replay::orderSnapshotPackets;

TEST_CASE("snapshot packets: controller clips are applied after the actors and ride links") {
    Packets packets{
        {PacketId::AddPlayer,        "player"            },
        {PacketId::TriggerAnimation, "player ride clip"  },
        {PacketId::MobEquipment,     "player equipment"  },
        {PacketId::TriggerAnimation, "player hold clip"  },
        {PacketId::AddActor,         "bicycle"           },
        {PacketId::SetActorLink,     "player on bicycle" },
        {PacketId::TriggerAnimation, "bicycle wheel clip"},
    };
    orderSnapshotPackets(packets, PacketId::TriggerAnimation);

    CHECK(
        payloads(packets)
        == std::vector<std::string>{
            "player",
            "player equipment",
            "bicycle",
            "player on bicycle",
            "player ride clip",
            "player hold clip",
            "bicycle wheel clip",
        }
    );
}

TEST_CASE("snapshot packets: a snapshot without controller clips keeps its order") {
    Packets packets{
        {PacketId::AddActor,     "b"},
        {PacketId::AddPlayer,    "a"},
        {PacketId::SetActorLink, "c"},
    };
    orderSnapshotPackets(packets, PacketId::TriggerAnimation);

    CHECK(payloads(packets) == std::vector<std::string>{"b", "a", "c"});
}
