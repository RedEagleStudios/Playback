#include "VanillaAssets.h"

#include "playback/Playback.h"

#include "nlohmann/json.hpp"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <system_error>
#include <unordered_map>

// The vanilla packs as the game ships them (data/resource_packs): "vanilla", then "vanilla_<version>" packs that each
// add or replace files, applied in version order. Most files sit in .brarchive files: "__brarchive/<dir>.brarchive"
// holds the files of <dir> (subdirectories have archives of their own). The format: a 16-byte header (magic u64,
// entry count u32, version u32), then one 256-byte entry per file (name length u8, name padded to 247 bytes, data
// offset u32, size u32), then the files' bytes, offsets counted from the end of the entries.
namespace playback::overlay::vanilla {
namespace {

using json = nlohmann::json;
using Microsoft::WRL::ComPtr;
using mimodel::Face;
using mimodel::Model;
using mimodel::Texture;

auto& logger() { return Playback::getInstance().getSelf().getLogger(); }

constexpr uint64_t ArchiveMagic     = 0x267052a0b125277dULL;
constexpr size_t   ArchiveEntrySize = 256;
constexpr size_t   ArchiveNameSize  = 247;
constexpr uint32_t MaxTextureSide   = 256;
constexpr uint8_t  OpaqueAlpha      = 128;

// Where one file's bytes are: a plain file (size 0, offset 0, whole file) or a stretch of an archive.
struct Source {
    std::filesystem::path file;
    uint64_t              offset{};
    uint64_t              size{};
    bool                  archived{};
};

struct Index {
    // Relative paths with forward slashes, as the packs' JSON names them; later packs replace earlier ones.
    std::unordered_map<std::string, Source> files;
    json                                    blocks = json::object();
    json                                    terrain = json::object();
    json                                    items   = json::object();
    std::vector<Asset>                      assets;
};

std::filesystem::path packsRoot() {
    std::array<wchar_t, MAX_PATH * 4> buffer{};
    DWORD const length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path() / "data" / "resource_packs";
}

// "vanilla" first, then "vanilla_1.14", "vanilla_1.21.100" ... by version; other packs are not vanilla layers.
std::optional<std::vector<int>> packVersion(std::string const& name) {
    if (name == "vanilla") return std::vector<int>{};
    if (!name.starts_with("vanilla_")) return std::nullopt;
    std::vector<int> version;
    char const*      cursor = name.data() + 8;
    char const*      end    = name.data() + name.size();
    while (cursor < end) {
        int value{};
        auto const [next, error] = std::from_chars(cursor, end, value);
        if (error != std::errc{}) return std::nullopt;
        version.push_back(value);
        cursor = next;
        if (cursor < end && *cursor != '.') return std::nullopt;
        if (cursor < end) ++cursor;
    }
    return version.empty() ? std::nullopt : std::optional{version};
}

std::string utf8(std::filesystem::path const& path) {
    auto const text = path.generic_u8string();
    return {reinterpret_cast<char const*>(text.data()), text.size()};
}

void indexArchive(std::filesystem::path const& archive, std::string const& directory, Index& index) {
    std::ifstream stream(archive, std::ios::binary);
    std::array<char, 16> header{};
    if (!stream.read(header.data(), header.size())) return;
    uint64_t magic{};
    uint32_t count{};
    std::memcpy(&magic, header.data(), 8);
    std::memcpy(&count, header.data() + 8, 4);
    if (magic != ArchiveMagic || count > 1'000'000) return;
    std::vector<char> entries(static_cast<size_t>(count) * ArchiveEntrySize);
    if (!stream.read(entries.data(), static_cast<std::streamsize>(entries.size()))) return;
    uint64_t const dataStart = 16 + entries.size();
    for (uint32_t i = 0; i < count; ++i) {
        char const* entry  = entries.data() + static_cast<size_t>(i) * ArchiveEntrySize;
        auto const  length = std::min<size_t>(static_cast<uint8_t>(entry[0]), ArchiveNameSize);
        uint32_t    offset{};
        uint32_t    size{};
        std::memcpy(&offset, entry + 1 + ArchiveNameSize, 4);
        std::memcpy(&size, entry + 1 + ArchiveNameSize + 4, 4);
        std::string const name(entry + 1, length);
        index.files[directory.empty() ? name : directory + "/" + name] = {archive, dataStart + offset, size, true};
    }
}

void indexPack(std::filesystem::path const& pack, Index& index) {
    std::error_code ec;
    auto const      archives = pack / "__brarchive";
    for (std::filesystem::recursive_directory_iterator it(pack, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        auto const& path = it->path();
        auto const  rel  = path.lexically_relative(pack);
        if (path.extension() == ".brarchive") {
            // __brarchive/textures/blocks.brarchive holds textures/blocks/*.
            auto inside = path.lexically_relative(archives);
            inside.replace_extension();
            indexArchive(path, utf8(inside) == "." ? std::string{} : utf8(inside), index);
        } else if (!rel.empty() && *rel.begin() != "__brarchive") {
            index.files[utf8(rel)] = {path, 0, 0, false};
        }
    }
}

std::optional<std::string> readFile(Index const& index, std::string const& name) {
    auto const found = index.files.find(name);
    if (found == index.files.end()) return std::nullopt;
    auto const& source = found->second;
    std::ifstream stream(source.file, std::ios::binary);
    if (!stream) return std::nullopt;
    if (!source.archived) return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    std::string bytes(static_cast<size_t>(source.size), '\0');
    stream.seekg(static_cast<std::streamoff>(source.offset));
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;
    return bytes;
}

// The packs' JSON may carry comments; a file that still fails to parse is skipped.
json parseJson(std::string const& text, std::string const& name) {
    auto parsed = json::parse(text, nullptr, false, true);
    if (parsed.is_discarded()) logger().warn("Vanilla assets: cannot parse {}", name);
    return parsed.is_object() ? parsed : json::object();
}

void mergeObject(json& into, json const& from) {
    if (!from.is_object()) return;
    for (auto const& [key, value] : from.items()) into[key] = value;
}

std::string readable(std::string const& key) {
    std::string name = key;
    std::replace(name.begin(), name.end(), '_', ' ');
    bool start = true;
    for (auto& c : name) {
        if (start && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        start = c == ' ';
    }
    return name;
}

// A blocks.json entry's texture name for one face (up, down, north, south, east, west); the inventory look
// (carried_textures) first.
std::optional<std::string> faceTexture(json const& block, char const* face) {
    for (char const* field : {"carried_textures", "textures"}) {
        auto const textures = block.find(field);
        if (textures == block.end()) continue;
        if (textures->is_string()) return textures->get<std::string>();
        if (!textures->is_object()) continue;
        bool const side = std::strcmp(face, "up") != 0 && std::strcmp(face, "down") != 0;
        for (char const* key : {face, side ? "side" : "", "side"}) {
            if (!*key) continue;
            if (auto const value = textures->find(key); value != textures->end() && value->is_string()) {
                return value->get<std::string>();
            }
        }
    }
    return std::nullopt;
}

struct TextureSpec {
    std::string                         path;
    std::optional<std::array<float, 3>> tint;
    // overlay_color: the texture's alpha marks where the colour goes (a grass side's grass), and the block is opaque.
    bool overlay{};
};

std::optional<std::array<float, 3>> hexColour(json const& node, char const* key) {
    auto const it = node.find(key);
    if (it == node.end() || !it->is_string()) return std::nullopt;
    auto text = it->get<std::string>();
    if (!text.empty() && text.front() == '#') text.erase(0, 1);
    if (text.size() < 6) return std::nullopt;
    std::array<float, 3> colour{};
    for (size_t channel = 0; channel < 3; ++channel) {
        int value{};
        if (std::from_chars(text.data() + channel * 2, text.data() + channel * 2 + 2, value, 16).ec != std::errc{}) {
            return std::nullopt;
        }
        colour[channel] = static_cast<float>(value) / 255.0f;
    }
    return colour;
}

// A texture_data entry: a path, {path, overlay_color | tint_color}, {variations: [...]}, or a list of those (the
// block's or item's variants by data value; `variant` picks one, the first when it is out of range).
std::optional<TextureSpec> textureSpec(json const& data, std::string const& name, size_t variant = 0) {
    auto const entry = data.find(name);
    if (entry == data.end() || !entry->is_object()) return std::nullopt;
    auto textures = entry->find("textures");
    if (textures == entry->end()) return std::nullopt;
    json const* node    = &*textures;
    bool        indexed = false;
    for (int depth = 0; depth < 4; ++depth) {
        if (node->is_array()) {
            if (node->empty()) return std::nullopt;
            node    = &(*node)[!indexed && variant < node->size() ? variant : 0];
            indexed = true;
        } else if (node->is_object() && node->contains("variations")) {
            node = &(*node)["variations"];
        } else {
            break;
        }
    }
    if (node->is_string()) return TextureSpec{node->get<std::string>(), std::nullopt};
    if (!node->is_object()) return std::nullopt;
    auto const path = node->find("path");
    if (path == node->end() || !path->is_string()) return std::nullopt;
    if (auto overlay = hexColour(*node, "overlay_color")) return TextureSpec{path->get<std::string>(), overlay, true};
    return TextureSpec{path->get<std::string>(), hexColour(*node, "tint_color")};
}

// The distinct texture paths of an entry's variants, in order; one entry for a texture without variants.
std::vector<std::string> variantPaths(json const& data, std::string const& name) {
    std::vector<std::string> paths;
    auto const               entry = data.find(name);
    if (entry == data.end() || !entry->is_object()) return paths;
    auto const textures = entry->find("textures");
    size_t const count  = textures != entry->end() && textures->is_array() ? textures->size() : 1;
    for (size_t variant = 0; variant < count; ++variant) {
        auto const spec = textureSpec(data, name, variant);
        paths.push_back(spec ? spec->path : std::string{});
    }
    return paths;
}

// Textures the game tints by biome carry no colour in the packs; they take the plains colours.
std::optional<std::array<float, 3>> biomeTint(std::string const& path) {
    auto const has = [&](char const* part) { return path.find(part) != std::string::npos; };
    if (has("leaves") || has("vine") || has("waterlily")) return std::array{0.28f, 0.71f, 0.09f};
    if (has("grass") || has("fern") || has("tallgrass") || has("reeds")) return std::array{0.47f, 0.75f, 0.35f};
    if (has("water")) return std::array{0.25f, 0.46f, 0.89f};
    return std::nullopt;
}

Index const& index() {
    static Index     shared;
    static std::once_flag once;
    std::call_once(once, [] {
        auto const root = packsRoot();
        std::vector<std::pair<std::vector<int>, std::filesystem::path>> packs;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            if (auto version = packVersion(utf8(it->path().filename()))) packs.emplace_back(std::move(*version), it->path());
        }
        std::ranges::sort(packs, {}, &decltype(packs)::value_type::first);
        for (auto const& [version, pack] : packs) {
            // Each pack's JSON adds to the ones before it, as its files replace theirs.
            Index layer;
            indexPack(pack, layer);
            for (auto& [name, source] : layer.files) shared.files[name] = source;
            auto const merge = [&](char const* name, json& into, char const* field) {
                if (!layer.files.contains(name)) return;
                if (auto const text = readFile(layer, name)) {
                    auto const parsed = parseJson(*text, name);
                    mergeObject(into, field ? parsed.value(field, json::object()) : parsed);
                }
            };
            merge("blocks.json", shared.blocks, nullptr);
            merge("textures/terrain_texture.json", shared.terrain, "texture_data");
            merge("textures/item_texture.json", shared.items, "texture_data");
        }
        if (packs.empty()) logger().warn("Vanilla assets: no vanilla resource packs under {}", utf8(root));

        // An entry whose texture lists variants (an item's "sword": wood, stone, iron...; the old "planks" block) is
        // one asset per distinct look, "<key>#<variant>", named after its texture file.
        auto const addVariants = [&](std::string const& prefix, std::string const& key, json const& data,
                                     std::string const& textureName, bool block) {
            auto const paths = variantPaths(data, textureName);
            std::vector<std::string> seen;
            for (size_t variant = 0; variant < paths.size(); ++variant) {
                auto const& path = paths[variant];
                if (path.empty() || std::ranges::find(seen, path) != seen.end()) continue;
                seen.push_back(path);
            }
            if (seen.size() <= 1) {
                if (!seen.empty()) shared.assets.push_back({prefix + key, readable(key), block});
                return;
            }
            seen.clear();
            for (size_t variant = 0; variant < paths.size(); ++variant) {
                auto const& path = paths[variant];
                if (path.empty() || std::ranges::find(seen, path) != seen.end()) continue;
                seen.push_back(path);
                auto const stem = path.substr(path.find_last_of('/') + 1);
                shared.assets.push_back({prefix + key + "#" + std::to_string(variant), readable(stem), block});
            }
        };
        for (auto const& [key, block] : shared.blocks.items()) {
            if (!block.is_object()) continue;
            auto const north = faceTexture(block, "north");
            if (!north || !textureSpec(shared.terrain, *north)) continue;
            // Variants only when every face shows the same texture entry.
            bool const uniform = faceTexture(block, "up") == north && faceTexture(block, "down") == north;
            if (uniform) addVariants("minecraft:block/", key, shared.terrain, *north, true);
            else shared.assets.push_back({"minecraft:block/" + key, readable(key), true});
        }
        for (auto const& [key, item] : shared.items.items()) {
            addVariants("minecraft:item/", key, shared.items, key, false);
        }
        std::ranges::sort(shared.assets, [](Asset const& a, Asset const& b) {
            return a.block != b.block ? a.block : a.name < b.name;
        });
        logger().debug("Vanilla assets: {} packs, {} blocks and items", packs.size(), shared.assets.size());
    });
    return shared;
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

class ComScope {
public:
    ComScope() : mNeedsUninit(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() {
        if (mNeedsUninit) CoUninitialize();
    }
    ComScope(ComScope const&)            = delete;
    ComScope& operator=(ComScope const&) = delete;

private:
    bool mNeedsUninit;
};

// Straight RGBA in, R in the low byte.
bool decodePng(std::string const& bytes, Texture& out) {
    ComScope                   com;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream>         stream;
    ComPtr<IWICBitmapDecoder>  decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter>   converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
        || FAILED(factory->CreateStream(&stream))
        || FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
            static_cast<DWORD>(bytes.size())
        ))
        || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
        || FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        ))) {
        return false;
    }
    UINT width{};
    UINT height{};
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 || width > 4096 || height > 4096) {
        return false;
    }
    out.width  = width;
    out.height = height;
    out.texels.assign(static_cast<size_t>(width) * height, 0);
    return SUCCEEDED(converter->CopyPixels(
        nullptr,
        width * 4,
        static_cast<UINT>(out.texels.size() * 4),
        reinterpret_cast<BYTE*>(out.texels.data())
    ));
}

