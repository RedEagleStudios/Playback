#include "OverlayRenderer.h"

#include "MimodelRenderer.h"

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <utility>

namespace playback::overlay {

namespace {

using Microsoft::WRL::ComPtr;
using state::editing::model::Color4;
using state::editing::model::OverlayItem;
using state::editing::model::OverlayKind;
using state::editing::model::ShapeKind;
using state::editing::model::TextAlign;
using state::editing::model::TitleStyle;

constexpr float MinVisibleAlpha = 1.0f / 512.0f;
// Distance from the eye to the frame for tilted layers, in frame heights: far enough that 80 degrees of tilt still
// looks like a card turning rather than a fisheye.
constexpr double FocalDistance = 2.5;
constexpr float  MaxTiltDegrees = 80.0f;
constexpr float  MinTiltDegrees = 0.01f;
// tan() of the shear runs away past this.
constexpr float  MaxSkewDegrees = 85.0f;
// Antialiased edges spill past a geometry's exact bounds.
constexpr float  EdgeMargin     = 2.0f;
constexpr size_t MaxPooledSurfaces = 3;
// Keeps float-to-int pixel conversions defined for absurd placements.
constexpr float  CoordinateLimit   = 16777216.0f;

[[nodiscard]] float finiteOr(float value, float fallback) noexcept { return std::isfinite(value) ? value : fallback; }

[[nodiscard]] float unit(float value) noexcept { return std::clamp(finiteOr(value, 0.0f), 0.0f, 1.0f); }

[[nodiscard]] D2D1_COLOR_F toColor(Color4 const& color) noexcept {
    return D2D1::ColorF(unit(color.r), unit(color.g), unit(color.b), unit(color.a));
}

[[nodiscard]] std::wstring toWide(std::string const& text) {
    if (text.empty()) return {};
    int const length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

[[nodiscard]] std::string toUtf8(std::wstring const& text) {
    if (text.empty()) return {};
    int const length = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (length <= 0) return {};
    std::string narrow(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        narrow.data(),
        length,
        nullptr,
        nullptr
    );
    return narrow;
}

// ---------------------------------------------------------------------------------------------------------------------
// Factories

struct Graphics {
    ComPtr<ID2D1Factory>          d2d;
    ComPtr<IDWriteFactory>        dwrite;
    ComPtr<IWICImagingFactory>    wic;
    ComPtr<IDWriteFontCollection> fonts;
    // Title outlines: round joins and caps so the stroke hugs every glyph without spikes.
    ComPtr<ID2D1StrokeStyle> roundStroke;

    [[nodiscard]] bool ready() const noexcept { return d2d && dwrite && wic; }
};

[[nodiscard]] Graphics* createGraphics() {
    auto* graphics = new Graphics;

    D2D1_FACTORY_OPTIONS options{};
    options.debugLevel = D2D1_DEBUG_LEVEL_NONE;
    // Multithreaded: the preview and the export render at once, and D2D then guards the factory's shared state.
    D2D1CreateFactory(
        D2D1_FACTORY_TYPE_MULTI_THREADED,
        __uuidof(ID2D1Factory),
        &options,
        reinterpret_cast<void**>(graphics->d2d.GetAddressOf())
    );
    DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(graphics->dwrite.GetAddressOf())
    );

    // Only CoCreateInstance needs COM on this thread. RPC_E_CHANGED_MODE means the thread already lives in a
    // single-threaded apartment, which serves as well. The WIC factory is free-threaded and in-process, so the pointer
    // stays usable from any thread after this apartment is left.
    HRESULT const comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(graphics->wic.GetAddressOf())
    );
    if (SUCCEEDED(comInit)) CoUninitialize();

    if (graphics->dwrite) graphics->dwrite->GetSystemFontCollection(graphics->fonts.GetAddressOf(), FALSE);
    if (graphics->d2d) {
        graphics->d2d->CreateStrokeStyle(
            D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND,
                D2D1_CAP_STYLE_ROUND,
                D2D1_CAP_STYLE_ROUND,
                D2D1_LINE_JOIN_ROUND
            ),
            nullptr,
            0,
            graphics->roundStroke.GetAddressOf()
        );
    }
    return graphics;
}

