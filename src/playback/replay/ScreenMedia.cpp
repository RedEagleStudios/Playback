#include "ScreenMedia.h"

#include "playback/Playback.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/deps/core/container/Blob.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/image/Image.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTexture.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/resources/ResourcePackManager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace playback::replay {

namespace {

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

constexpr std::array<ScreenType, 6> ScreenTypes{{
    {"re_furn:medium_stand_tv_entity", ScreenGroup::MediumTv, "mediumStandTv"},
    {"re_furn:medium_hanging_tv_entity", ScreenGroup::MediumTv, "mediumHangingTv"},
    {"re_furn:big_stand_tv_entity", ScreenGroup::BigTv, "bigStandTv"},
    {"re_furn:big_hanging_tv_entity", ScreenGroup::BigTv, "bigHangingTv"},
    {"re_furn:small_monitor_entity", ScreenGroup::Monitor, "monitor"},
    {"re_furn:wide_monitor_entity", ScreenGroup::WideMonitor, "wideMonitor"},
}};

// A rectangle of a texture in its model's UV units (the geometry's texture_width x texture_height).
struct UvRect {
    uint32_t u{};
    uint32_t v{};
    uint32_t width{};
    uint32_t height{};
};

// Where a group's screens show their picture, from the add-on's resource pack (render_controllers/re/furn/
// main.render_controller.json, models/entity/furn/re/gaming/{tv,monitor}/*.geo.json, textures/re/furn/...).
struct ScreenLayout {
    // By variant or skin_id. A TV's 0 is its 'off' picture, never replaced.
    std::array<char const*, 6> textures{};
    // TVs: one frame of the screen's animation strip; monitors: the whole texture.
    uint32_t                   uvWidth{};
    uint32_t                   uvHeight{};
    // Texels of the replaced texture per UV unit.
    uint32_t                   scale{};
    // TVs: the strip's frames, stacked top to bottom (uv_anim steps through them at 8 fps), all given the picture so it
    // stands still.
    uint32_t                   frames{};
    // Monitors: the screen glass, left to right as seen from the front; the picture spans them as one.
    std::array<UvRect, 4>      panels{};
    size_t                     panelCount{};
};

constexpr ScreenLayout MediumTvLayout{
    {nullptr,
     "textures/re/furn/sprite/med_01",
     "textures/re/furn/sprite/med_02",
     "textures/re/furn/sprite/med_03",
     "textures/re/furn/sprite/med_04",
     "textures/re/furn/sprite/med_05"},
    28,
    14,
    16,
    10,
    {},
    0,
};

constexpr ScreenLayout BigTvLayout{
    {nullptr,
     "textures/re/furn/sprite/big_01",
     "textures/re/furn/sprite/big_02",
     "textures/re/furn/sprite/big_03",
     "textures/re/furn/sprite/big_04",
     "textures/re/furn/sprite/big_05"},
    44,
    24,
    12,
    12,
    {},
    0,
};

// The north face of the monitor's front cube (16 x 10 at 0,0) is a one-texel bezel round a 14 x 8 screen.
constexpr ScreenLayout MonitorLayout{
    {"textures/re/furn/gaming/monitor_packet_black_blue",
     "textures/re/furn/gaming/monitor_packet_black_green",
     "textures/re/furn/gaming/monitor_packet_gold_rare",
     "textures/re/furn/gaming/monitor_packet_white_magenta",
     "textures/re/furn/gaming/monitor_packet_white_pink",
     nullptr},
    48,
    48,
    24,
    0,
    {{{1, 1, 14, 8}}},
    1,
};

// Four 9 x 14 panels curve round the viewer: the outer two (UV 0,14 left and 27,0 right) keep a bezel column on their
// outer edge, the inner two (0,0 and 18,0) meet with none; all four keep a bezel row above and below.
constexpr ScreenLayout WideMonitorLayout{
    {"textures/re/furn/gaming/wide_monitor_packet_black_blue",
     "textures/re/furn/gaming/wide_monitor_packet_black_green",
     "textures/re/furn/gaming/wide_monitor_packet_gold",
     "textures/re/furn/gaming/wide_monitor_packet_white_magenta",
     "textures/re/furn/gaming/wide_monitor_packet_white_pink",
     nullptr},
    64,
    64,
    16,
    0,
    {{{1, 15, 8, 12}, {0, 1, 9, 12}, {18, 1, 9, 12}, {27, 1, 8, 12}}},
    4,
};

ScreenLayout const& layoutOf(ScreenGroup group) {
    switch (group) {
    case ScreenGroup::MediumTv:
        return MediumTvLayout;
    case ScreenGroup::BigTv:
        return BigTvLayout;
    case ScreenGroup::Monitor:
        return MonitorLayout;
    case ScreenGroup::WideMonitor:
        break;
    }
    return WideMonitorLayout;
}

// Texture keys as the TextureGroup holds them, compared without case, extension or slash style.
[[nodiscard]] std::string normalizedTexturePath(std::string_view path) {
    std::string result(path);
    for (auto& character : result) {
        character = character == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (auto const dot = result.rfind('.'); dot != std::string::npos && result.find('/', dot) == std::string::npos) {
        result.erase(dot);
    }
    return result;
}

[[nodiscard]] std::shared_ptr<mce::TextureGroup> textureGroup() {
    auto client = ll::service::getClientInstance();
    if (!client) return nullptr;
    return client->getTextureGroup();
}

} // namespace

ScreenType const* screenTypeOf(std::string_view entityType) {
    for (auto const& type : ScreenTypes) {
        if (type.entity == entityType) return &type;
    }
    return nullptr;
}

std::string_view screenGroupKey(ScreenGroup group) {
    switch (group) {
    case ScreenGroup::MediumTv:
        return "mediumTv";
    case ScreenGroup::BigTv:
        return "bigTv";
    case ScreenGroup::Monitor:
        return "monitor";
    case ScreenGroup::WideMonitor:
        break;
    }
    return "wideMonitor";
}

std::span<int const> screenSlots(ScreenGroup group) {
    // 4 and 5 first: their textures have no PBR texture set beside them.
    static constexpr std::array<int, 5> TvSlots{5, 4, 3, 2, 1};
    static constexpr std::array<int, 5> MonitorSlots{0, 1, 2, 3, 4};
    return isTvGroup(group) ? std::span<int const>{TvSlots} : std::span<int const>{MonitorSlots};
}

std::pair<uint32_t, uint32_t> screenPictureSize(ScreenGroup group) {
    auto const& layout = layoutOf(group);
    if (layout.panelCount == 0) return {layout.uvWidth * layout.scale, layout.uvHeight * layout.scale};
    uint32_t width = 0;
    for (size_t index = 0; index < layout.panelCount; ++index) width += layout.panels[index].width;
    return {width * layout.scale, layout.panels[0].height * layout.scale};
}

struct ScreenTextures::Slot {
    ScreenGroup group{};
    std::string path;
    bool        prepared{};
    // The texture's own picture, straight RGBA, read from the resource pack.
    bool                 originalRead{};
    uint32_t             originalWidth{};
    uint32_t             originalHeight{};
    std::vector<uint8_t> original;
    // The format the game loaded the texture in, given to the replacements too.
    std::optional<std::pair<mce::TextureFormat, cg::ColorSpace>> format;
    // Monitors: the original scaled up to the replacement's size, which the picture is painted into.
    std::vector<uint8_t> base;
    uint32_t             width{};
    uint32_t             height{};
    std::vector<uint8_t> composed;
    // What the texture shows now.
    bool                            replaced{};
    std::weak_ptr<MediaFrame const> shownPicture;
    float                           shownBrightness{-1.0f};
    bool                            outcomeLogged{};
    // TV textures 1-3 have a PBR texture set: its MER map (metalness, emissive, roughness) is drawn from the channel's
    // own picture, so its outline would shade the new one. While replaced, it is flat: lit across the whole screen.
    bool                 merRead{};
    uint32_t             merWidth{};
    uint32_t             merHeight{};
    std::vector<uint8_t> merOriginal;
    bool                 merReplaced{};
    // Frames until the next look for a texture the game has not loaded.
    int lookDelay{};
};

ScreenTextures::ScreenTextures()  = default;
ScreenTextures::~ScreenTextures() = default;

ScreenTextures::Slot& ScreenTextures::slot(ScreenGroup group, int index) {
    auto& entry = mSlots[{group, index}];
    if (!entry) {
        entry        = std::make_unique<Slot>();
        entry->group = group;
        auto const* texture = layoutOf(group).textures[static_cast<size_t>(std::clamp(index, 0, 5))];
        entry->path         = texture ? texture : "";
    }
    return *entry;
}

namespace {

// Every key the TextureGroup holds for the texture: the actor renderer's, and any other file system's.
[[nodiscard]] std::vector<ResourceLocation const*> loadedKeys(mce::TextureGroup& textures, std::string const& path) {
    std::vector<ResourceLocation const*> keys;
    for (auto const& [location, texture] : *textures.mLoadedTextures) {
        auto const& value = location.mPath->value;
        if (value.size() < path.size() || value.find("furn") == std::string::npos) continue;
        if (normalizedTexturePath(value) == path) keys.push_back(&location);
    }
    return keys;
}

[[nodiscard]] cg::ImageBuffer makeImage(
    uint32_t                                                            width,
    uint32_t                                                            height,
    std::vector<uint8_t> const&                                         pixels,
    std::optional<std::pair<mce::TextureFormat, cg::ColorSpace>> const& format
) {
    mce::Image image(width, height, mce::ImageFormat::RGBA8Unorm, mce::ImageUsage::SRGB);
    // Pixels are RGBA; the game's actor textures take them as BGRA (seen in game: yellow came out cyan, red blue),
    // so red and blue are swapped on the way in.
    std::vector<uint8_t> swapped(pixels);
    for (size_t byte = 0; byte + 3 < swapped.size(); byte += 4) std::swap(swapped[byte], swapped[byte + 2]);
    image.mImageBytes = mce::Blob(swapped.data(), swapped.size());
    cg::ImageBuffer buffer(std::move(image));
    if (format) {
        buffer.mImageDescription->mTextureFormat = format->first;
        buffer.mImageDescription->mColorSpace    = format->second;
    }
    return buffer;
}

enum class UploadOutcome { NotLoaded, InPlace, Uploaded };

// Replaces the texture under every key it is loaded as: in place where the game takes it (same size), else as a new
// upload under the same key.
[[nodiscard]] UploadOutcome upload(
    std::string const&                                                  path,
    uint32_t                                                            width,
    uint32_t                                                            height,
    std::vector<uint8_t> const&                                         pixels,
    std::optional<std::pair<mce::TextureFormat, cg::ColorSpace>> const& format
) {
    auto textures = textureGroup();
    if (!textures) return UploadOutcome::NotLoaded;
    auto const keys = loadedKeys(*textures, path);
    if (keys.empty()) return UploadOutcome::NotLoaded;
    bool inPlace = true;
    for (auto const* key : keys) {
        if (!textures->isLoaded(*key, false, cg::TextureSetLayerType::Color)) return UploadOutcome::NotLoaded;
    }
    for (auto const* key : keys) {
        // A copy: uploading may add keys to the map the pointer is in.
        ResourceLocation const location(*key);
        if (textures->updateTextureInPlace(location, makeImage(width, height, pixels, format))) continue;
        inPlace = false;
        (void)textures->uploadTexture(location, makeImage(width, height, pixels, format));
    }
    return inPlace ? UploadOutcome::InPlace : UploadOutcome::Uploaded;
}

// Nearest-neighbour: the add-on's pixel art stays sharp round the picture.
void scaleNearest(
    std::vector<uint8_t> const& source,
    uint32_t                    sourceWidth,
    uint32_t                    sourceHeight,
    std::vector<uint8_t>&       target,
    uint32_t                    width,
    uint32_t                    height
) {
    target.resize(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t const sy  = static_cast<uint32_t>(static_cast<uint64_t>(y) * sourceHeight / height);
        auto const*    row = source.data() + static_cast<size_t>(sy) * sourceWidth * 4;
        auto*          out = target.data() + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t const sx = static_cast<uint32_t>(static_cast<uint64_t>(x) * sourceWidth / width);
            std::memcpy(out + static_cast<size_t>(x) * 4, row + static_cast<size_t>(sx) * 4, 4);
        }
    }
}

[[nodiscard]] std::array<uint8_t, 256> brightnessTable(float brightness) {
    float const                factor = std::max(0.0f, brightness) / 100.0f;
    std::array<uint8_t, 256>   table{};
    for (size_t value = 0; value < table.size(); ++value) {
        table[value] = static_cast<uint8_t>(std::min(255.0f, std::round(static_cast<float>(value) * factor)));
    }
    return table;
}

// The format the game loaded a texture in, so a replacement keeps its colour space (a MER map is linear).
[[nodiscard]] std::optional<std::pair<mce::TextureFormat, cg::ColorSpace>> merFormat(std::string const& path) {
    auto textures = textureGroup();
    if (!textures) return std::nullopt;
    for (auto const* key : loadedKeys(*textures, path)) {
        if (auto const* image = textures->getCachedImageOrLoadSync(*key, false)) {
            return std::pair{image->mImageDescription->mTextureFormat, image->mImageDescription->mColorSpace};
        }
    }
    return std::nullopt;
}

} // namespace