// Uncompressed or run-length true-colour TGA (WIC has no TGA decoder), straight RGBA out.
bool decodeTga(std::string const& bytes, Texture& out) {
    if (bytes.size() < 18) return false;
    auto const u8     = [&](size_t at) { return static_cast<uint8_t>(bytes[at]); };
    uint8_t const idLength = u8(0);
    uint8_t const type     = u8(2);
    uint32_t const width   = u8(12) | u8(13) << 8;
    uint32_t const height  = u8(14) | u8(15) << 8;
    uint8_t const depth    = u8(16);
    bool const    topDown  = (u8(17) & 0x20) != 0;
    if ((type != 2 && type != 10) || (depth != 24 && depth != 32) || width == 0 || height == 0 || width > 4096
        || height > 4096 || u8(1) != 0) {
        return false;
    }
    size_t const pixelBytes = depth / 8;
    size_t       at         = 18 + idLength;
    std::vector<uint32_t> pixels;
    pixels.reserve(static_cast<size_t>(width) * height);
    auto const readPixel = [&]() -> std::optional<uint32_t> {
        if (at + pixelBytes > bytes.size()) return std::nullopt;
        uint32_t const b = u8(at), g = u8(at + 1), r = u8(at + 2);
        uint32_t const a = pixelBytes == 4 ? u8(at + 3) : 255;
        at += pixelBytes;
        return r | g << 8 | b << 16 | a << 24;
    };
    while (pixels.size() < static_cast<size_t>(width) * height) {
        if (type == 2) {
            auto const pixel = readPixel();
            if (!pixel) return false;
            pixels.push_back(*pixel);
            continue;
        }
        if (at >= bytes.size()) return false;
        uint8_t const packet = u8(at++);
        size_t const  count  = (packet & 0x7F) + 1;
        if (packet & 0x80) {
            auto const pixel = readPixel();
            if (!pixel) return false;
            for (size_t i = 0; i < count; ++i) pixels.push_back(*pixel);
        } else {
            for (size_t i = 0; i < count; ++i) {
                auto const pixel = readPixel();
                if (!pixel) return false;
                pixels.push_back(*pixel);
            }
        }
    }
    pixels.resize(static_cast<size_t>(width) * height);
    out.width  = width;
    out.height = height;
    out.texels.resize(pixels.size());
    for (uint32_t row = 0; row < height; ++row) {
        uint32_t const source = topDown ? row : height - 1 - row;
        std::copy_n(pixels.begin() + static_cast<ptrdiff_t>(source) * width, width, out.texels.begin() + static_cast<ptrdiff_t>(row) * width);
    }
    return true;
}