// Never destroyed: tearing COM objects down while the module unloads is unsafe, and they live as long as the process.
[[nodiscard]] Graphics const& graphics() {
    static Graphics const* const instance = createGraphics();
    return *instance;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pixel rectangles

struct PixelRect {
    int x0{};
    int y0{};
    int x1{};
    int y1{};

    [[nodiscard]] bool empty() const noexcept { return x0 >= x1 || y0 >= y1; }
    [[nodiscard]] int  width() const noexcept { return x1 - x0; }
    [[nodiscard]] int  height() const noexcept { return y1 - y0; }
};

[[nodiscard]] PixelRect unite(PixelRect const& a, PixelRect const& b) noexcept {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

[[nodiscard]] PixelRect intersect(PixelRect const& a, PixelRect const& b) noexcept {
    PixelRect const r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    return r.empty() ? PixelRect{} : r;
}

// The whole pixels a float rectangle touches, grown by the antialiasing margin; empty for empty or invalid bounds.
[[nodiscard]] PixelRect outward(D2D1_RECT_F const& r) noexcept {
    if (!(r.right > r.left) || !(r.bottom > r.top)) return {};
    auto const down = [](float v) {
        return static_cast<int>(std::clamp(std::floor(v - EdgeMargin), -CoordinateLimit, CoordinateLimit));
    };
    auto const up = [](float v) {
        return static_cast<int>(std::clamp(std::ceil(v + EdgeMargin), -CoordinateLimit, CoordinateLimit));
    };
    return {down(r.left), down(r.top), up(r.right), up(r.bottom)};
}

[[nodiscard]] D2D1_RECT_F toRectF(PixelRect const& r) noexcept {
    return D2D1::RectF(
        static_cast<float>(r.x0),
        static_cast<float>(r.y0),
        static_cast<float>(r.x1),
        static_cast<float>(r.y1)
    );
}

// ---------------------------------------------------------------------------------------------------------------------
// Render surfaces

// A software D2D target over a WIC bitmap. Its pixels are transparent everywhere outside `stale`.
struct Surface {
    ComPtr<IWICBitmap>         bitmap;
    ComPtr<ID2D1RenderTarget> target;
    uint32_t                   width{};
    uint32_t                   height{};
    PixelRect                  stale;
};

[[nodiscard]] std::unique_ptr<Surface> createSurface(Graphics const& g, uint32_t width, uint32_t height) {
    auto surface = std::make_unique<Surface>();
    if (FAILED(g.wic->CreateBitmap(
            width,
            height,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad,
            surface->bitmap.GetAddressOf()
        ))) {
        return nullptr;
    }
    auto const properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f,
        96.0f
    );
    if (FAILED(g.d2d->CreateWicBitmapRenderTarget(surface->bitmap.Get(), properties, surface->target.GetAddressOf()))) {
        return nullptr;
    }
    surface->width  = width;
    surface->height = height;
    // WIC does not promise zeroed memory.
    surface->stale = {0, 0, static_cast<int>(width), static_cast<int>(height)};
    return surface;
}

// Surfaces outlive a call so an idle frame does not rebuild its render target. Each is leased to one thread at a time.
class SurfacePool {
public:
    [[nodiscard]] std::unique_ptr<Surface> acquire(Graphics const& g, uint32_t width, uint32_t height) {
        {
            std::lock_guard lock{mMutex};
            auto            best = mFree.end();
            for (auto it = mFree.begin(); it != mFree.end(); ++it) {
                Surface const& s = **it;
                if (s.width < width || s.height < height) continue;
                if (best == mFree.end()
                    || uint64_t{s.width} * s.height < uint64_t{(*best)->width} * (*best)->height) {
                    best = it;
                }
            }
            if (best != mFree.end()) {
                auto surface = std::move(*best);
                mFree.erase(best);
                return surface;
            }
        }
        return createSurface(g, width, height);
    }

    void release(std::unique_ptr<Surface> surface) {
        if (!surface) return;
        std::lock_guard lock{mMutex};
        mFree.push_back(std::move(surface));
        if (mFree.size() > MaxPooledSurfaces) mFree.erase(mFree.begin());
    }

private:
    std::mutex                            mMutex;
    std::vector<std::unique_ptr<Surface>> mFree;
};

[[nodiscard]] SurfacePool& surfacePool() {
    static auto* const pool = new SurfacePool;
    return *pool;
}

void beginDraw(Surface& surface) {
    surface.target->BeginDraw();
    surface.target->SetTransform(D2D1::Matrix3x2F::Identity());
    if (!surface.stale.empty()) {
        surface.target->PushAxisAlignedClip(toRectF(surface.stale), D2D1_ANTIALIAS_MODE_ALIASED);
        surface.target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        surface.target->PopAxisAlignedClip();
        surface.stale = {};
    }
}

// Calls fn(pixels, stride) with the surface's PBGRA pixels of `region`, its first pixel at the region's top-left.
template <typename Fn>
bool readPixels(Surface& surface, PixelRect const& region, Fn&& fn) {
    WICRect const rect{region.x0, region.y0, region.width(), region.height()};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(surface.bitmap->Lock(&rect, static_cast<DWORD>(WICBitmapLockRead), lock.GetAddressOf()))) return false;
    UINT             stride = 0;
    UINT             size   = 0;
    WICInProcPointer data   = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data)) || !data) return false;
    std::forward<Fn>(fn)(static_cast<uint8_t const*>(data), static_cast<uint32_t>(stride));
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pixel math

// x / 255 rounded, exact for x up to 255 * 255.
[[nodiscard]] inline uint32_t div255(uint32_t x) noexcept {
    x += 128;
    return (x + (x >> 8)) >> 8;
}

[[nodiscard]] inline uint8_t over(uint32_t source, uint32_t destination, uint32_t inverseAlpha) noexcept {
    return static_cast<uint8_t>(std::min<uint32_t>(255, source + div255(destination * inverseAlpha)));
}

// Source-over of premultiplied PBGRA rows onto the premultiplied RGBA image.
void blendPbgraOver(uint8_t const* source, uint32_t sourceStride, OverlayImage& out, PixelRect const& region) {
    size_t const outStride = static_cast<size_t>(out.width) * 4;
    for (int y = 0; y < region.height(); ++y) {
        uint8_t const* s = source + static_cast<size_t>(y) * sourceStride;
        uint8_t* d = out.rgba.data() + static_cast<size_t>(region.y0 + y) * outStride + static_cast<size_t>(region.x0) * 4;
        for (int x = 0; x < region.width(); ++x, s += 4, d += 4) {
            uint32_t const sa = s[3];
            if (sa == 0) continue;
            if (sa == 255 || d[3] == 0) {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = s[3];
                continue;
            }
            uint32_t const inverse = 255 - sa;
            d[0]                   = over(s[2], d[0], inverse);
            d[1]                   = over(s[1], d[1], inverse);
            d[2]                   = over(s[0], d[2], inverse);
            d[3]                   = over(sa, d[3], inverse);
        }
    }
}

