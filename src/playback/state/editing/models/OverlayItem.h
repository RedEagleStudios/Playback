#pragma once

#include "MathTypes.h"
#include "PropertyChannel.h"

#include <algorithm>
#include <string_view>
#include <string>
#include <vector>

namespace playback::state::editing::model {

// A layer over the replay picture, in the preview and in every export: a title, a shape, a Mine-imator model placed in
// the world, or a blur of the picture itself. Items later in EditorStateExt::overlays draw on top of earlier ones;
// blurs act on the replay picture, under every title, shape and model.
enum class OverlayKind : uint8_t { Title, Shape, Model, Blur };

// Polygon: a regular polygon of `points` corners (3 is a triangle; many approach a circle).
enum class ShapeKind : uint8_t { Rectangle, Ellipse, Polygon };

enum class TextAlign : uint8_t { Left, Centre, Right };

// Placement of a title or shape on the frame. Positions are fractions of the frame (0,0 top-left, 1,1 bottom-right);
// sizes are fractions of the frame's height, so a 0.2 x 0.2 shape is square at any aspect ratio.
struct OverlayTransform {
    float x{0.5f};
    float y{0.5f};
    // Shapes only: the box the shape fills. Titles size from their font.
    float width{0.3f};
    float height{0.3f};
    // Degrees clockwise in the frame's plane.
    float rotation{};
    // 2D skew: degrees of shear along x (top leans right) and along y.
    float skewX{};
    float skewY{};
    // 3D skew: degrees the layer turns about its horizontal axis (top away from the viewer) and its vertical axis
    // (right side away), seen in perspective.
    float tiltX{};
    float tiltY{};
    float opacity{1.0f};

    bool operator==(OverlayTransform const&) const = default;
};

struct TitleStyle {
    std::string text{"Title"};
    std::string fontFamily{"Segoe UI"};
    // Fraction of the frame's height.
    float     fontSize{0.08f};
    bool      bold{true};
    bool      italic{};
    TextAlign align{TextAlign::Centre};
    Color4    color{1, 1, 1, 1};
    // Stroke around every glyph, a fraction of the frame's height; 0 draws none.
    float  outlineWidth{0.004f};
    Color4 outlineColor{0, 0, 0, 1};

    bool operator==(TitleStyle const&) const = default;
};

struct ShapeStyle {
    ShapeKind kind{ShapeKind::Rectangle};
    // Polygon corners, 3 to 64.
    int    points{3};
    Color4 fill{0.23f, 0.55f, 0.94f, 1.0f};
    // Fraction of the frame's height; 0 draws no outline.
    float  strokeWidth{};
    Color4 stroke{1, 1, 1, 1};
    // Rectangle only: corner radius as a fraction of the frame's height.
    float cornerRadius{};

    bool operator==(ShapeStyle const&) const = default;
};

// A Mine-imator model (.mimodel) standing in the replay world.
struct ModelPlacement {
    // Absolute path to the .mimodel file; its textures resolve relative to it.
    std::string file;
    Vec3        position{};
    // Degrees: yaw about +Y (Minecraft's), pitch about the model's X, roll about its Z.
    float yaw{};
    float pitch{};
    float roll{};
    // 1 is the model's own size, 16 model units to a block.
    float scale{1.0f};

    bool operator==(ModelPlacement const&) const = default;
};

// Gaussian: a soft even blur. Mosaic: square blocks of one colour (pixelate). Motion: streaks along one direction.
// Radial: a zoom blur towards the area's centre.
enum class BlurStyle : uint8_t { Gaussian, Mosaic, Motion, Radial };

// Where a blur acts: the whole frame, or a rectangle or ellipse placed by the layer's transform (x, y, width, height,
// rotation).
enum class BlurArea : uint8_t { Frame, Rectangle, Ellipse };

struct BlurSettings {
    BlurStyle style{BlurStyle::Gaussian};
    // Gaussian radius, mosaic block size or motion streak length as a fraction of the frame's height; Radial: how far
    // each pixel smears towards the centre, as a fraction of its distance to it.
    float amount{0.02f};
    // Motion: degrees, 0 horizontal, clockwise.
    float    angle{};
    BlurArea area{BlurArea::Frame};
    // Soft edge of the area, a fraction of the frame's height.
    float feather{0.02f};
    // Blurs everything outside the area instead.
    bool invert{};