// The texture named in the packs (no extension), as the renderer takes it: the first frame of a flipbook strip,
// tinted where the game tints it (only its grey texels, so a grass side keeps its dirt), premultiplied.
std::optional<Texture> loadTexture(Index const& index, TextureSpec const& spec) {
    Texture image;
    bool    decoded = false;
    for (char const* extension : {".png", ".tga"}) {
        auto const bytes = readFile(index, spec.path + extension);
        if (!bytes) continue;
        decoded = std::strcmp(extension, ".png") == 0 ? decodePng(*bytes, image) : decodeTga(*bytes, image);
        if (decoded) break;
    }
    if (!decoded) return std::nullopt;
    if (image.height > image.width && image.height % image.width == 0) {
        image.texels.resize(static_cast<size_t>(image.width) * image.width);
        image.height = image.width;
    }
    if (image.width > MaxTextureSide || image.height > MaxTextureSide) return std::nullopt;

    auto const tint = spec.tint ? spec.tint : biomeTint(spec.path);
    for (auto& texel : image.texels) {
        uint32_t r = texel & 0xFF, g = (texel >> 8) & 0xFF, b = (texel >> 16) & 0xFF;
        uint32_t a = texel >> 24;
        // An overlay texture's alpha is the tint's mask, not transparency; elsewhere grey texels take the tint.
        bool const tinted = spec.overlay ? a > 0 : tint && std::max({r, g, b}) - std::min({r, g, b}) < 24;
        if (spec.overlay) a = 255;
        if (tint && tinted) {
            r = static_cast<uint32_t>(r * (*tint)[0] + 0.5f);
            g = static_cast<uint32_t>(g * (*tint)[1] + 0.5f);
            b = static_cast<uint32_t>(b * (*tint)[2] + 0.5f);
        }
        auto const mul = [a](uint32_t c) { return (c * a + 127) / 255; };
        texel = a == 0 ? 0 : mul(r) | mul(g) << 8 | mul(b) << 16 | a << 24;
    }
    return image;
}