struct SourceView {
    uint8_t const* data{};
    uint32_t       stride{};
    int            width{};
    int            height{};

    [[nodiscard]] uint32_t texel(int x, int y) const noexcept {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0;
        uint32_t value;
        std::memcpy(&value, data + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4, 4);
        return value;
    }
};

// Draws the flat layer in `source` (PBGRA; its pixel (0, 0) sits at frame pixel `origin`) onto `out` as if the layer
// were a card turned about `centre`: tiltX about the horizontal axis (top away), tiltY about the vertical axis (right
// side away), seen from FocalDistance frame heights. Each destination pixel is mapped back onto the card and sampled
// bilinearly. Returns whether any pixel was touched.
bool warpOver(
    OverlayImage&       out,
    SourceView const&   source,
    PixelRect const&    sourceRect,
    D2D1_POINT_2F       centre,
    float               tiltXDegrees,
    float               tiltYDegrees,
    float               alpha
) {
    constexpr double DegreesToRadians = std::numbers::pi / 180.0;
    double const     f                = FocalDistance * out.height;
    double const     tx               = tiltXDegrees * DegreesToRadians;
    double const     ty               = tiltYDegrees * DegreesToRadians;

    // The card's x and y axes after turning: A = Ry(ty) * Rx(tx) * (1, 0, 0), B = Ry(ty) * Rx(tx) * (0, 1, 0), with
    // +z pointing away from the viewer and y down.
    double const ax = std::cos(ty);
    double const ay = 0.0;
    double const az = std::sin(ty);
    double const bx = std::sin(tx) * std::sin(ty);
    double const by = std::cos(tx);
    double const bz = -std::sin(tx) * std::cos(ty);
    double const cx = centre.x;
    double const cy = centre.y;
    // Card points closer to the eye than this are dropped rather than blown up.
    double const minDepth = f * 0.02;

    // Homography from card (u, v, 1) to screen (x w, y w, w), relative to the centre.
    double const m00 = f * ax, m01 = f * bx, m02 = 0.0;
    double const m10 = f * ay, m11 = f * by, m12 = 0.0;
    double const m20 = az, m21 = bz, m22 = f;
    // Its adjugate maps the screen back to the card; the determinant cancels in the divide.
    double const a00 = m11 * m22 - m12 * m21, a01 = m02 * m21 - m01 * m22, a02 = m01 * m12 - m02 * m11;
    double const a10 = m12 * m20 - m10 * m22, a11 = m00 * m22 - m02 * m20, a12 = m02 * m10 - m00 * m12;
    double const a20 = m10 * m21 - m11 * m20, a21 = m01 * m20 - m00 * m21, a22 = m00 * m11 - m01 * m10;

    PixelRect const frame{0, 0, static_cast<int>(out.width), static_cast<int>(out.height)};
    PixelRect       dest = frame;
    {
        bool   behind = false;
        double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
        for (int corner = 0; corner < 4; ++corner) {
            double const u     = ((corner & 1) ? sourceRect.x1 : sourceRect.x0) - cx;
            double const v     = ((corner & 2) ? sourceRect.y1 : sourceRect.y0) - cy;
            double const depth = f + az * u + bz * v;
            if (depth <= minDepth) {
                behind = true;
                break;
            }
            double const sx = cx + f * (ax * u + bx * v) / depth;
            double const sy = cy + f * (ay * u + by * v) / depth;
            minX            = std::min(minX, sx);
            minY            = std::min(minY, sy);
            maxX            = std::max(maxX, sx);
            maxY            = std::max(maxY, sy);
        }
        if (!behind) {
            constexpr double Limit = CoordinateLimit;
            dest                   = intersect(
                PixelRect{
                    static_cast<int>(std::clamp(std::floor(minX) - 1.0, -Limit, Limit)),
                    static_cast<int>(std::clamp(std::floor(minY) - 1.0, -Limit, Limit)),
                    static_cast<int>(std::clamp(std::ceil(maxX) + 1.0, -Limit, Limit)),
                    static_cast<int>(std::clamp(std::ceil(maxY) + 1.0, -Limit, Limit)),
                },
                frame
            );
        }
    }
    if (dest.empty()) return false;

    uint32_t const alpha256  = static_cast<uint32_t>(std::clamp(std::lround(alpha * 256.0f), 0L, 256L));
    size_t const   outStride = static_cast<size_t>(out.width) * 4;
    // Source pixel coordinates (pixel centres at integers) of card point (u, v).
    double const offsetX = cx - sourceRect.x0 - 0.5;
    double const offsetY = cy - sourceRect.y0 - 0.5;
    bool         touched = false;

    for (int y = dest.y0; y < dest.y1; ++y) {
        double const dy  = y + 0.5 - cy;
        double const dx  = dest.x0 + 0.5 - cx;
        double       hu  = a00 * dx + a01 * dy + a02;
        double       hv  = a10 * dx + a11 * dy + a12;
        double       hw  = a20 * dx + a21 * dy + a22;
        uint8_t*     row = out.rgba.data() + static_cast<size_t>(y) * outStride;
        for (int x = dest.x0; x < dest.x1; ++x, hu += a00, hv += a10, hw += a20) {
            if (std::abs(hw) < 1e-12) continue;
            double const inverse = 1.0 / hw;
            double const u       = hu * inverse;
            double const v       = hv * inverse;
            if (f + az * u + bz * v <= minDepth) continue;

            float const sx = static_cast<float>(u + offsetX);
            float const sy = static_cast<float>(v + offsetY);
            if (!(sx > -1.0f && sy > -1.0f && sx < static_cast<float>(source.width)
                  && sy < static_cast<float>(source.height))) {
                continue;
            }
            float const    fx  = std::floor(sx);
            float const    fy  = std::floor(sy);
            int const      ix  = static_cast<int>(fx);
            int const      iy  = static_cast<int>(fy);
            uint32_t const t00 = source.texel(ix, iy);
            uint32_t const t10 = source.texel(ix + 1, iy);
            uint32_t const t01 = source.texel(ix, iy + 1);
            uint32_t const t11 = source.texel(ix + 1, iy + 1);
            if ((t00 | t10 | t01 | t11) == 0) continue;

            uint32_t const wx  = static_cast<uint32_t>((sx - fx) * 256.0f + 0.5f);
            uint32_t const wy  = static_cast<uint32_t>((sy - fy) * 256.0f + 0.5f);
            uint32_t const w00 = (256 - wx) * (256 - wy);
            uint32_t const w10 = wx * (256 - wy);
            uint32_t const w01 = (256 - wx) * wy;
            uint32_t const w11 = wx * wy;
            auto const     channel = [&](int shift) {
                uint32_t const mixed = (((t00 >> shift) & 255) * w00 + ((t10 >> shift) & 255) * w10
                                        + ((t01 >> shift) & 255) * w01 + ((t11 >> shift) & 255) * w11 + 32768)
                                    >> 16;
                return (mixed * alpha256 + 128) >> 8;
            };
            uint32_t const sa = channel(24);
            if (sa == 0) continue;
            uint32_t const sb = channel(0);
            uint32_t const sg = channel(8);
            uint32_t const sr = channel(16);

            uint8_t*       d       = row + static_cast<size_t>(x) * 4;
            uint32_t const inverse8 = 255 - sa;
            d[0]                    = over(sr, d[0], inverse8);
            d[1]                    = over(sg, d[1], inverse8);
            d[2]                    = over(sb, d[2], inverse8);
            d[3]                    = over(sa, d[3], inverse8);
            touched                 = true;
        }
    }
    return touched;
}

