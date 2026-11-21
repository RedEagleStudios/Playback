#pragma once

#include "MathTypes.h"
#include "PropertyChannel.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <string>
#include <vector>

namespace playback::state::editing::model {

// A layer over the replay picture, in the preview and in every export: a title, a shape, a model placed in the world
// (a Mine-imator file, or a Minecraft block or item), a blur of the picture, a light on it, or a Stand-in (a recorded
// player's actions played again by another character). Items later in EditorStateExt::overlays draw on top of earlier
// ones; blurs and lights act on the replay picture, in layer order, under every title, shape and model.
enum class OverlayKind : uint8_t { Title, Shape, Model, Blur, Light, StandIn, Screen };

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

// Point: a glow around the position, the size of the layer's width x height, turned by its rotation. Spot: a cone from
// the position towards the rotation (0 points right, clockwise), reaching the layer's height, `spread` degrees either
// side. Strip: a band through the position along the rotation, the layer's height thick, as light through a window.
// Ambient: the whole frame alike.
enum class LightStyle : uint8_t { Point, Spot, Strip, Ambient };

struct LightSettings {
    LightStyle style{LightStyle::Point};
    // How much brighter the lit picture gets at full strength: 1 doubles it; negative darkens.
    float level{1.0f};
    // 0 fades over the whole reach, 1 a hard edge.
    float hardness{0.2f};
    // Colour temperature: 1000 K candle orange to 12000 K blue sky; 6500 K is white.
    float kelvin{6500.0f};
    // A gel over the light, multiplied with the temperature's colour.
    std::array<float, 3> filter{1.0f, 1.0f, 1.0f};
    // Spot: half the cone's angle, in degrees.
    float spread{25.0f};

    bool operator==(LightSettings const&) const = default;
};

// How a Stand-in looks: the source player's own skin, another recorded player's skin, a skin image (64x64 or 64x32
// PNG), or a mob of any entity type.
enum class StandInLook : uint8_t { SameSkin, PlayerSkin, SkinFile, Mob };

// What a Stand-in wears or holds: what the source had at that moment, nothing, or the items chosen for it.
enum class StandInGear : uint8_t { AsRecorded, None, Custom };

// A Stand-in plays a recorded player's actions (movement, turning, swings, what they hold and wear, and optionally the
// blocks they changed) again as another character, so one player can fill a scene with many. The layer's start plays
// the recording at `sourceTick`; each tick after it plays the next recorded tick.
struct StandInSettings {
    // The recorded player whose actions it plays: their UUID, empty for the player who recorded (as the follow list
    // names them), and their name when it was chosen.
    std::string subject;
    std::string subjectLabel;
    int         sourceTick{};
    StandInLook look{StandInLook::SameSkin};
    // PlayerSkin: the recorded player whose skin it wears (UUID, empty for the recorder).
    std::string skinPlayer;
    // SkinFile: an absolute path to a skin image; `slim` uses the thin-armed (Alex) model.
    std::string skinFile;
    bool        slim{};
    // Mob: an entity type such as "minecraft:zombie".
    std::string mob{"minecraft:zombie"};
    // Shown over its head; empty shows none.
    std::string nameTag;
    // Where it plays: moved by `offset` blocks and turned `turn` degrees about where the source stood at sourceTick.
    Vec3  offset{};
    float turn{};
    // Also places and breaks the blocks the source changed within reach, at the Stand-in's time and place.
    bool blocks{};
    // Armour: as recorded, none (shows the skin), or these items by slot: head, chest, legs, feet ("minecraft:..."; an
    // empty slot wears nothing there).
    StandInGear                armor{StandInGear::AsRecorded};
    std::array<std::string, 4> armorItems;
    // Hands: as recorded, empty, or these items ("minecraft:diamond_sword"; empty holds nothing).
    StandInGear hands{StandInGear::AsRecorded};
    std::string mainHand;
    std::string offHand;

    bool operator==(StandInSettings const&) const = default;
};

// How a picture fills a screen whose shape differs from it.
enum class ScreenFit : uint8_t { Fill, Fit, Stretch };

// A Screen layer shows a photo or video from a local file on one of the furniture add-on's screens (medium TV, big TV,
// monitor, wide monitor) placed in the replay world, from the layer's start to its end. The picture replaces the
// screen's own texture in the game, so it is lit, hidden behind what stands in front of it and exported as the world is.
struct ScreenSettings {
    // The screen: the recorded unique id of its entity, as decimal text; empty until one is chosen.
    std::string target;
    // How the screen was named when chosen ("Big TV at 12, 64, -30"), shown when the replay does not have it loaded.
    std::string targetLabel;
    // An image (png, jpg, bmp, gif, webp, tiff) or a video (mp4, mov, mkv, webm, avi, m4v) by absolute path.
    std::string file;
    ScreenFit   fit{ScreenFit::Fill};
    // Seconds into the video at the layer's start; trimming the layer's start moves it with the edge.
    float sourceOffset{};
    // A video shorter than the layer starts again; otherwise it holds its last frame.
    bool loop{true};
    // Percent of the picture's brightness (keyable as screenBrightness).
    float brightness{100.0f};

    bool operator==(ScreenSettings const&) const = default;
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
    LightSettings    light;
    StandInSettings  standIn;
    ScreenSettings   screen;
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
    if (name == "lightLevel") return &item.light.level;
    if (name == "lightHardness") return &item.light.hardness;
    if (name == "lightKelvin") return &item.light.kelvin;
    if (name == "lightFilterR") return &item.light.filter[0];
    if (name == "lightFilterG") return &item.light.filter[1];
    if (name == "lightFilterB") return &item.light.filter[2];
    if (name == "lightSpread") return &item.light.spread;
    if (name == "standInX") return &item.standIn.offset.x;
    if (name == "standInY") return &item.standIn.offset.y;
    if (name == "standInZ") return &item.standIn.offset.z;
    if (name == "standInTurn") return &item.standIn.turn;
    if (name == "screenBrightness") return &item.screen.brightness;
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

// Moves only the layer's start edge: its keys stay where they are on the timeline, so they shift within the layer. A
// Stand-in's recording moves with the edge too, as trimming a clip does, so what plays at every other tick is unchanged.
inline void setOverlayStart(OverlayItem& item, int startTick) {
    int const shift = startTick - item.startTick;
    item.startTick  = startTick;
    if (shift == 0) return;
    if (item.kind == OverlayKind::StandIn) item.standIn.sourceTick = std::max(0, item.standIn.sourceTick + shift);
    if (item.kind == OverlayKind::Screen) {
        item.screen.sourceOffset = std::max(0.0f, item.screen.sourceOffset + static_cast<float>(shift) / 20.0f);
    }
    for (auto& [name, channel] : item.channels) {
        ValueChannel moved;
        for (auto const& [tick, key] : channel) moved.emplace(tick - shift, key);
        channel = std::move(moved);
    }
}

} // namespace playback::state::editing::model
