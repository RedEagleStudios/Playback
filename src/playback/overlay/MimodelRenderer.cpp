#include "MimodelRenderer.h"

#include "MimodelMesh.h"
#include "VanillaAssets.h"

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <limits>
#include <numbers>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace playback::overlay {

namespace {

using mimodel::Model;
using mimodel::Texture;

// ---------------------------------------------------------------------------------------------------------------------
// Cache

// The preview asks every frame; checking file times that often would be a stat storm for no benefit.
constexpr auto   RecheckInterval = std::chrono::milliseconds(500);
constexpr size_t MaxCachedModels = 16;

using Sources = std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>>;

struct CacheEntry {
    std::shared_ptr<Model const>          model;
    std::string                           error;
    Sources                               sources;
    std::chrono::steady_clock::time_point checkedAt;
    uint64_t                              lastUse{};
};

struct ModelCache {
    std::mutex                                  mutex;
    std::unordered_map<std::string, CacheEntry> entries;
    uint64_t                                    useCounter{};
};

[[nodiscard]] ModelCache& modelCache() {
    static ModelCache cache;
    return cache;
}

[[nodiscard]] std::filesystem::file_time_type stampOf(std::filesystem::path const& file) {
    std::error_code ec;
    auto const      stamp = std::filesystem::last_write_time(file, ec);
    return ec ? std::filesystem::file_time_type::min() : stamp;
}

[[nodiscard]] bool sourcesChanged(Sources const& sources) {
    return std::any_of(sources.begin(), sources.end(), [](auto const& source) {
        return stampOf(source.first) != source.second;
    });
}

// The model for `file`, parsing it (outside the lock, so a slow decode never stalls the other renderer) when it is new
// or one of its files changed. Failures are cached too, so a broken file is not re-read every frame.
[[nodiscard]] std::shared_ptr<Model const> acquireModel(std::string const& file, std::string& error) {
    error.clear();
    if (file.empty()) {
        error = "No model file chosen.";
        return nullptr;
    }
    auto&      cache = modelCache();
    auto const now   = std::chrono::steady_clock::now();
    Sources    known;
    {
        std::lock_guard lock(cache.mutex);
        if (auto const it = cache.entries.find(file); it != cache.entries.end()) {
            auto& entry   = it->second;
            entry.lastUse = ++cache.useCounter;
            // Minecraft's own blocks and items come from the installed game, which does not change while it runs.
            if (now - entry.checkedAt < RecheckInterval || vanilla::isAsset(file)) {
                error = entry.error;
                return entry.model;
            }
            known = entry.sources;
        }
    }
    if (!known.empty() && !sourcesChanged(known)) {
        std::lock_guard lock(cache.mutex);
        if (auto const it = cache.entries.find(file); it != cache.entries.end()) {
            it->second.checkedAt = now;
            error                = it->second.error;
            return it->second.model;
        }
    }

    CacheEntry fresh;
    auto const path = mimodel::pathFromUtf8(file);
    fresh.model     = vanilla::isAsset(file) ? vanilla::loadModel(file, fresh.error) : mimodel::loadModel(path, fresh.error);
    if (fresh.model) fresh.sources = fresh.model->sources;
    else if (!vanilla::isAsset(file)) fresh.sources = {{path, stampOf(path)}};
    fresh.checkedAt = now;

    std::lock_guard lock(cache.mutex);
    fresh.lastUse       = ++cache.useCounter;
    error               = fresh.error;
    auto model          = fresh.model;
    cache.entries[file] = std::move(fresh);
    if (cache.entries.size() > MaxCachedModels) {
        auto oldest = cache.entries.end();
        for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
            if (it->first != file && (oldest == cache.entries.end() || it->second.lastUse < oldest->second.lastUse)) {
                oldest = it;
            }
        }
        if (oldest != cache.entries.end()) cache.entries.erase(oldest);
    }
    return model;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rasterizer

// Texels with less alpha than this (of 255) are holes: Minecraft's cutout look.
constexpr uint32_t AlphaTest = 26;
// Missing textures draw opaque mid grey.
constexpr uint32_t GreyTexel = 0xFF808080u;

struct ClipVertex {
    double x, y, w;
    float  u, v;
};

struct ScreenVertex {
    float x, y;
    float invW, uOverW, vOverW;
};

// How a face turns a premultiplied texel into its colour: rgb = texel.rgb * colourScale + texel.a * mixScale,
// a = texel.a * alphaScale. Folds the face's blend, mix, alpha and lighting together.
struct FaceShading {
    Texture const* texture{};
    int32_t        minX{}, minY{}, maxX{}, maxY{};
    float          colourScale[3]{};
    float          mixScale[3]{};
    float          alphaScale{};
};

struct ScreenTriangle {
    std::array<ScreenVertex, 3> v;
    uint32_t                    shading;
};

// A clip-space half-space: inside where dot((x, y, w), normal) >= offset.
struct ClipPlane {
    double x, y, w, offset;