// ---------------------------------------------------------------------------------------------------------------------
// Title outlines

// Forwards a glyph run's outline into the title's path, moved to the run's baseline origin.
class OffsetSink final : public ID2D1SimplifiedGeometrySink {
public:
    OffsetSink(ID2D1SimplifiedGeometrySink* target, D2D1_POINT_2F offset) : mTarget(target), mOffset(offset) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1SimplifiedGeometrySink)) {
            *object = static_cast<ID2D1SimplifiedGeometrySink*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    // Lives on the stack for one call.
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

    // The title's path is winding throughout; a glyph's own fill mode would reset it mid-path.
    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) noexcept override {}
    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT flags) noexcept override {
        mTarget->SetSegmentFlags(flags);
    }
    void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F start, D2D1_FIGURE_BEGIN begin) noexcept override {
        mTarget->BeginFigure(shift(start), begin);
    }
    void STDMETHODCALLTYPE AddLines(D2D1_POINT_2F const* points, UINT32 count) noexcept override {
        D2D1_POINT_2F moved[64];
        while (count > 0) {
            UINT32 const chunk = std::min<UINT32>(count, 64);
            for (UINT32 i = 0; i < chunk; ++i) moved[i] = shift(points[i]);
            mTarget->AddLines(moved, chunk);
            points += chunk;
            count  -= chunk;
        }
    }
    void STDMETHODCALLTYPE AddBeziers(D2D1_BEZIER_SEGMENT const* beziers, UINT32 count) noexcept override {
        D2D1_BEZIER_SEGMENT moved[32];
        while (count > 0) {
            UINT32 const chunk = std::min<UINT32>(count, 32);
            for (UINT32 i = 0; i < chunk; ++i) {
                moved[i] = {shift(beziers[i].point1), shift(beziers[i].point2), shift(beziers[i].point3)};
            }
            mTarget->AddBeziers(moved, chunk);
            beziers += chunk;
            count   -= chunk;
        }
    }
    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END end) noexcept override { mTarget->EndFigure(end); }
    // The owner closes the real sink once every run is in.
    HRESULT STDMETHODCALLTYPE Close() noexcept override { return S_OK; }

private:
    [[nodiscard]] D2D1_POINT_2F shift(D2D1_POINT_2F p) const noexcept { return {p.x + mOffset.x, p.y + mOffset.y}; }

    ID2D1SimplifiedGeometrySink* mTarget;
    D2D1_POINT_2F                mOffset;
};

// Collects a text layout's glyphs as outlines instead of drawing them, so the outline can be stroked around them.
class GlyphOutlineRenderer final : public IDWriteTextRenderer {
public:
    explicit GlyphOutlineRenderer(ID2D1SimplifiedGeometrySink* sink) : mSink(sink) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping)
            || riid == __uuidof(IDWriteTextRenderer)) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

    // Unsnapped, so glyphs sit at the same fractions of the frame at every resolution.
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) noexcept override {
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) noexcept override {
        *transform = DWRITE_MATRIX{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixelsPerDip) noexcept override {
        *pixelsPerDip = 1.0f;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*,
        FLOAT                               baselineOriginX,
        FLOAT                               baselineOriginY,
        DWRITE_MEASURING_MODE,
        DWRITE_GLYPH_RUN const*             glyphRun,
        DWRITE_GLYPH_RUN_DESCRIPTION const*,
        IUnknown*
    ) noexcept override {
        if (!glyphRun || !glyphRun->fontFace || glyphRun->glyphCount == 0) return S_OK;
        OffsetSink sink{mSink, D2D1::Point2F(baselineOriginX, baselineOriginY)};
        return glyphRun->fontFace->GetGlyphRunOutline(
            glyphRun->fontEmSize,
            glyphRun->glyphIndices,
            glyphRun->glyphAdvances,
            glyphRun->glyphOffsets,
            glyphRun->glyphCount,
            glyphRun->isSideways,
            (glyphRun->bidiLevel & 1) != 0,
            &sink
        );
    }
    HRESULT STDMETHODCALLTYPE
    DrawUnderline(void*, FLOAT, FLOAT, DWRITE_UNDERLINE const*, IUnknown*) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    DrawStrikethrough(void*, FLOAT, FLOAT, DWRITE_STRIKETHROUGH const*, IUnknown*) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) noexcept override {
        return S_OK;
    }

