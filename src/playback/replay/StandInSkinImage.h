#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace playback::replay {

// A skin image as the game takes it: straight RGBA8, 64x64 or 128x128.
struct StandInSkinImage {
    uint32_t             width{};
    uint32_t             height{};
    std::vector<uint8_t> rgba;
};

// Reads a skin PNG of 64x64, 128x128 or the old 64x32 layout, which is widened to 64x64 with the left arm and leg
// mirrored from the right ones, as Java Edition does. False with `error` set (a sentence) when it is none of those.
[[nodiscard]] bool loadStandInSkinImage(std::filesystem::path const& file, StandInSkinImage& out, std::string& error);

} // namespace playback::replay