    [[nodiscard]] double distance(ClipVertex const& v) const { return x * v.x + y * v.y + w * v.w - offset; }
};

constexpr size_t MaxPolygon = 16;
using Polygon               = std::array<ClipVertex, MaxPolygon>;

// One Sutherland–Hodgman pass. Attributes interpolate linearly in clip space, which is what perspective needs.
[[nodiscard]] size_t clipPolygon(Polygon const& in, size_t count, ClipPlane const& plane, Polygon& out) {
    size_t written = 0;
    for (size_t i = 0; i < count; ++i) {
        auto const&  current  = in[i];
        auto const&  next     = in[(i + 1) % count];
        double const dCurrent = plane.distance(current);
        double const dNext    = plane.distance(next);
        if (dCurrent >= 0.0 && written < MaxPolygon) out[written++] = current;
        if ((dCurrent >= 0.0) != (dNext >= 0.0) && written < MaxPolygon) {
            double const t = dCurrent / (dCurrent - dNext);
            auto const   f = static_cast<float>(t);
            out[written++] = {
                current.x + (next.x - current.x) * t,
                current.y + (next.y - current.y) * t,
                current.w + (next.w - current.w) * t,
                current.u + (next.u - current.u) * f,
                current.v + (next.v - current.v) * f,
            };
        }
    }
    return written;
}

struct Scratch {
    std::vector<ScreenTriangle> triangles;
    std::vector<FaceShading>    shadings;
    // The model alone, premultiplied, over its screen bounds; composited onto the target with the opacity at the end
    // so a fading model does not show its own hidden faces.
    std::vector<uint32_t> colour;
    std::vector<float>    depth;
};

// Per thread: the preview and the export render at once, and the buffers are worth keeping between frames.
thread_local Scratch tScratch;

struct Bounds {
    int x0, y0, x1, y1; // pixels, x1/y1 exclusive