// ---------------------------------------------------------------------------------------------------------------------
// Meshes

void addFace(
    Model&                          model,
    std::array<glm::vec3, 4> const& corners,
    std::array<glm::vec2, 4> const& uvs,
    glm::vec3 const&                normal,
    int32_t                         texture,
    glm::ivec4 const&               texels
) {
    Face face;
    face.position  = corners;
    face.uv        = uvs;
    face.normal    = normal;
    face.texture   = texture;
    face.texelMinX = texels.x;
    face.texelMinY = texels.y;
    face.texelMaxX = texels.z;
    face.texelMaxY = texels.w;
    model.faces.push_back(face);
}

// A full block from (-8, 0, -8) to (8, 16, 8); each face's texture upright as the game shows it.
void addCube(Model& model, std::array<int32_t, 6> const& textures) {
    // up, down, north (-z), south (+z), east (+x), west (-x): corners top-left, top-right, bottom-right, bottom-left.
    struct Side {
        std::array<glm::vec3, 4> corners;
        glm::vec3                normal;
    };
    std::array<Side, 6> const sides{{
        {{{{-8, 16, -8}, {8, 16, -8}, {8, 16, 8}, {-8, 16, 8}}}, {0, 1, 0}},
        {{{{-8, 0, 8}, {8, 0, 8}, {8, 0, -8}, {-8, 0, -8}}}, {0, -1, 0}},
        {{{{8, 16, -8}, {-8, 16, -8}, {-8, 0, -8}, {8, 0, -8}}}, {0, 0, -1}},
        {{{{-8, 16, 8}, {8, 16, 8}, {8, 0, 8}, {-8, 0, 8}}}, {0, 0, 1}},
        {{{{8, 16, 8}, {8, 16, -8}, {8, 0, -8}, {8, 0, 8}}}, {1, 0, 0}},
        {{{{-8, 16, -8}, {-8, 16, 8}, {-8, 0, 8}, {-8, 0, -8}}}, {-1, 0, 0}},
    }};
    for (size_t s = 0; s < sides.size(); ++s) {
        auto const& image = model.textures[static_cast<size_t>(textures[s])];
        auto const  w     = static_cast<float>(image.width);
        auto const  h     = static_cast<float>(image.height);
        addFace(
            model,
            sides[s].corners,
            {{{0, 0}, {w, 0}, {w, h}, {0, h}}},
            sides[s].normal,
            textures[s],
            {0, 0, static_cast<int>(image.width) - 1, static_cast<int>(image.height) - 1}
        );
    }
}