void ScreenTextures::prepare(ScreenGroup group, int index) {
    auto& entry = slot(group, index);
    if (entry.prepared || entry.path.empty()) return;
    entry.prepared = true;
    auto textures  = textureGroup();
    if (!textures) return;
    // Loads it (or queues the load) as the renderer would on first use.
    (void)textures->getTexture(
        ResourceLocation{Core::PathView{entry.path}},
        false,
        std::nullopt,
        cg::TextureSetLayerType::Color
    );
}

bool ScreenTextures::show(
    ScreenGroup                              group,
    int                                      index,
    std::shared_ptr<MediaFrame const> const& picture,
    float                                    brightness
) {
    auto& entry = slot(group, index);
    if (entry.path.empty() || !picture) return false;
    auto const [pictureWidth, pictureHeight] = screenPictureSize(group);
    if (picture->width != pictureWidth || picture->height != pictureHeight) {
        getLogger().debug("Screen picture {}x{} does not fit {}x{}", picture->width, picture->height, pictureWidth, pictureHeight);
        return false;
    }
    if (entry.replaced && entry.shownPicture.lock() == picture && entry.shownBrightness == brightness) return true;
    if (entry.lookDelay > 0) {
        --entry.lookDelay;
        return false;
    }
    prepare(group, index);

    auto const& layout = layoutOf(group);
    if (!entry.originalRead) {
        entry.originalRead = true;
        if (auto client = ll::service::getClientInstance()) {
            std::string bytes;
            if (client->getResourcePackManager().load(ResourceLocation{Core::PathView{entry.path + ".png"}}, bytes)) {
                (void)decodeImageBytes(
                    std::span{reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()},
                    entry.originalWidth,
                    entry.originalHeight,
                    entry.original
                );
            }
        }
        if (entry.original.empty()) {
            getLogger().warn("Unable to read the screen texture {}; it cannot be put back after a Screen layer", entry.path);
        }
    }
    if (!entry.format) {
        if (auto textures = textureGroup()) {
            for (auto const* key : loadedKeys(*textures, entry.path)) {
                if (auto const* image = textures->getCachedImageOrLoadSync(*key, false)) {
                    entry.format = std::pair{image->mImageDescription->mTextureFormat, image->mImageDescription->mColorSpace};
                    break;
                }
            }
        }
    }

    auto const table = brightnessTable(brightness);
    if (layout.panelCount == 0) {
        // TVs: the picture in every frame of the strip.
        entry.width  = pictureWidth;
        entry.height = pictureHeight * layout.frames;
        entry.composed.resize(static_cast<size_t>(entry.width) * entry.height * 4);
        size_t const frameBytes = picture->rgba.size();
        for (size_t byte = 0; byte < frameBytes; byte += 4) {
            entry.composed[byte]     = table[picture->rgba[byte]];
            entry.composed[byte + 1] = table[picture->rgba[byte + 1]];
            entry.composed[byte + 2] = table[picture->rgba[byte + 2]];
            entry.composed[byte + 3] = 255;
        }
        for (uint32_t frame = 1; frame < layout.frames; ++frame) {
            std::memcpy(entry.composed.data() + frame * frameBytes, entry.composed.data(), frameBytes);
        }
    } else {
        // Monitors: the bezel and the rest of the model from the original, the picture across the glass.
        if (entry.original.empty()) return false;
        entry.width  = layout.uvWidth * layout.scale;
        entry.height = layout.uvHeight * layout.scale;
        if (entry.base.empty()) {
            scaleNearest(entry.original, entry.originalWidth, entry.originalHeight, entry.base, entry.width, entry.height);
        }
        entry.composed = entry.base;
        uint32_t column = 0;
        for (size_t panelIndex = 0; panelIndex < layout.panelCount; ++panelIndex) {
            auto const& panel = layout.panels[panelIndex];
            for (uint32_t y = 0; y < panel.height * layout.scale; ++y) {
                auto const* in  = picture->rgba.data() + (static_cast<size_t>(y) * pictureWidth + column) * 4;
                auto*       out = entry.composed.data()
                          + ((static_cast<size_t>(panel.v * layout.scale + y)) * entry.width + panel.u * layout.scale) * 4;
                for (uint32_t x = 0; x < panel.width * layout.scale; ++x) {
                    // The glass keeps its own alpha: the add-on's emissive mask.
                    out[x * 4]     = table[in[x * 4]];
                    out[x * 4 + 1] = table[in[x * 4 + 1]];
                    out[x * 4 + 2] = table[in[x * 4 + 2]];
                }
            }
            column += panel.width * layout.scale;
        }
    }

    auto const outcome = upload(entry.path, entry.width, entry.height, entry.composed, entry.format);
    if (outcome == UploadOutcome::NotLoaded) {
        getLogger().debug("Screen texture {} is not loaded by the game yet", entry.path);
        entry.lookDelay = 10;
        return false;
    }
    if (!std::exchange(entry.outcomeLogged, true)) {
        getLogger().debug(
            "Screen texture {} replaced {} ({}x{})",
            entry.path,
            outcome == UploadOutcome::InPlace ? "in place" : "by a new upload",
            entry.width,
            entry.height
        );
    }
    entry.replaced        = true;
    entry.shownPicture    = picture;
    entry.shownBrightness = brightness;
    flattenMer(entry);
    return true;
}