    [[nodiscard]] int width() const { return x1 - x0; }
    [[nodiscard]] int height() const { return y1 - y0; }
};

[[nodiscard]] uint32_t packRgba(float r, float g, float b, float a) {
    auto const byte = [](float value) { return static_cast<uint32_t>(std::clamp(value, 0.0f, 255.0f) + 0.5f); };
    return byte(r) | byte(g) << 8 | byte(b) << 16 | byte(a) << 24;
}

// Fills `triangle` into the scratch buffers, depth-tested on 1/w (larger is nearer). Pixel centres are sampled; edges
// shared by two triangles go to exactly one of them, so cutout texels on a quad's diagonal are not blended twice.
// Returns whether any pixel was written.
bool rasterizeTriangle(
    ScreenTriangle const& triangle,
    FaceShading const&    shading,
    Bounds const&         bounds,
    uint32_t*             colour,
    float*                depth
) {
    auto const& a    = triangle.v[0];
    auto        b    = triangle.v[1];
    auto        c    = triangle.v[2];
    double      area = (double(b.x) - a.x) * (double(c.y) - a.y) - (double(b.y) - a.y) * (double(c.x) - a.x);
    if (std::abs(area) < 1e-9) return false;
    if (area < 0.0) {
        std::swap(b, c);
        area = -area;
    }

    // Edge i is opposite vertex i: E(p) = A x + B y + C, positive inside.
    struct Edge {
        double a, b, c;
        bool   inclusive;
    };
    auto const makeEdge = [](ScreenVertex const& from, ScreenVertex const& to) {
        Edge edge;
        edge.a         = double(from.y) - to.y;
        edge.b         = double(to.x) - from.x;
        edge.c         = -(edge.a * from.x + edge.b * from.y);
        edge.inclusive = edge.a > 0.0 || (edge.a == 0.0 && edge.b > 0.0);
        return edge;
    };
    std::array<Edge, 3> const edges{makeEdge(b, c), makeEdge(c, a), makeEdge(a, b)};

    // Attribute planes: f(x, y) = f0 + dfdx (x - a.x) + dfdy (y - a.y), from the barycentric weights of b and c.
    auto const gradient = [&](float fa, float fb, float fc) {
        double const db = double(fb) - fa;
        double const dc = double(fc) - fa;
        return std::array<double, 2>{
            (db * edges[1].a + dc * edges[2].a) / area,
            (db * edges[1].b + dc * edges[2].b) / area,
        };
    };
    auto const gInvW = gradient(a.invW, b.invW, c.invW);
    auto const gU    = gradient(a.uOverW, b.uOverW, c.uOverW);
    auto const gV    = gradient(a.vOverW, b.vOverW, c.vOverW);

    double const minY = std::min({double(a.y), double(b.y), double(c.y)});
    double const maxY = std::max({double(a.y), double(b.y), double(c.y)});
    int const    rowStart = std::max(bounds.y0, static_cast<int>(std::ceil(minY - 0.5)));
    int const    rowEnd   = std::min(bounds.y1 - 1, static_cast<int>(std::floor(maxY - 0.5)));

    auto const&    texture = shading.texture;
    int const      stride  = bounds.width();
    bool           wrote   = false;
    constexpr auto Limit   = 1e7;
    for (int y = rowStart; y <= rowEnd; ++y) {
        double const py    = y + 0.5;
        int          first = bounds.x0;
        int          last  = bounds.x1 - 1;
        for (auto const& edge : edges) {
            double const k = edge.b * py + edge.c;
            if (edge.a == 0.0) {
                if (k < 0.0 || (k == 0.0 && !edge.inclusive)) first = last + 1;
                continue;
            }
            double const t = std::clamp(-k / edge.a, -Limit, Limit);
            if (edge.a > 0.0) {
                int x = static_cast<int>(std::ceil(t - 0.5));
                if (!edge.inclusive && x + 0.5 <= t) ++x;
                first = std::max(first, x);
            } else {
                int x = static_cast<int>(std::floor(t - 0.5));
                if (!edge.inclusive && x + 0.5 >= t) --x;
                last = std::min(last, x);
            }
        }
        if (first > last) continue;

        double const dx    = first + 0.5 - a.x;
        double const dy    = py - a.y;
        float        invW  = static_cast<float>(a.invW + gInvW[0] * dx + gInvW[1] * dy);
        float        uw    = static_cast<float>(a.uOverW + gU[0] * dx + gU[1] * dy);
        float        vw    = static_cast<float>(a.vOverW + gV[0] * dx + gV[1] * dy);
        auto const   dInvW = static_cast<float>(gInvW[0]);
        auto const   dU    = static_cast<float>(gU[0]);
        auto const   dV    = static_cast<float>(gV[0]);
        size_t       index = static_cast<size_t>(y - bounds.y0) * stride + static_cast<size_t>(first - bounds.x0);
        for (int x = first; x <= last; ++x, ++index, invW += dInvW, uw += dU, vw += dV) {
            if (invW <= depth[index]) continue;
            uint32_t texel = GreyTexel;
            if (texture) {
                float const w  = 1.0f / invW;
                int const   tx = std::clamp(static_cast<int>(std::floor(uw * w)), shading.minX, shading.maxX);
                int const   ty = std::clamp(static_cast<int>(std::floor(vw * w)), shading.minY, shading.maxY);
                if (tx < 0 || ty < 0 || static_cast<uint32_t>(tx) >= texture->width
                    || static_cast<uint32_t>(ty) >= texture->height) {
                    continue;
                }
                texel = texture->texels[static_cast<size_t>(ty) * texture->width + static_cast<size_t>(tx)];
            }
            uint32_t const texelAlpha = texel >> 24;
            if (texelAlpha < AlphaTest) continue;
            auto const  ta    = static_cast<float>(texelAlpha);
            float const alpha = ta * shading.alphaScale;
            if (alpha < 0.5f) continue;
            float red   = static_cast<float>(texel & 0xFF) * shading.colourScale[0] + ta * shading.mixScale[0];
            float green = static_cast<float>((texel >> 8) & 0xFF) * shading.colourScale[1] + ta * shading.mixScale[1];
            float blue  = static_cast<float>((texel >> 16) & 0xFF) * shading.colourScale[2] + ta * shading.mixScale[2];
            float cover = alpha;
            if (alpha < 254.5f) {
                uint32_t const below = colour[index];
                float const    keep  = 1.0f - alpha / 255.0f;
                red   += static_cast<float>(below & 0xFF) * keep;
                green += static_cast<float>((below >> 8) & 0xFF) * keep;
                blue  += static_cast<float>((below >> 16) & 0xFF) * keep;
                cover += static_cast<float>(below >> 24) * keep;
            }
            colour[index] = packRgba(red, green, blue, cover);
            depth[index]  = invW;
            wrote         = true;
        }
    }
    return wrote;
}

// Minecraft's directional shade: full on top, half underneath, 0.8 on z faces, 0.6 on x faces, blended by how much of
// the (world) normal points each way so rotated models shade smoothly.
[[nodiscard]] float minecraftShade(glm::dvec3 const& normal) {
    double const shade = normal.x * normal.x * 0.6 + normal.z * normal.z * 0.8
                       + normal.y * normal.y * (normal.y >= 0.0 ? 1.0 : 0.5);
    return static_cast<float>(shade);
}

// Model to world: scale (16 model units to a block), roll about z, pitch about x (positive tips the front, +z, down),
// yaw about y the way Minecraft's is (0 faces south, 90 west), then translate.
[[nodiscard]] glm::dmat3 placementRotation(state::editing::model::ModelPlacement const& placement) {
    constexpr double DegToRad = std::numbers::pi / 180.0;
    double const     yaw      = -placement.yaw * DegToRad;
    double const     pitch    = placement.pitch * DegToRad;
    double const     roll     = placement.roll * DegToRad;
    double const     cy = std::cos(yaw), sy = std::sin(yaw);
    double const     cx = std::cos(pitch), sx = std::sin(pitch);
    double const     cz = std::cos(roll), sz = std::sin(roll);
    glm::dmat3 const ry(cy, 0, -sy, 0, 1, 0, sy, 0, cy);
    glm::dmat3 const rx(1, 0, 0, 0, cx, sx, 0, -sx, cx);
    glm::dmat3 const rz(cz, sz, 0, -sz, cz, 0, 0, 0, 1);
    return ry * rx * rz;
}

// The camera position in model space: the point every clip row but z maps to 0 (x = y = w = 0). None for an
// orthographic projection.
[[nodiscard]] std::optional<glm::dvec3> eyeInModel(glm::dmat4 const& modelToClip) {
    auto const row = [&](int i) { return glm::dvec3(modelToClip[0][i], modelToClip[1][i], modelToClip[2][i]); };
    glm::dvec3 const r0 = row(0), r1 = row(1), r3 = row(3);
    double const     det   = glm::dot(r0, glm::cross(r1, r3));
    double const     scale = glm::length(r0) * glm::length(r1) * glm::length(r3);
    if (!(std::abs(det) > 1e-12 * scale)) return std::nullopt;
    glm::dvec3 const b(-modelToClip[3][0], -modelToClip[3][1], -modelToClip[3][3]);
    return (b.x * glm::cross(r1, r3) + b.y * glm::cross(r3, r0) + b.z * glm::cross(r0, r1)) / det;
}

} // namespace