// The sprite across (-8, 0) .. (8, 16), one unit thick: front and back whole, and a side wherever an opaque texel
// borders a hole or the edge, in that texel's colour.
void addSprite(Model& model, int32_t texture) {
    auto const& image = model.textures[static_cast<size_t>(texture)];
    int const   w     = static_cast<int>(image.width);
    int const   h     = static_cast<int>(image.height);
    float const unitX = 16.0f / static_cast<float>(w);
    float const unitY = 16.0f / static_cast<float>(h);
    float const fw    = static_cast<float>(w);
    float const fh    = static_cast<float>(h);
    glm::ivec4 const whole{0, 0, w - 1, h - 1};
    addFace(model, {{{-8, 16, 0.5f}, {8, 16, 0.5f}, {8, 0, 0.5f}, {-8, 0, 0.5f}}}, {{{0, 0}, {fw, 0}, {fw, fh}, {0, fh}}}, {0, 0, 1}, texture, whole);
    addFace(model, {{{8, 16, -0.5f}, {-8, 16, -0.5f}, {-8, 0, -0.5f}, {8, 0, -0.5f}}}, {{{fw, 0}, {0, 0}, {0, fh}, {fw, fh}}}, {0, 0, -1}, texture, whole);

    auto const opaque = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h
            && (image.texels[static_cast<size_t>(y) * image.width + static_cast<size_t>(x)] >> 24) >= OpaqueAlpha;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!opaque(x, y)) continue;
            float const x0 = -8.0f + static_cast<float>(x) * unitX;
            float const x1 = x0 + unitX;
            float const y1 = 16.0f - static_cast<float>(y) * unitY;
            float const y0 = y1 - unitY;
            float const u  = static_cast<float>(x);
            float const v  = static_cast<float>(y);
            std::array<glm::vec2, 4> const uv{{{u, v}, {u + 1, v}, {u + 1, v + 1}, {u, v + 1}}};
            glm::ivec4 const               texel{x, y, x, y};
            if (!opaque(x - 1, y)) addFace(model, {{{x0, y1, -0.5f}, {x0, y1, 0.5f}, {x0, y0, 0.5f}, {x0, y0, -0.5f}}}, uv, {-1, 0, 0}, texture, texel);
            if (!opaque(x + 1, y)) addFace(model, {{{x1, y1, 0.5f}, {x1, y1, -0.5f}, {x1, y0, -0.5f}, {x1, y0, 0.5f}}}, uv, {1, 0, 0}, texture, texel);
            if (!opaque(x, y - 1)) addFace(model, {{{x0, y1, -0.5f}, {x1, y1, -0.5f}, {x1, y1, 0.5f}, {x0, y1, 0.5f}}}, uv, {0, 1, 0}, texture, texel);
            if (!opaque(x, y + 1)) addFace(model, {{{x0, y0, 0.5f}, {x1, y0, 0.5f}, {x1, y0, -0.5f}, {x0, y0, -0.5f}}}, uv, {0, -1, 0}, texture, texel);
        }
    }
}

} // namespace