    bool operator==(BlurSettings const&) const = default;
};

struct OverlayItem {
    std::string id;
    std::string name;
    OverlayKind kind{OverlayKind::Title};
    // Shown from startTick until endTick (exclusive), in replay ticks.
    int  startTick{};
    int  endTick{100};
    int  fadeInTicks{};
    int  fadeOutTicks{};
    bool visible{true};
    bool locked{};

    OverlayTransform transform;
    TitleStyle       title;
    ShapeStyle       shape;
    ModelPlacement   model;
    BlurSettings     blur;
    // Keyed numeric properties (overlayProperty names), each on its own. Key ticks count from startTick, so moving
    // the layer moves its keys with it.
    ValueChannels channels;

    [[nodiscard]] bool activeAt(double tick) const noexcept {
        return visible && tick >= static_cast<double>(startTick) && tick < static_cast<double>(endTick);
    }

    // Fade multiplier at `tick`, 0 outside the item.
    [[nodiscard]] float fadeAt(double tick) const noexcept {
        if (!activeAt(tick)) return 0.0f;
        double alpha = 1.0;
        if (fadeInTicks > 0) alpha = std::min(alpha, (tick - startTick) / fadeInTicks);
        if (fadeOutTicks > 0) alpha = std::min(alpha, (endTick - tick) / fadeOutTicks);
        return static_cast<float>(alpha < 0.0 ? 0.0 : alpha > 1.0 ? 1.0 : alpha);
    }

    bool operator==(OverlayItem const&) const = default;
};

// The layer's keyable numbers, by name; an unknown name gives null.
[[nodiscard]] inline float* overlayProperty(OverlayItem& item, std::string_view name) {
    auto& t = item.transform;
    if (name == "x") return &t.x;
    if (name == "y") return &t.y;
    if (name == "width") return &t.width;
    if (name == "height") return &t.height;
    if (name == "rotation") return &t.rotation;
    if (name == "skewX") return &t.skewX;
    if (name == "skewY") return &t.skewY;
    if (name == "tiltX") return &t.tiltX;
    if (name == "tiltY") return &t.tiltY;
    if (name == "opacity") return &t.opacity;
    if (name == "fontSize") return &item.title.fontSize;
    if (name == "outlineWidth") return &item.title.outlineWidth;
    if (name == "strokeWidth") return &item.shape.strokeWidth;
    if (name == "cornerRadius") return &item.shape.cornerRadius;
    if (name == "modelX") return &item.model.position.x;
    if (name == "modelY") return &item.model.position.y;
    if (name == "modelZ") return &item.model.position.z;
    if (name == "yaw") return &item.model.yaw;
    if (name == "pitch") return &item.model.pitch;
    if (name == "roll") return &item.model.roll;
    if (name == "scale") return &item.model.scale;
    if (name == "blurAmount") return &item.blur.amount;
    if (name == "blurAngle") return &item.blur.angle;
    if (name == "blurFeather") return &item.blur.feather;
    return nullptr;
}

// The layer as it stands at `tick`: every keyed property at its keyed value.
[[nodiscard]] inline OverlayItem overlayAt(OverlayItem const& item, double tick) {
    if (item.channels.empty()) return item;
    OverlayItem result = item;
    double const local = tick - item.startTick;
    for (auto const& [name, channel] : item.channels) {
        auto const value = sampleChannel(channel, local);
        if (!value) continue;
        if (float* field = overlayProperty(result, name)) *field = *value;
    }
    return result;
}

// Moves only the layer's start edge: its keys stay where they are on the timeline, so they shift within the layer.
inline void setOverlayStart(OverlayItem& item, int startTick) {
    int const shift = startTick - item.startTick;
    item.startTick  = startTick;
    if (shift == 0) return;
    for (auto& [name, channel] : item.channels) {
        ValueChannel moved;
        for (auto const& [tick, key] : channel) moved.emplace(tick - shift, key);
        channel = std::move(moved);
    }
}

} // namespace playback::state::editing::model