bool renderMimodel(
    state::editing::model::ModelPlacement const& placement,
    OverlayCamera const&                         camera,
    float                                        opacity,
    OverlayImage&                                target,
    std::string*                                 error
) {
    std::string loadError;
    auto const  model = acquireModel(placement.file, loadError);
    if (error) *error = loadError;
    if (!model) return false;

    auto const width  = static_cast<int>(target.width);
    auto const height = static_cast<int>(target.height);
    opacity           = std::clamp(opacity, 0.0f, 1.0f);
    if (opacity <= 0.0f || width <= 0 || height <= 0 || target.rgba.size() < size_t(width) * size_t(height) * 4
        || !std::isfinite(placement.scale) || placement.scale == 0.0f) {
        return false;
    }
    double const spanX = double(camera.ndcRight) - camera.ndcLeft;
    double const spanY = double(camera.ndcBottom) - camera.ndcTop;
    if (std::abs(spanX) < 1e-6 || std::abs(spanY) < 1e-6) return false;

    double const     scale    = placement.scale / 16.0;
    glm::dmat3 const rotation = placementRotation(placement);
    glm::dmat4       modelToWorld(rotation * scale);
    modelToWorld[3] = glm::dvec4(placement.position.x, placement.position.y, placement.position.z, 1.0);
    glm::dmat4 const modelToClip = camera.viewProjection * modelToWorld;
    // Outward normals stay outward through a negative (mirroring) scale.
    glm::dmat3 const normalToWorld = scale < 0.0 ? -rotation : rotation;
    auto const       eye           = eyeInModel(modelToClip);

    // Clip to a guard band just past the image, so every projected coordinate stays small enough for float edge
    // functions however close to the near plane a vertex is.
    auto const ndcAtX = [&](double px) { return (px - camera.ndcLeft) / spanX * 2.0 - 1.0; };
    auto const ndcAtY = [&](double py) { return 1.0 - (py - camera.ndcTop) / spanY * 2.0; };
    double const guard = 1.05 * std::max({1.0, std::abs(ndcAtX(0)), std::abs(ndcAtX(width)), std::abs(ndcAtY(0)),
                                          std::abs(ndcAtY(height))});
    std::array<ClipPlane, 5> const planes{
        {{0, 0, 1, std::max(camera.nearClipW, 1e-6)},
         {1, 0, guard, 0},
         {-1, 0, guard, 0},
         {0, 1, guard, 0},
         {0, -1, guard, 0}}
    };

    auto& scratch = tScratch;
    scratch.triangles.clear();
    scratch.shadings.clear();
    float minX = std::numeric_limits<float>::max(), minY = minX;
    float maxX = std::numeric_limits<float>::lowest(), maxY = maxX;

    for (auto const& face : model->faces) {
        glm::dvec3 lightNormal(face.normal);
        if (eye) {
            double const side = glm::dot(lightNormal, *eye - glm::dvec3(face.position[0]));
            if (side <= 0.0) {
                if (!face.twoSided) continue;
                lightNormal = -lightNormal;
            }
        }

        Polygon polygon;
        size_t  count      = 4;
        uint8_t outside    = 0;
        uint8_t allOutside = 0x1F;
        for (size_t i = 0; i < 4; ++i) {
            auto const clip = modelToClip * glm::dvec4(glm::dvec3(face.position[i]), 1.0);
            polygon[i]      = {clip.x, clip.y, clip.w, face.uv[i].x, face.uv[i].y};
            uint8_t code    = 0;
            for (size_t p = 0; p < planes.size(); ++p) {
                if (planes[p].distance(polygon[i]) < 0.0) code |= uint8_t(1u << p);
            }
            outside    |= code;
            allOutside &= code;
        }
        if (allOutside) continue;
        if (outside) {
            Polygon other;
            for (size_t p = 0; p < planes.size() && count >= 3; ++p) {
                if ((outside & (1u << p)) == 0) continue;
                count = clipPolygon(polygon, count, planes[p], other);
                std::swap(polygon, other);
            }
            if (count < 3) continue;
        }

        float const shade = [&] {
            float const lit = minecraftShade(glm::normalize(normalToWorld * lightNormal));
            return lit + (1.0f - lit) * face.emissive;
        }();
        FaceShading shading;
        shading.texture = face.texture >= 0 ? &model->textures[static_cast<size_t>(face.texture)] : nullptr;
        shading.minX    = face.texelMinX;
        shading.minY    = face.texelMinY;
        shading.maxX    = face.texelMaxX;
        shading.maxY    = face.texelMaxY;
        for (glm::length_t ch = 0; ch < 3; ++ch) {
            shading.colourScale[ch] = face.blend[ch] * (1.0f - face.mixAmount) * shade * face.alpha;
            shading.mixScale[ch]    = face.mix[ch] * face.mixAmount * shade * face.alpha;
        }
        shading.alphaScale = face.alpha;
        auto const shadingIndex = static_cast<uint32_t>(scratch.shadings.size());
        scratch.shadings.push_back(shading);

        std::array<ScreenVertex, MaxPolygon> screen;
        for (size_t i = 0; i < count; ++i) {
            auto const&  v    = polygon[i];
            double const invW = 1.0 / v.w;
            auto&        s    = screen[i];
            s.x               = static_cast<float>(camera.ndcLeft + (v.x * invW + 1.0) * 0.5 * spanX);
            s.y               = static_cast<float>(camera.ndcTop + (1.0 - v.y * invW) * 0.5 * spanY);
            s.invW            = static_cast<float>(invW);
            s.uOverW          = static_cast<float>(v.u * invW);
            s.vOverW          = static_cast<float>(v.v * invW);
            minX              = std::min(minX, s.x);
            maxX              = std::max(maxX, s.x);
            minY              = std::min(minY, s.y);
            maxY              = std::max(maxY, s.y);
        }
        for (size_t i = 1; i + 1 < count; ++i) {
            scratch.triangles.push_back({{screen[0], screen[i], screen[i + 1]}, shadingIndex});
        }
    }
    if (scratch.triangles.empty()) return false;

    Bounds const bounds{
        std::max(0, static_cast<int>(std::floor(minX))),
        std::max(0, static_cast<int>(std::floor(minY))),
        std::min(width, static_cast<int>(std::ceil(maxX)) + 1),
        std::min(height, static_cast<int>(std::ceil(maxY)) + 1),
    };
    if (bounds.width() <= 0 || bounds.height() <= 0) return false;
    size_t const pixels = static_cast<size_t>(bounds.width()) * static_cast<size_t>(bounds.height());
    scratch.colour.assign(pixels, 0);
    scratch.depth.assign(pixels, 0.0f);

    bool drew = false;
    for (auto const& triangle : scratch.triangles) {
        drew |= rasterizeTriangle(
            triangle,
            scratch.shadings[triangle.shading],
            bounds,
            scratch.colour.data(),
            scratch.depth.data()
        );
    }
    if (!drew) return false;

    for (int y = bounds.y0; y < bounds.y1; ++y) {
        uint32_t const* source = scratch.colour.data() + static_cast<size_t>(y - bounds.y0) * bounds.width();
        uint8_t*        dest   = target.rgba.data() + (static_cast<size_t>(y) * width + bounds.x0) * 4;
        for (int x = bounds.x0; x < bounds.x1; ++x, ++source, dest += 4) {
            uint32_t const pixel = *source;
            if (pixel == 0) continue;
            if (opacity >= 1.0f && (pixel >> 24) == 255) {
                std::memcpy(dest, &pixel, 4);
                continue;
            }
            float const keep = 1.0f - static_cast<float>(pixel >> 24) * opacity / 255.0f;
            for (int ch = 0; ch < 4; ++ch) {
                float const value = static_cast<float>((pixel >> (ch * 8)) & 0xFF) * opacity + dest[ch] * keep;
                dest[ch]          = static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f) + 0.5f);
            }
        }
    }
    return true;
}

bool probeMimodel(std::string const& file, std::string& error) { return acquireModel(file, error) != nullptr; }

} // namespace playback::overlay