private:
    ID2D1SimplifiedGeometrySink* mSink;
};

[[nodiscard]] std::wstring resolveFamily(Graphics const& g, std::string const& family) {
    std::wstring wide = toWide(family);
    if (!wide.empty() && g.fonts) {
        UINT32 index  = 0;
        BOOL   exists = FALSE;
        if (SUCCEEDED(g.fonts->FindFamilyName(wide.c_str(), &index, &exists)) && exists) return wide;
    }
    return L"Segoe UI";
}

// The title's glyph outlines in frame pixels, the text block centred on `centre`.
[[nodiscard]] ComPtr<ID2D1Geometry>
titleGeometry(Graphics const& g, TitleStyle const& style, D2D1_POINT_2F centre, float frameHeight) {
    std::wstring const text = toWide(style.text);
    float const        size = finiteOr(style.fontSize, 0.0f) * frameHeight;
    if (text.empty() || !(size > 0.01f)) return nullptr;

    ComPtr<IDWriteTextFormat> format;
    if (FAILED(g.dwrite->CreateTextFormat(
            resolveFamily(g, style.fontFamily).c_str(),
            nullptr,
            style.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size,
            L"en-us",
            format.GetAddressOf()
        ))) {
        return nullptr;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetTextAlignment(
        style.align == TextAlign::Left    ? DWRITE_TEXT_ALIGNMENT_LEADING
        : style.align == TextAlign::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                          : DWRITE_TEXT_ALIGNMENT_CENTER
    );

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(g.dwrite->CreateTextLayout(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            format.Get(),
            0.0f,
            0.0f,
            layout.GetAddressOf()
        ))) {
        return nullptr;
    }
    // Lines align within the widest one, so the block is as wide as its widest line.
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return nullptr;
    layout->SetMaxWidth(metrics.width);
    if (FAILED(layout->GetMetrics(&metrics))) return nullptr;
    float const originX = centre.x - (metrics.left + metrics.width * 0.5f);
    float const originY = centre.y - (metrics.top + metrics.height * 0.5f);

    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(g.d2d->CreatePathGeometry(path.GetAddressOf())) || FAILED(path->Open(sink.GetAddressOf()))) {
        return nullptr;
    }
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    GlyphOutlineRenderer renderer{sink.Get()};
    HRESULT const        drawn  = layout->Draw(nullptr, &renderer, originX, originY);
    HRESULT const        closed = sink->Close();
    if (FAILED(drawn) || FAILED(closed)) return nullptr;
    return path;
}

// ---------------------------------------------------------------------------------------------------------------------
// Layers

[[nodiscard]] ComPtr<ID2D1Geometry> shapeGeometry(Graphics const& g, OverlayItem const& item, D2D1_POINT_2F centre, float frameHeight) {
    float const width  = finiteOr(item.transform.width, 0.0f) * frameHeight;
    float const height = finiteOr(item.transform.height, 0.0f) * frameHeight;
    if (!(width > 0.0f) || !(height > 0.0f)) return nullptr;
    D2D1_RECT_F const box =
        D2D1::RectF(centre.x - width * 0.5f, centre.y - height * 0.5f, centre.x + width * 0.5f, centre.y + height * 0.5f);

    ComPtr<ID2D1Geometry> geometry;
    switch (item.shape.kind) {
    case ShapeKind::Rectangle: {
        float const radius = std::min({finiteOr(item.shape.cornerRadius, 0.0f) * frameHeight, width * 0.5f, height * 0.5f});
        if (radius > 0.0f) {
            ComPtr<ID2D1RoundedRectangleGeometry> rounded;
            if (FAILED(g.d2d->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(box, radius, radius),
                    rounded.GetAddressOf()
                ))) {
                return nullptr;
            }
            geometry = rounded;
        } else {
            ComPtr<ID2D1RectangleGeometry> rectangle;
            if (FAILED(g.d2d->CreateRectangleGeometry(box, rectangle.GetAddressOf()))) return nullptr;
            geometry = rectangle;
        }
        break;
    }
    case ShapeKind::Ellipse: {
        ComPtr<ID2D1EllipseGeometry> ellipse;
        if (FAILED(g.d2d->CreateEllipseGeometry(
                D2D1::Ellipse(centre, width * 0.5f, height * 0.5f),
                ellipse.GetAddressOf()
            ))) {
            return nullptr;
        }
        geometry = ellipse;
        break;
    }
    case ShapeKind::Polygon: {
        // The regular polygon's corners on the unit circle, first at the top, then stretched so they touch every side
        // of the box (a triangle fills its box rather than leaving a gap below).
        int const     count = std::clamp(item.shape.points, 3, 64);
        D2D1_POINT_2F corners[64];
        float         minX = 1.0f, minY = 1.0f, maxX = -1.0f, maxY = -1.0f;
        for (int i = 0; i < count; ++i) {
            double const angle = -std::numbers::pi / 2.0 + 2.0 * std::numbers::pi * i / count;
            corners[i]         = D2D1::Point2F(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
            minX               = std::min(minX, corners[i].x);
            minY               = std::min(minY, corners[i].y);
            maxX               = std::max(maxX, corners[i].x);
            maxY               = std::max(maxY, corners[i].y);
        }
        for (int i = 0; i < count; ++i) {
            corners[i].x = box.left + (corners[i].x - minX) / (maxX - minX) * width;
            corners[i].y = box.top + (corners[i].y - minY) / (maxY - minY) * height;
        }
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(g.d2d->CreatePathGeometry(path.GetAddressOf())) || FAILED(path->Open(sink.GetAddressOf()))) {
            return nullptr;
        }
        sink->BeginFigure(corners[0], D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLines(corners + 1, static_cast<UINT32>(count - 1));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) return nullptr;
        geometry = path;
        break;
    }
    }
    return geometry;
}

