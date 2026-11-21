#include "MimodelMesh.h"

#include "nlohmann/json.hpp"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>

// The format as Mine-imator reads it (GmProject/scripts/model_file_load*.gml, model_shape_generate_*.gml in
// github.com/stuffbydavid/Mine-imator):
// - The file's axes are Minecraft's: x east, y up, z south; 16 units to a block. Mine-imator swaps y and z on load
//   (value_get_point3D) to work Z-up internally, so the file needs no conversion to land in the Minecraft world.
// - Rotations are degrees, applied about the file's axes in the order z, then x, then y (v' = Ry * Rx * Rz * v,
//   counter-clockwise looking down each positive axis). That is matrix_build's transposed Y'X'Z' in Mine-imator's
//   swapped space; the swap is a reflection, which turns its clockwise angles into counter-clockwise ones here.
// - A part sits at `position` in its parent, turned by `rotation`. `scale` is not a transform: it multiplies down the
//   tree and scales the positions of child parts and shapes and the shapes' from/to boxes, component-wise.
// - A shape sits at `position` in its part, turned by `rotation`; its box is from - inflate .. to + inflate, scaled.
// - Box UVs: `uv` is the top-left corner of the south face (the front, +z) in texture_size units. Around it, in the
//   classic Minecraft unwrap, west (-x) is left of it, east (+x) and then north (-z) right of it, up (+y) above it and
//   down (-y) right of up. texture_mirror swaps east and west and flips every face horizontally.
// - Planes lie in x/y at from.z, front facing +z, UV'd like a block's south face. "3d" planes are one unit thick and
//   extruded per texel.
// - texture_size is made square (the larger side) and so is the image (padded right/down with transparency).
namespace playback::overlay::mimodel {

namespace {

using Microsoft::WRL::ComPtr;
using nlohmann::json;

constexpr uint32_t MaxTextureDimension = 8192;
constexpr uintmax_t MaxModelBytes      = 64ull * 1024 * 1024;
constexpr int      MaxPartDepth        = 64;
constexpr size_t   MaxFaces            = 1u << 20;
// Extruded planes sample one cell per texture unit; this caps a careless 4096-unit plane.
constexpr int      MaxPlaneCells       = 256;
// Texels below this alpha (of 255) are holes, matching the renderer's alpha test.
constexpr uint32_t OpaqueAlpha         = 26;

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

[[nodiscard]] bool decodeImage(std::filesystem::path const& file, Texture& out) {
    // An STA caller (the UI thread) keeps its apartment; WIC works in either.
    ComScope                   com;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return false;
    }
    ComPtr<IWICBitmapDecoder>     decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter>   converter;
    if (FAILED(factory->CreateDecoderFromFilename(
            file.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            &decoder
        ))
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
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 || width > MaxTextureDimension
        || height > MaxTextureDimension) {
        return false;
    }
    out.width  = width;
    out.height = height;
    out.texels.assign(static_cast<size_t>(width) * height, 0);
    if (FAILED(converter->CopyPixels(
            nullptr,
            width * 4,
            static_cast<UINT>(out.texels.size() * 4),
            reinterpret_cast<BYTE*>(out.texels.data())
        ))) {
        return false;
    }
    for (auto& texel : out.texels) {
        uint32_t const a = texel >> 24;
        if (a == 255) continue;
        if (a == 0) {
            texel = 0;
            continue;
        }
        auto const mul = [a](uint32_t c) { return (c * a + 127) / 255; };
        texel = mul(texel & 0xFF) | mul((texel >> 8) & 0xFF) << 8 | mul((texel >> 16) & 0xFF) << 16 | a << 24;
    }
    return true;
}

[[nodiscard]] std::filesystem::file_time_type stampOf(std::filesystem::path const& file) {
    std::error_code ec;
    auto const      stamp = std::filesystem::last_write_time(file, ec);
    return ec ? std::filesystem::file_time_type::min() : stamp;
}

[[nodiscard]] std::string toUtf8(std::filesystem::path const& file) {
    auto const text = file.u8string();
    return {reinterpret_cast<char const*>(text.data()), text.size()};
}

// Mine-imator reads numbers and flags alike as reals (JSON true is 1), and ignores anything malformed.
[[nodiscard]] std::optional<double> readNumber(json const& node, char const* key) {
    auto const it = node.find(key);
    if (it == node.end()) return std::nullopt;
    if (it->is_number()) return it->get<double>();
    if (it->is_boolean()) return it->get<bool>() ? 1.0 : 0.0;
    return std::nullopt;
}

[[nodiscard]] bool readFlag(json const& node, char const* key, bool fallback) {
    auto const value = readNumber(node, key);
    return value ? *value != 0.0 : fallback;
}

[[nodiscard]] std::optional<glm::dvec3> readVec3(json const& node, char const* key) {
    auto const it = node.find(key);
    if (it == node.end() || !it->is_array() || it->size() < 3) return std::nullopt;
    glm::dvec3 value{};
    for (glm::length_t i = 0; i < 3; ++i) {
        auto const& item = (*it)[static_cast<size_t>(i)];
        if (!item.is_number()) return std::nullopt;
        value[i] = item.get<double>();
    }
    return value;
}

[[nodiscard]] std::optional<glm::dvec2> readVec2(json const& node, char const* key) {
    auto const it = node.find(key);
    if (it == node.end() || !it->is_array() || it->size() < 2) return std::nullopt;
    auto const& x = (*it)[0];
    auto const& y = (*it)[1];
    if (!x.is_number() || !y.is_number()) return std::nullopt;
    return glm::dvec2{x.get<double>(), y.get<double>()};
}

// "#RRGGBB" (Mine-imator pads short strings with zeros).
[[nodiscard]] std::optional<glm::dvec3> readColour(json const& node, char const* key) {
    auto const it = node.find(key);
    if (it == node.end() || !it->is_string()) return std::nullopt;
    auto text = it->get<std::string>();
    if (!text.empty() && text.front() == '#') text.erase(0, 1);
    text.resize(6, '0');
    glm::dvec3 colour{};
    for (glm::length_t channel = 0; channel < 3; ++channel) {
        int value = 0;
        for (size_t i = 0; i < 2; ++i) {
            char const c     = text[static_cast<size_t>(channel) * 2 + i];
            int         digit = 0;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            value = value * 16 + digit;
        }
        colour[channel] = value / 255.0;
    }
    return colour;
}

[[nodiscard]] glm::dmat4 translation(glm::dvec3 const& offset) {
    glm::dmat4 matrix(1.0);
    matrix[3] = glm::dvec4(offset, 1.0);
    return matrix;
}

// v' = Ry(y) * Rx(x) * Rz(z) * v, degrees, counter-clockwise about each positive axis.
[[nodiscard]] glm::dmat4 rotation(glm::dvec3 const& degrees) {
    if (degrees == glm::dvec3(0.0)) return glm::dmat4(1.0);
    auto const       radians = degrees * (std::numbers::pi / 180.0);
    double const     cx = std::cos(radians.x), sx = std::sin(radians.x);
    double const     cy = std::cos(radians.y), sy = std::sin(radians.y);
    double const     cz = std::cos(radians.z), sz = std::sin(radians.z);
    glm::dmat3 const rx(1, 0, 0, 0, cx, sx, 0, -sx, cx);
    glm::dmat3 const ry(cy, 0, -sy, 0, 1, 0, sy, 0, cy);
    glm::dmat3 const rz(cz, sz, 0, -sz, cz, 0, 0, 0, 1);
    return glm::dmat4(ry * rx * rz);
}

struct TextureRef {
    int32_t index{-1};
    // Texture-size units to texels of the decoded image.
    double unitsToTexels{1.0};
};

struct Colour {
    glm::dvec3 blend{1.0};
    double     alpha{1.0};
    glm::dvec3 mix{0.0};
    double     mixAmount{};
    double     emissive{};
};

enum class Side : uint8_t { East, West, Up, Down, South, North };

struct SideCorners {
    Side               side;
    std::array<int, 3> normal;
    // 0 picks the box's low bound on that axis, 1 its high bound.
    std::array<std::array<int, 3>, 4> corners;
};

// Corners go around each face's perimeter, so (0, 1, 2) and (0, 2, 3) cover it.
constexpr std::array<SideCorners, 6> BoxSides{
    {{Side::East, {1, 0, 0}, {{{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}}},
     {Side::West, {-1, 0, 0}, {{{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}}},
     {Side::Up, {0, 1, 0}, {{{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}}},
     {Side::Down, {0, -1, 0}, {{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}}},
     {Side::South, {0, 0, 1}, {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}}},
     {Side::North, {0, 0, -1}, {{{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}}}}
};

// Where a box corner lands in the texture, in texture_size units: model_shape_generate_block's unwrap.
[[nodiscard]] glm::dvec2
boxUv(Side side, std::array<int, 3> const& corner, glm::dvec2 uv, glm::dvec3 const& size, bool mirror) {
    int ix = corner[0];
    if (mirror) {
        // Mirroring is the unwrap of the box reflected in x: east and west trade regions, every face flips.
        ix = 1 - ix;
        if (side == Side::East) side = Side::West;
        else if (side == Side::West) side = Side::East;
    }
    int const    iy    = corner[1];
    int const    iz    = corner[2];
    double const sideV = uv.y + (1 - iy) * size.y;
    switch (side) {
    case Side::South:
        return {uv.x + ix * size.x, sideV};
    case Side::North:
        return {uv.x + size.x + size.z + (1 - ix) * size.x, sideV};
    case Side::East:
        return {uv.x + size.x + (1 - iz) * size.z, sideV};
    case Side::West:
        return {uv.x - size.z + iz * size.z, sideV};
    case Side::Up:
        return {uv.x + ix * size.x, uv.y - size.z + iz * size.z};
    case Side::Down:
        return {uv.x + size.x + ix * size.x, uv.y - size.z + iz * size.z};
    }
    return uv;
}

class Builder {
public:
    Builder(std::filesystem::path directory, Model& model, bool floorBoxUvs)
    : mDirectory(std::move(directory)),
      mModel(model),
      mFloorBoxUvs(floorBoxUvs) {}

    // The texture a node names, or the one it inherits.
    [[nodiscard]] TextureRef texture(json const& node, TextureRef const& inherited) {
        auto const it = node.find("texture");
        if (it == node.end() || !it->is_string()) return inherited;
        TextureRef ref;
        ref.index = loadTexture(it->get<std::string>());
        if (ref.index >= 0) {
            auto const&  image = mModel.textures[static_cast<size_t>(ref.index)];
            double const side  = std::max(image.width, image.height);
            double       units = side;
            if (auto const size = readVec2(node, "texture_size")) units = std::max(size->x, size->y);
            if (units > 0.0) ref.unitsToTexels = side / units;
        }
        return ref;
    }

    void part(
        json const&       node,
        glm::dmat4 const& parentMatrix,
        glm::dvec3 const& parentScale,
        TextureRef const& parentTexture,
        Colour const&     parentColour,
        int               depth
    ) {
        if (!node.is_object() || depth > MaxPartDepth || !readFlag(node, "visible", true)) return;
        auto const position  = readVec3(node, "position").value_or(glm::dvec3(0.0)) * parentScale;
        auto const angles    = readVec3(node, "rotation").value_or(glm::dvec3(0.0));
        auto const scale     = readVec3(node, "scale").value_or(glm::dvec3(1.0)) * parentScale;
        auto const matrix    = parentMatrix * translation(position) * rotation(angles);
        auto const tex       = texture(node, parentTexture);
        auto const colour    = readColourOptions(node, parentColour);
        bool const backfaces = readFlag(node, "backfaces", false);

        if (auto const shapes = node.find("shapes"); shapes != node.end() && shapes->is_array()) {
            for (auto const& item : *shapes) shape(item, matrix, scale, tex, colour, backfaces);
        }
        if (auto const parts = node.find("parts"); parts != node.end() && parts->is_array()) {
            for (auto const& item : *parts) part(item, matrix, scale, tex, colour, depth + 1);
        }
    }

private:
    [[nodiscard]] int32_t loadTexture(std::string const& name) {
        if (auto const it = mTextureIndex.find(name); it != mTextureIndex.end()) return it->second;
        auto const file = mDirectory / pathFromUtf8(name);
        mModel.sources.emplace_back(file, stampOf(file));
        int32_t index = -1;
        Texture image;
        if (decodeImage(file, image)) {
            index = static_cast<int32_t>(mModel.textures.size());
            mModel.textures.push_back(std::move(image));
        }
        mTextureIndex.emplace(name, index);
        return index;
    }

    [[nodiscard]] static Colour readColourOptions(json const& node, Colour const& parent) {
        Colour colour;
        colour.blend     = readColour(node, "color_blend").value_or(glm::dvec3(1.0));
        colour.alpha     = readNumber(node, "color_alpha").value_or(1.0);
        colour.mix       = readColour(node, "color_mix").value_or(glm::dvec3(0.0));
        colour.mixAmount = readNumber(node, "color_mix_percent").value_or(0.0);
        // color_brightness is the older name.
        colour.emissive = readNumber(node, "color_emissive").value_or(readNumber(node, "color_brightness").value_or(0.0));
        if (readFlag(node, "color_inherit", true)) {
            colour.blend     *= parent.blend;
            colour.alpha     *= parent.alpha;
            colour.emissive   = std::clamp(colour.emissive + parent.emissive, 0.0, 1.0);
            colour.mix        = glm::clamp(colour.mix + parent.mix, 0.0, 1.0);
            colour.mixAmount  = std::clamp(colour.mixAmount + parent.mixAmount, 0.0, 1.0);
        }
        colour.alpha = std::clamp(colour.alpha, 0.0, 1.0);
        return colour;
    }

    void shape(
        json const&       node,
        glm::dmat4 const& partMatrix,
        glm::dvec3 const& partScale,
        TextureRef const& partTexture,
        Colour const&     partColour,
        bool              backfaces
    ) {
        if (!node.is_object() || !readFlag(node, "visible", true)) return;
        auto const type = node.find("type");
        if (type == node.end() || !type->is_string()) return;
        bool const block = *type == "block";
        bool const plane = *type == "plane";
        auto       from  = readVec3(node, "from");
        auto       to    = readVec3(node, "to");
        if ((!block && !plane) || !from || !to) return;

        auto const tex    = texture(node, partTexture);
        auto const colour = readColourOptions(node, partColour);
        auto       uv     = readVec2(node, "uv").value_or(glm::dvec2(0.0));
        bool const mirror = readFlag(node, "texture_mirror", false);
        bool const invert = readFlag(node, "invert", false);
        glm::dvec3 inflate(readNumber(node, "inflate").value_or(0.0));

        bool extruded = false;
        if (plane) {
            to->z    = from->z;
            extruded = readFlag(node, "3d", false);
            // Extruded planes are a unit thick and inflate every way; flat ones stay flat.
            if (extruded) to->z += 1.0;
            else inflate.z = 0.0;
        }

        auto const position = readVec3(node, "position").value_or(glm::dvec3(0.0)) * partScale;
        auto const angles   = readVec3(node, "rotation").value_or(glm::dvec3(0.0));
        auto const scale    = readVec3(node, "scale").value_or(glm::dvec3(1.0)) * partScale;
        auto const low      = (*from - inflate) * scale;
        auto const high     = (*to + inflate) * scale;

        // UVs follow the unscaled, uninflated size.
        auto size = *to - *from;
        if (mFloorBoxUvs) {
            size = glm::floor(size + 0.000001);
            uv   = glm::floor(uv);
        }

        Shape const target{
            partMatrix * translation(position) * rotation(angles),
            tex,
            colour,
            invert
        };
        if (block) {
            addBlock(target, low, high, uv, size, mirror, backfaces);
        } else if (extruded) {
            addExtrudedPlane(target, low, high, uv, size, mirror, backfaces);
        } else {
            bool hideBack = readFlag(node, "hide_back", false);
            if (auto const legacy = node.find("hide_backface"); legacy != node.end() && legacy->is_boolean()) {
                hideBack = legacy->get<bool>();
            }
            addPlane(target, low, high, uv, size, mirror, readFlag(node, "hide_front", false), hideBack);
        }
    }

    struct Shape {
        glm::dmat4 matrix;
        TextureRef texture;
        Colour     colour;
        bool       invert;
    };

    void emit(
        Shape const&                     shape,
        std::array<glm::dvec3, 4> const& corners,
        std::array<glm::dvec2, 4> const& uvUnits,
        glm::dvec3 const&                normal,
        bool                             twoSided
    ) {
        if (mModel.faces.size() >= MaxFaces) return;
        Face face;
        for (size_t i = 0; i < 4; ++i) face.position[i] = glm::vec3(shape.matrix * glm::dvec4(corners[i], 1.0));
        // Zero-area faces (flat blocks) have nothing to draw.
        auto const span = glm::cross(face.position[2] - face.position[0], face.position[3] - face.position[1]);
        if (glm::dot(span, span) < 1e-12f) return;

        face.normal = glm::vec3(glm::normalize(glm::dmat3(shape.matrix) * (shape.invert ? -normal : normal)));
        glm::dvec2 low(std::numeric_limits<double>::max());
        glm::dvec2 high(std::numeric_limits<double>::lowest());
        for (size_t i = 0; i < 4; ++i) {
            auto const texel = uvUnits[i] * shape.texture.unitsToTexels;
            face.uv[i]       = glm::vec2(texel);
            low              = glm::min(low, texel);
            high             = glm::max(high, texel);
        }
        auto const clampToInt = [](double value) { return static_cast<int32_t>(std::clamp(value, -1e9, 1e9)); };
        face.texelMinX        = clampToInt(std::floor(low.x + 1e-4));
        face.texelMinY        = clampToInt(std::floor(low.y + 1e-4));
        face.texelMaxX        = std::max(face.texelMinX, clampToInt(std::ceil(high.x - 1e-4)) - 1);
        face.texelMaxY        = std::max(face.texelMinY, clampToInt(std::ceil(high.y - 1e-4)) - 1);
        face.texture          = shape.texture.index;
        face.blend            = glm::vec3(shape.colour.blend);
        face.alpha            = static_cast<float>(shape.colour.alpha);
        face.mix              = glm::vec3(shape.colour.mix);
        face.mixAmount        = static_cast<float>(shape.colour.mixAmount);
        face.emissive         = static_cast<float>(shape.colour.emissive);
        face.twoSided         = twoSided;
        mModel.faces.push_back(face);
    }

    template <typename UvOf>
    void emitBox(
        Shape const&      shape,
        glm::dvec3 const& low,
        glm::dvec3 const& high,
        uint8_t           sideMask,
        bool              twoSided,
        UvOf const&       uvOf
    ) {
        // A negative scale turns the box inside out; its faces still point away from its middle.
        glm::dvec3 const outward(high.x >= low.x ? 1 : -1, high.y >= low.y ? 1 : -1, high.z >= low.z ? 1 : -1);
        for (size_t s = 0; s < BoxSides.size(); ++s) {
            if ((sideMask & (1u << s)) == 0) continue;
            auto const&               entry = BoxSides[s];
            std::array<glm::dvec3, 4> corners;
            std::array<glm::dvec2, 4> uvs;
            for (size_t i = 0; i < 4; ++i) {
                auto const& pick = entry.corners[i];
                corners[i] = {pick[0] ? high.x : low.x, pick[1] ? high.y : low.y, pick[2] ? high.z : low.z};
                uvs[i]     = uvOf(entry.side, pick);
            }
            glm::dvec3 const normal(entry.normal[0], entry.normal[1], entry.normal[2]);
            emit(shape, corners, uvs, normal * outward, twoSided);
        }
    }

    void addBlock(
        Shape const&      shape,
        glm::dvec3 const& low,
        glm::dvec3 const& high,
        glm::dvec2 const& uv,
        glm::dvec3 const& size,
        bool              mirror,
        bool              backfaces
    ) {
        emitBox(shape, low, high, 0x3F, backfaces, [&](Side side, std::array<int, 3> const& corner) {
            return boxUv(side, corner, uv, size, mirror);
        });
    }

    void addPlane(
        Shape const&      shape,
        glm::dvec3 const& low,
        glm::dvec3 const& high,
        glm::dvec2 const& uv,
        glm::dvec3 const& size,
        bool              mirror,
        bool              hideFront,
        bool              hideBack
    ) {
        if (hideFront && hideBack) return;
        std::array<glm::dvec3, 4> corners;
        std::array<glm::dvec2, 4> uvs;
        constexpr std::array<std::array<int, 2>, 4> Picks{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        for (size_t i = 0; i < 4; ++i) {
            int const ix = Picks[i][0];
            int const iy = Picks[i][1];
            corners[i]   = {ix ? high.x : low.x, iy ? high.y : low.y, low.z};
            uvs[i]       = {uv.x + (mirror ? 1 - ix : ix) * size.x, uv.y + (1 - iy) * size.y};
        }
        // Both sides show unless one is hidden; then only the other faces the camera.
        glm::dvec3 const front(0.0, 0.0, 1.0);
        emit(shape, corners, uvs, hideFront ? -front : front, !hideFront && !hideBack);
    }

    // One unit-thick voxel per opaque texel, sides only where the neighbour is a hole, every face in its texel's colour.
    void addExtrudedPlane(
        Shape const&      shape,
        glm::dvec3 const& low,
        glm::dvec3 const& high,
        glm::dvec2 const& uv,
        glm::dvec3 const& size,
        bool              mirror,
        bool              backfaces
    ) {
        int const columns = std::clamp(static_cast<int>(std::ceil(std::abs(size.x) - 1e-6)), 1, MaxPlaneCells);
        int const rows    = std::clamp(static_cast<int>(std::ceil(std::abs(size.y) - 1e-6)), 1, MaxPlaneCells);

        auto const texelCentre = [&](int column, int row) {
            int const across = mirror ? columns - 1 - column : column;
            return glm::dvec2(uv.x + across + 0.5, uv.y + row + 0.5);
        };
        Texture const* image = shape.texture.index >= 0 ? &mModel.textures[static_cast<size_t>(shape.texture.index)]
                                                        : nullptr;
        auto const opaque = [&](int column, int row) {
            if (column < 0 || row < 0 || column >= columns || row >= rows) return false;
            if (!image) return true;
            auto const texel = glm::floor(texelCentre(column, row) * shape.texture.unitsToTexels);
            if (texel.x < 0 || texel.y < 0 || texel.x >= image->width || texel.y >= image->height) return false;
            auto const index = static_cast<size_t>(texel.y) * image->width + static_cast<size_t>(texel.x);
            return (image->texels[index] >> 24) >= OpaqueAlpha;
        };

        auto const step = (high - low) / glm::dvec3(columns, rows, 1.0);
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column) {
                if (!opaque(column, row)) continue;
                // Bit order follows BoxSides: east, west, up, down, south, north. Row 0 is the plane's top.
                uint8_t mask = 0x30;
                if (!opaque(column + 1, row)) mask |= 0x01;
                if (!opaque(column - 1, row)) mask |= 0x02;
                if (!opaque(column, row - 1)) mask |= 0x04;
                if (!opaque(column, row + 1)) mask |= 0x08;
                glm::dvec3 const cellLow{low.x + step.x * column, high.y - step.y * (row + 1), low.z};
                glm::dvec3 const cellHigh{low.x + step.x * (column + 1), high.y - step.y * row, high.z};
                auto const       centre = texelCentre(column, row);
                emitBox(shape, cellLow, cellHigh, mask, backfaces, [&](Side, std::array<int, 3> const&) {
                    return centre;
                });
            }
        }
    }

    std::filesystem::path                    mDirectory;
    Model&                                   mModel;
    bool                                     mFloorBoxUvs;
    std::unordered_map<std::string, int32_t> mTextureIndex;
};

} // namespace

std::filesystem::path pathFromUtf8(std::string const& text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(text.data()), text.size()));
}

