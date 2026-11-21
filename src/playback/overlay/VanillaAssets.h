#pragma once

#include "MimodelMesh.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Minecraft's own blocks and items as models for layers, built from the game's vanilla resource packs (blocks.json,
// terrain_texture.json, item_texture.json and their textures, packed in .brarchive files), layered in version order as
// the game stacks them. A model layer names one as its file: "minecraft:block/<name>" or "minecraft:item/<name>".
namespace playback::overlay::vanilla {

struct Asset {
    // "minecraft:block/stone", "minecraft:item/apple".
    std::string id;
    // "Stone", "Apple".
    std::string name;
    bool        block{};
};

// Every block and item that has a texture, blocks first, each sorted by name. Read once, on first use; empty when the
// game's resource packs cannot be read. Thread-safe.
[[nodiscard]] std::vector<Asset> const& catalog();

[[nodiscard]] bool isAsset(std::string_view file);

// The asset's readable name ("Oak Planks"), or the file as given when it is not one.
[[nodiscard]] std::string displayName(std::string_view file);

// A block as a full cube with its own face textures (the inventory look), standing on the origin; an item as its
// sprite extruded one unit thick, as a dropped item, standing on the origin and facing +z. 16 units to a block.
// Null with `error` set when the asset is unknown or its texture cannot be read. Thread-safe.
[[nodiscard]] std::shared_ptr<mimodel::Model const> loadModel(std::string_view file, std::string& error);

} // namespace playback::overlay::vanilla