void ScreenTextures::flattenMer(Slot& entry) {
    if (entry.merReplaced) return;
    std::string const merPath = entry.path + "_mer";
    if (!entry.merRead) {
        entry.merRead = true;
        if (auto client = ll::service::getClientInstance()) {
            std::string bytes;
            if (client->getResourcePackManager().load(ResourceLocation{Core::PathView{merPath + ".png"}}, bytes)) {
                (void)decodeImageBytes(
                    std::span{reinterpret_cast<uint8_t const*>(bytes.data()), bytes.size()},
                    entry.merWidth,
                    entry.merHeight,
                    entry.merOriginal
                );
            }
        }
    }
    if (entry.merOriginal.empty()) return;
    // The screen's brightest emissive and its usual roughness, over every texel; no metalness.
    uint8_t  emissive  = 0;
    uint64_t roughness = 0;
    size_t const texels = entry.merOriginal.size() / 4;
    for (size_t texel = 0; texel < texels; ++texel) {
        emissive   = std::max(emissive, entry.merOriginal[texel * 4 + 1]);
        roughness += entry.merOriginal[texel * 4 + 2];
    }
    std::vector<uint8_t> flat(entry.merOriginal.size());
    uint8_t const rough = texels ? static_cast<uint8_t>(roughness / texels) : 255;
    for (size_t texel = 0; texel < texels; ++texel) {
        flat[texel * 4]     = 0;
        flat[texel * 4 + 1] = emissive;
        flat[texel * 4 + 2] = rough;
        flat[texel * 4 + 3] = 255;
    }
    entry.merReplaced =
        upload(merPath, entry.merWidth, entry.merHeight, flat, merFormat(merPath)) != UploadOutcome::NotLoaded;
}

void ScreenTextures::restore(ScreenGroup group, int index) {
    auto found = mSlots.find({group, index});
    if (found == mSlots.end() || !found->second->replaced) return;
    auto& entry    = *found->second;
    entry.replaced = false;
    entry.shownPicture.reset();
    entry.shownBrightness = -1.0f;
    auto textures         = textureGroup();
    if (!textures) return;
    if (std::exchange(entry.merReplaced, false)) {
        std::string const merPath = entry.path + "_mer";
        (void)upload(merPath, entry.merWidth, entry.merHeight, entry.merOriginal, merFormat(merPath));
    }
    if (!entry.original.empty()) {
        if (upload(entry.path, entry.originalWidth, entry.originalHeight, entry.original, entry.format)
            != UploadOutcome::NotLoaded) {
            return;
        }
    }
    // Nothing to put back by hand: the game reads it from its pack again when next drawn.
    for (auto const* key : loadedKeys(*textures, entry.path)) {
        ResourceLocation const location(*key);
        (void)textures->unloadTexture(location, false);
    }
}

void ScreenTextures::restoreAll() {
    for (auto const& [key, entry] : mSlots) restore(key.first, key.second);
}

} // namespace playback::replay