// A title or shape ready to draw: its outline in frame pixels and how to place and paint it.
struct Layer {
    ComPtr<ID2D1Geometry> geometry;
    // Skew, then rotation, about the centre.
    D2D1::Matrix3x2F  placement = D2D1::Matrix3x2F::Identity();
    D2D1_POINT_2F     centre{};
    D2D1_COLOR_F      fill{};
    D2D1_COLOR_F      stroke{};
    float             strokeWidth{};
    ID2D1StrokeStyle* strokeStyle{};
    bool              filled{};
    bool              stroked{};
    // Frame pixels covered once placed, stroke and antialiasing included.
    PixelRect bounds;
    float     tiltX{};
    float     tiltY{};

    [[nodiscard]] bool tilted() const noexcept {
        return std::abs(tiltX) > MinTiltDegrees || std::abs(tiltY) > MinTiltDegrees;
    }
};

[[nodiscard]] std::optional<Layer>
prepareLayer(Graphics const& g, OverlayItem const& item, float frameWidth, float frameHeight) {
    auto const& transform = item.transform;
    Layer       layer;
    layer.centre = D2D1::Point2F(finiteOr(transform.x, 0.5f) * frameWidth, finiteOr(transform.y, 0.5f) * frameHeight);

    if (item.kind == OverlayKind::Title) {
        auto const& style = item.title;
        layer.geometry    = titleGeometry(g, style, layer.centre, frameHeight);
        layer.fill        = toColor(style.color);
        layer.filled      = layer.fill.a > 0.0f;
        // Stroked at twice the width under the fill, so only the outer half shows.
        layer.strokeWidth = 2.0f * finiteOr(style.outlineWidth, 0.0f) * frameHeight;
        layer.stroke      = toColor(style.outlineColor);
        layer.stroked     = layer.strokeWidth > 0.0f && layer.stroke.a > 0.0f;
        layer.strokeStyle = g.roundStroke.Get();
    } else {
        auto const& style = item.shape;
        layer.geometry    = shapeGeometry(g, item, layer.centre, frameHeight);
        layer.fill        = toColor(style.fill);
        layer.filled      = layer.fill.a > 0.0f;
        layer.strokeWidth = finiteOr(style.strokeWidth, 0.0f) * frameHeight;
        layer.stroke      = toColor(style.stroke);
        layer.stroked     = layer.strokeWidth > 0.0f && layer.stroke.a > 0.0f;
    }
    if (!layer.geometry || (!layer.filled && !layer.stroked)) return std::nullopt;

    float const skewX = std::clamp(finiteOr(transform.skewX, 0.0f), -MaxSkewDegrees, MaxSkewDegrees);
    float const skewY = std::clamp(finiteOr(transform.skewY, 0.0f), -MaxSkewDegrees, MaxSkewDegrees);
    // D2D's x skew leans the top left for positive angles; the model's leans it right.
    layer.placement = D2D1::Matrix3x2F::Skew(-skewX, skewY, layer.centre)
                    * D2D1::Matrix3x2F::Rotation(finiteOr(transform.rotation, 0.0f), layer.centre);
    layer.tiltX = std::clamp(finiteOr(transform.tiltX, 0.0f), -MaxTiltDegrees, MaxTiltDegrees);
    layer.tiltY = std::clamp(finiteOr(transform.tiltY, 0.0f), -MaxTiltDegrees, MaxTiltDegrees);

    D2D1_RECT_F bounds{};
    HRESULT const measured = layer.stroked ? layer.geometry->GetWidenedBounds(
                                                 layer.strokeWidth,
                                                 layer.strokeStyle,
                                                 &layer.placement,
                                                 D2D1_DEFAULT_FLATTENING_TOLERANCE,
                                                 &bounds
                                             )
                                           : layer.geometry->GetBounds(&layer.placement, &bounds);
    if (FAILED(measured)) return std::nullopt;
    layer.bounds = outward(bounds);
    if (layer.bounds.empty()) return std::nullopt;
    return layer;
}