std::vector<Asset> const& catalog() { return index().assets; }

bool isAsset(std::string_view file) { return file.starts_with("minecraft:"); }

std::string displayName(std::string_view file) {
    if (!isAsset(file)) return std::string(file);
    for (auto const& asset : catalog()) {
        if (asset.id == file) return asset.name;
    }
    auto const slash = file.find('/');
    return readable(std::string(slash == std::string_view::npos ? file.substr(10) : file.substr(slash + 1)));
}

std::shared_ptr<Model const> loadModel(std::string_view file, std::string& error) {
    error.clear();
    auto const& assets = index();
    bool const  block  = file.starts_with("minecraft:block/");
    bool const  item   = file.starts_with("minecraft:item/");
    // "<key>" or "<key>#<variant>".
    std::string key(file.substr(block ? 16 : item ? 15 : 0));
    size_t      variant = 0;
    if (auto const hash = key.find('#'); hash != std::string::npos) {
        std::from_chars(key.data() + hash + 1, key.data() + key.size(), variant);
        key.resize(hash);
    }
    if ((!block && !item) || key.empty()) {
        error = "Not a Minecraft block or item: " + std::string(file);
        return nullptr;
    }

    auto       model   = std::make_shared<Model>();
    // Textures by path, so faces sharing one (most blocks) decode it once.
    std::map<std::string, int32_t> loaded;
    auto const texture = [&](std::optional<TextureSpec> const& spec) -> std::optional<int32_t> {
        if (!spec) return std::nullopt;
        if (auto const it = loaded.find(spec->path); it != loaded.end()) return it->second;
        auto image = loadTexture(assets, *spec);
        if (!image) return std::nullopt;
        model->textures.push_back(std::move(*image));
        auto const at = static_cast<int32_t>(model->textures.size() - 1);
        loaded.emplace(spec->path, at);
        return at;
    };

    if (block) {
        auto const entry = assets.blocks.find(key);
        if (entry == assets.blocks.end() || !entry->is_object()) {
            error = "Minecraft has no block named " + key;
            return nullptr;
        }
        std::array<int32_t, 6> faces{};
        constexpr std::array<char const*, 6> Names{"up", "down", "north", "south", "east", "west"};
        for (size_t side = 0; side < Names.size(); ++side) {
            auto const name  = faceTexture(*entry, Names[side]);
            auto const index = texture(name ? textureSpec(assets.terrain, *name, variant) : std::nullopt);
            if (!index) {
                error = "The texture of " + key + " cannot be read from the game's packs.";
                return nullptr;
            }
            faces[side] = *index;
        }
        addCube(*model, faces);
    } else {
        auto const index = texture(textureSpec(assets.items, key, variant));
        if (!index) {
            error = "The texture of " + key + " cannot be read from the game's packs.";
            return nullptr;
        }
        addSprite(*model, *index);
    }
    if (model->faces.empty()) {
        error = key + " has nothing to draw.";
        return nullptr;
    }
    return model;
}

} // namespace playback::overlay::vanilla