std::shared_ptr<Model const> loadModel(std::filesystem::path const& file, std::string& error) {
    error.clear();
    std::error_code ec;
    auto const      stamp = std::filesystem::last_write_time(file, ec);
    if (ec) {
        error = "Model file not found: " + toUtf8(file);
        return nullptr;
    }
    auto const bytes = std::filesystem::file_size(file, ec);
    if (ec || bytes > MaxModelBytes) {
        error = ec ? "Cannot read " + toUtf8(file) : "Model file is too large: " + toUtf8(file);
        return nullptr;
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "Cannot open " + toUtf8(file);
        return nullptr;
    }
    std::string const text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};

    json root;
    try {
        root = json::parse(text);
    } catch (json::exception const& e) {
        error = std::string("Not valid JSON: ") + e.what();
        return nullptr;
    }
    auto const parts = root.is_object() ? root.find("parts") : root.end();
    if (!root.is_object() || parts == root.end() || !parts->is_array()) {
        error = "Not a Mine-imator model: it has no \"parts\" list.";
        return nullptr;
    }

    auto model = std::make_shared<Model>();
    model->sources.emplace_back(file, stamp);
    Builder    builder(file.parent_path(), *model, readFlag(root, "floor_box_uvs", false));
    auto const rootTexture = builder.texture(root, TextureRef{});
    auto const rootScale   = readVec3(root, "scale").value_or(glm::dvec3(1.0));
    for (auto const& part : *parts) builder.part(part, glm::dmat4(1.0), rootScale, rootTexture, Colour{}, 0);

    if (model->faces.empty()) {
        error = "The model has no shapes to draw.";
        return nullptr;
    }
    return model;
}

} // namespace playback::overlay::mimodel