// Paints `layer` onto `target`, whose pixels relate to the frame's by `toTarget`. `area` is where it lands on the
// target. With both a fill and a stroke the layer fades as one, so its outline does not show through a faded fill.
void drawLayer(
    ID2D1RenderTarget&       target,
    Layer const&             layer,
    D2D1::Matrix3x2F const&  toTarget,
    float                    alpha,
    PixelRect const&         area
) {
    bool const          grouped = alpha < 1.0f && layer.filled && layer.stroked;
    ComPtr<ID2D1Layer>  group;
    if (grouped) {
        if (FAILED(target.CreateLayer(group.GetAddressOf()))) group.Reset();
        if (group) {
            target.SetTransform(D2D1::Matrix3x2F::Identity());
            target.PushLayer(
                D2D1::LayerParameters(
                    toRectF(area),
                    nullptr,
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                    D2D1::IdentityMatrix(),
                    alpha
                ),
                group.Get()
            );
        }
    }
    float const brushOpacity = group ? 1.0f : alpha;

    target.SetTransform(layer.placement * toTarget);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (layer.stroked
        && SUCCEEDED(target.CreateSolidColorBrush(layer.stroke, D2D1::BrushProperties(brushOpacity), brush.GetAddressOf()))) {
        target.DrawGeometry(layer.geometry.Get(), brush.Get(), layer.strokeWidth, layer.strokeStyle);
    }
    brush.Reset();
    if (layer.filled
        && SUCCEEDED(target.CreateSolidColorBrush(layer.fill, D2D1::BrushProperties(brushOpacity), brush.GetAddressOf()))) {
        target.FillGeometry(layer.geometry.Get(), brush.Get());
    }
    target.SetTransform(D2D1::Matrix3x2F::Identity());
    if (group) target.PopLayer();
}

// Draws flat layers into one frame-sized surface and moves them into the output in batches, so consecutive titles and
// shapes cost one readback. Anything that writes `out` directly (models, tilted layers) flushes first to keep order.
class FrameComposer {
public:
    FrameComposer(Graphics const& g, OverlayImage& out) : mGraphics(g), mOut(out) {}
    ~FrameComposer() {
        flush();
        surfacePool().release(std::move(mSurface));
    }
    FrameComposer(FrameComposer const&)            = delete;
    FrameComposer& operator=(FrameComposer const&) = delete;

    bool draw(Layer const& layer, float alpha) {
        if (intersect(layer.bounds, frame()).empty()) return false;
        if (!begin()) return false;
        PixelRect const area = intersect(
            layer.bounds,
            PixelRect{0, 0, static_cast<int>(mSurface->width), static_cast<int>(mSurface->height)}
        );
        drawLayer(*mSurface->target.Get(), layer, D2D1::Matrix3x2F::Identity(), alpha, area);
        mPending = unite(mPending, area);
        return true;
    }

    bool drawTilted(Layer const& layer, float alpha) {
        flush();
        // A turned card can bring in parts that lay outside the frame, but not without limit: keep a margin of a
        // quarter frame on each side so the intermediate stays bounded.
        int const       marginX = static_cast<int>(mOut.width / 4);
        int const       marginY = static_cast<int>(mOut.height / 4);
        PixelRect const reach{-marginX, -marginY, static_cast<int>(mOut.width) + marginX, static_cast<int>(mOut.height) + marginY};
        PixelRect const flat = intersect(layer.bounds, reach);
        if (flat.empty()) return false;

        auto surface = surfacePool().acquire(
            mGraphics,
            static_cast<uint32_t>(flat.width()),
            static_cast<uint32_t>(flat.height())
        );
        if (!surface) return false;
        PixelRect const area{0, 0, flat.width(), flat.height()};
        beginDraw(*surface);
        drawLayer(
            *surface->target.Get(),
            layer,
            D2D1::Matrix3x2F::Translation(static_cast<float>(-flat.x0), static_cast<float>(-flat.y0)),
            1.0f,
            area
        );
        bool const ended = SUCCEEDED(surface->target->EndDraw());
        surface->stale   = unite(surface->stale, area);
        bool drew        = false;
        if (ended) {
            readPixels(*surface, area, [&](uint8_t const* pixels, uint32_t stride) {
                SourceView const source{pixels, stride, area.width(), area.height()};
                drew = warpOver(mOut, source, flat, layer.centre, layer.tiltX, layer.tiltY, alpha);
            });
            surfacePool().release(std::move(surface));
        }
        return drew;
    }

    void flush() {
        if (!mDrawing) return;
        mDrawing              = false;
        bool const      ended = SUCCEEDED(mSurface->target->EndDraw());
        PixelRect const region = intersect(mPending, frame());
        mSurface->stale        = unite(mSurface->stale, mPending);
        mPending               = {};
        if (!ended) {
            // The target is unusable; a fresh one is made on the next draw.
            mSurface.reset();
            return;
        }
        if (region.empty()) return;
        readPixels(*mSurface, region, [&](uint8_t const* pixels, uint32_t stride) {
            blendPbgraOver(pixels, stride, mOut, region);
        });
    }

private:
    [[nodiscard]] PixelRect frame() const noexcept {
        return {0, 0, static_cast<int>(mOut.width), static_cast<int>(mOut.height)};
    }

    bool begin() {
        if (mDrawing) return true;
        if (!mSurface) mSurface = surfacePool().acquire(mGraphics, mOut.width, mOut.height);
        if (!mSurface) return false;
        beginDraw(*mSurface);
        mDrawing = true;
        return true;
    }

    Graphics const&          mGraphics;
    OverlayImage&            mOut;
    std::unique_ptr<Surface> mSurface;
    PixelRect                mPending;
    bool                     mDrawing{};
};

[[nodiscard]] float layerAlpha(OverlayItem const& item, double tick) noexcept {
    return item.fadeAt(tick) * unit(item.transform.opacity);
}

