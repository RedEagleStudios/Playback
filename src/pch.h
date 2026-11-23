#pragma once

// Precompiled header, force-included into every source file of the playback target (set_pcxxheader in xmake.lua).
// Only stable third-party headers that most sources pull in belong here: a project header would rebuild everything
// whenever it changes. Sources still include what they use.

// LeviLamina defines empty stand-ins for these D3D12 types unless they are already macros; the sources that use them
// include the real ones from d3d12.h (see playback/editor/graphics/D3D12Compat.h), which now come after this header.
#define D3D12_RAYTRACING_GEOMETRY_DESC   D3D12_RAYTRACING_GEOMETRY_DESC
#define D3D12_FEATURE_DATA_ARCHITECTURE  D3D12_FEATURE_DATA_ARCHITECTURE
#define D3D12_FEATURE_DATA_D3D12_OPTIONS D3D12_FEATURE_DATA_D3D12_OPTIONS

#include "mc/_HeaderOutputPredefine.h" // the standard library, entt, glm, gsl, fmt, rapidjson and leveldb

#include "ll/api/i18n/I18n.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/Packet.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/Level.h"

#include "imgui.h"