[[nodiscard]] bool hasContent(OverlayItem const& item) noexcept {
    switch (item.kind) {
    case OverlayKind::Title: return !item.title.text.empty() && item.title.fontSize > 0.0f;
    case OverlayKind::Shape: return item.transform.width > 0.0f && item.transform.height > 0.0f;
    case OverlayKind::Model: return !item.model.file.empty();
    // Blurs and lights act on the rendered picture on the GPU (BlurEffects), not in this image; Stand-ins are
    // characters in the replay world.
    case OverlayKind::Blur:
    case OverlayKind::Light:
    case OverlayKind::StandIn: return false;
    }
    return false;
}

[[nodiscard]] std::vector<std::string> loadFontFamilies() {
    std::vector<std::wstring> names;
    if (auto const& fonts = graphics().fonts) {
        UINT32 const count = fonts->GetFontFamilyCount();
        names.reserve(count);
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<IDWriteFontFamily>       family;
            ComPtr<IDWriteLocalizedStrings> strings;
            if (FAILED(fonts->GetFontFamily(i, family.GetAddressOf()))
                || FAILED(family->GetFamilyNames(strings.GetAddressOf())) || strings->GetCount() == 0) {
                continue;
            }
            UINT32 index  = 0;
            BOOL   exists = FALSE;
            if (FAILED(strings->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
            UINT32 length = 0;
            if (FAILED(strings->GetStringLength(index, &length)) || length == 0) continue;
            std::wstring name(static_cast<size_t>(length) + 1, L'\0');
            if (FAILED(strings->GetString(index, name.data(), length + 1))) continue;
            name.resize(length);
            names.push_back(std::move(name));
        }
    }
    std::ranges::sort(names, [](std::wstring const& a, std::wstring const& b) {
        int const order =
            CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE);
        return order == CSTR_EQUAL ? a < b : order == CSTR_LESS_THAN;
    });
    names.erase(std::unique(names.begin(), names.end()), names.end());

    std::vector<std::string> families;
    families.reserve(names.size());
    for (auto const& name : names) families.push_back(toUtf8(name));
    return families;
}

} // namespace

bool renderOverlays(
    std::span<state::editing::model::OverlayItem const> items,
    double                                              tick,
    uint32_t                                            width,
    uint32_t                                            height,
    std::optional<OverlayCamera> const&                 camera,
    OverlayImage&                                       out
) {
    out.reset(width, height);
    if (width == 0 || height == 0 || !hasActiveOverlays(items, tick)) return false;

    Graphics const& g          = graphics();
    float const     frameWidth  = static_cast<float>(width);
    float const     frameHeight = static_cast<float>(height);
    bool            drew        = false;
    bool            modelTried  = false;
    {
        FrameComposer composer{g, out};
        for (auto const& stored : items) {
            // Keyed properties take their value at this tick; the rest stay as set.
            auto const item = state::editing::model::overlayAt(stored, tick);
            float const alpha = layerAlpha(item, tick);
            if (alpha <= MinVisibleAlpha || !hasContent(item)) continue;

            if (item.kind == OverlayKind::Model) {
                if (!camera) continue;
                composer.flush();
                modelTried = true;
                if (renderMimodel(item.model, *camera, alpha, out)) drew = true;
                continue;
            }
            if (!g.ready()) continue;
            auto const layer = prepareLayer(g, item, frameWidth, frameHeight);
            if (!layer) continue;
            if (layer->tilted() ? composer.drawTilted(*layer, alpha) : composer.draw(*layer, alpha)) drew = true;
        }
    }
    // A model that reported nothing may still have touched pixels; keep the promise of a transparent image.
    if (!drew && modelTried) std::ranges::fill(out.rgba, uint8_t{0});
    return drew;
}

bool hasActiveOverlays(std::span<state::editing::model::OverlayItem const> items, double tick) {
    return std::ranges::any_of(items, [tick](OverlayItem const& stored) {
        if (!stored.activeAt(tick)) return false;
        auto const item = state::editing::model::overlayAt(stored, tick);
        return layerAlpha(item, tick) > MinVisibleAlpha && hasContent(item);
    });
}

void compositeOver(
    uint8_t*            pixels,
    uint32_t            width,
    uint32_t            height,
    uint32_t            rowPitch,
    bool                bgra,
    OverlayImage const& overlay
) {
    if (!pixels) return;
    uint32_t const columns = std::min(width, overlay.width);
    uint32_t const rows    = std::min(height, overlay.height);
    if (overlay.rgba.size() < static_cast<size_t>(overlay.width) * overlay.height * 4) return;
    size_t const red  = bgra ? 2 : 0;
    size_t const blue = bgra ? 0 : 2;

    for (uint32_t y = 0; y < rows; ++y) {
        uint8_t const* s = overlay.rgba.data() + static_cast<size_t>(y) * overlay.width * 4;
        uint8_t*       d = pixels + static_cast<size_t>(y) * rowPitch;
        for (uint32_t x = 0; x < columns; ++x, s += 4, d += 4) {
            uint32_t word;
            std::memcpy(&word, s, 4);
            if (word == 0) continue;
            uint32_t const sa = s[3];
            if (sa == 255) {
                d[red]  = s[0];
                d[1]    = s[1];
                d[blue] = s[2];
                continue;
            }
            uint32_t const inverse = 255 - sa;
            d[red]                 = over(s[0], d[red], inverse);
            d[1]                   = over(s[1], d[1], inverse);
            d[blue]                = over(s[2], d[blue], inverse);
        }
    }
}

std::vector<std::string> const& systemFontFamilies() {
    static std::vector<std::string> const families = loadFontFamilies();
    return families;
}

} // namespace playback::overlay
