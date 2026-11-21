#include "OverlayEditing.h"

#include "playback/editor/graphics/CameraRenderHooks.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/overlay/VanillaAssets.h"

#include "ll/api/i18n/I18n.h"

#include <Windows.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace playback::editor::ui::overlays {

using namespace ll::i18n_literals;
using state::EditorAction;
using state::EditorActionType;
using state::editing::model::EditorStateExt;

namespace {

constexpr int kDefaultLayerTicks = 100;

std::string defaultName(OverlayKind kind, ShapeKind shape, int points, BlurStyle blur, LightStyle light, int number) {
    switch (kind) {
    case OverlayKind::Title:
        return "playback.refactorEditor.layers.defaultTitle"_tr(number);
    case OverlayKind::Model:
        return "playback.refactorEditor.layers.defaultModel"_tr(number);
    case OverlayKind::Blur:
        return "playback.refactorEditor.layers.defaultBlur"_tr(blurStyleName(blur), number);
    case OverlayKind::Light:
        return "playback.refactorEditor.layers.defaultBlur"_tr(lightStyleName(light), number);
    case OverlayKind::Shape:
        break;
    }
    switch (shape) {
    case ShapeKind::Rectangle:
        return "playback.refactorEditor.layers.defaultRectangle"_tr(number);
    case ShapeKind::Ellipse:
        return "playback.refactorEditor.layers.defaultEllipse"_tr(number);
    case ShapeKind::Polygon:
        break;
    }
    return points == 3 ? "playback.refactorEditor.layers.defaultTriangle"_tr(number)
                       : "playback.refactorEditor.layers.defaultPolygon"_tr(number);
}

std::string nextId(EditorStateExt const& project) {
    int highest = 0;
    for (auto const& item : project.overlays) {
        if (item.id.rfind("layer-", 0) != 0) continue;
        highest = std::max(highest, std::atoi(item.id.c_str() + 6));
    }
    return "layer-" + std::to_string(highest + 1);
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    int const size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(std::max(0, size)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    int const size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(std::max(0, size)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::vector<OverlayItem> withItem(EditorStateExt const& project, OverlayItem const& item) {
    auto layers   = project.overlays;
    auto existing = std::ranges::find(layers, item.id, &OverlayItem::id);
    if (existing != layers.end()) *existing = item;
    else layers.push_back(item);
    return layers;
}

} // namespace

OverlayItem makeLayer(
    EditorStateExt const& project,
    OverlayKind           kind,
    int                   tick,
    ShapeKind             shape,
    int                   points,
    BlurStyle             blur,
    LightStyle            light
) {
    OverlayItem item;
    item.id          = nextId(project);
    item.kind        = kind;
    item.startTick   = std::clamp(tick, 0, std::max(0, project.totalTicks - 1));
    item.endTick     = std::max(item.startTick + 1, std::min(item.startTick + kDefaultLayerTicks, project.totalTicks));
    item.shape.kind  = shape;
    item.shape.points = std::clamp(points, 3, 64);
    if (kind == OverlayKind::Shape) {
        item.transform.width  = shape == ShapeKind::Rectangle ? 0.4f : 0.3f;
        item.transform.height = 0.3f;
    }
    if (kind == OverlayKind::Title) item.title.text = "playback.refactorEditor.layers.defaultText"_tr();
    if (kind == OverlayKind::Model) {
        if (auto const point = pointInFrontOfCamera(4.0f)) item.model.position = *point;
    }
    if (kind == OverlayKind::Blur) {
        // Strengths that read clearly at once; the area defaults to a box in the middle once it is not the frame.
        item.blur.style       = blur;
        item.blur.amount      = blur == BlurStyle::Mosaic   ? 0.03f
                              : blur == BlurStyle::Motion   ? 0.06f
                              : blur == BlurStyle::Radial   ? 0.3f
                                                            : 0.02f;
        item.transform.width  = 0.4f;
        item.transform.height = 0.3f;
    }
    if (kind == OverlayKind::Light) {
        // A warm key light from the upper left that reads at once; a spot points down and right into the frame.
        item.light.style      = light;
        item.light.kelvin     = 4500.0f;
        item.transform.x      = light == LightStyle::Spot ? 0.2f : 0.35f;
        item.transform.y      = light == LightStyle::Spot ? 0.1f : 0.35f;
        item.transform.width  = light == LightStyle::Spot ? 0.05f : 0.6f;
        item.transform.height = light == LightStyle::Spot ? 0.9f : 0.6f;
        item.transform.rotation = light == LightStyle::Spot ? 60.0f : light == LightStyle::Strip ? 45.0f : 0.0f;
        if (light == LightStyle::Strip) item.transform.height = 0.25f;
        if (light == LightStyle::Ambient) item.light.level = 0.3f;
    }

    int const sameKind = static_cast<int>(std::ranges::count_if(project.overlays, [&](OverlayItem const& other) {
        return other.kind == kind;
    }));
    for (int number = sameKind + 1;; ++number) {
        item.name = defaultName(kind, shape, item.shape.points, blur, light, number);
        if (std::ranges::none_of(project.overlays, [&](OverlayItem const& other) { return other.name == item.name; }))
            break;
    }
    return item;
}

OverlayItem const* find(EditorStateExt const& project, std::string const& id) {
    auto const item = std::ranges::find(project.overlays, id, &OverlayItem::id);
    return item == project.overlays.end() ? nullptr : &*item;
}

void commit(PanelContext const& ctx, std::vector<OverlayItem> layers) {
    EditorAction action{EditorActionType::SetOverlays};
    action.overlays = std::move(layers);
    ctx.submitAction(std::move(action));
}

void commitItem(PanelContext const& ctx, OverlayItem const& item) {
    if (auto const* project = ctx.project()) commit(ctx, withItem(*project, item));
}

void previewItem(PanelContext const& ctx, OverlayItem const& item) {
    auto const* project = ctx.project();
    if (!project) return;
    EditorAction action{EditorActionType::PreviewOverlays};
    action.overlays = withItem(*project, item);
    ctx.submitAction(std::move(action));
}

void endPreview(PanelContext const& ctx) { ctx.submitAction(EditorAction{EditorActionType::PreviewOverlays}); }

std::string blurStyleName(BlurStyle style) {
    switch (style) {
    case BlurStyle::Gaussian:
        return "playback.refactorEditor.layers.blurGaussian"_tr();
    case BlurStyle::Mosaic:
        return "playback.refactorEditor.layers.blurMosaic"_tr();
    case BlurStyle::Motion:
        return "playback.refactorEditor.layers.blurMotion"_tr();
    case BlurStyle::Radial:
        return "playback.refactorEditor.layers.blurRadial"_tr();
    }
    return {};
}

char const* blurStyleIcon(BlurStyle style) {
    switch (style) {
    case BlurStyle::Gaussian:
        return ICON_BLUR;
    case BlurStyle::Mosaic:
        return ICON_MOSAIC;
    case BlurStyle::Motion:
        return ICON_MOTION;
    case BlurStyle::Radial:
        return ICON_RADIAL;
    }
    return ICON_BLUR;
}

std::string lightStyleName(LightStyle style) {
    switch (style) {
    case LightStyle::Point:
        return "playback.refactorEditor.layers.lightPoint"_tr();
    case LightStyle::Spot:
        return "playback.refactorEditor.layers.lightSpot"_tr();
    case LightStyle::Strip:
        return "playback.refactorEditor.layers.lightStrip"_tr();
    case LightStyle::Ambient:
        return "playback.refactorEditor.layers.lightAmbient"_tr();
    }
    return {};
}

char const* lightStyleIcon(LightStyle style) {
    switch (style) {
    case LightStyle::Point:
        return ICON_LIGHT_POINT;
    case LightStyle::Spot:
        return ICON_LIGHT_SPOT;
    case LightStyle::Strip:
        return ICON_LIGHT_STRIP;
    case LightStyle::Ambient:
        return ICON_LIGHT_AMBIENT;
    }
    return ICON_LIGHT_POINT;
}

char const* icon(OverlayItem const& item) {
    switch (item.kind) {
    case OverlayKind::Title:
        return ICON_TITLE;
    case OverlayKind::Model:
        return overlay::vanilla::isAsset(item.model.file) ? ICON_VANILLA : ICON_MODEL;
    case OverlayKind::Blur:
        return blurStyleIcon(item.blur.style);
    case OverlayKind::Light:
        return lightStyleIcon(item.light.style);
    case OverlayKind::Shape:
        break;
    }
    switch (item.shape.kind) {
    case ShapeKind::Rectangle:
        return ICON_RECTANGLE;
    case ShapeKind::Ellipse:
        return ICON_ELLIPSE;
    case ShapeKind::Polygon:
        break;
    }
    return item.shape.points == 3 ? ICON_TRIANGLE : ICON_POLYGON;
}

std::optional<std::string> pickModelFile() {
    std::array<wchar_t, 32768> file{};
    std::wstring filter = wide("playback.refactorEditor.layers.modelFiles"_tr()) + L" (*.mimodel)";
    filter.push_back(L'\0');
    filter += L"*.mimodel";
    filter.push_back(L'\0');
    filter.push_back(L'\0');

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile   = file.data();
    dialog.nMaxFile    = static_cast<DWORD>(file.size());
    dialog.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return std::nullopt;
    return utf8(file.data());
}

std::optional<state::editing::model::Vec3> pointInFrontOfCamera(float distance) {
    auto const camera = graphics::currentRendererCameraState();
    if (!camera) return std::nullopt;
    // Minecraft's yaw turns clockwise from +Z; the model stands on the ground the camera looks across.
    float const yaw = camera->yaw * std::numbers::pi_v<float> / 180.0f;
    return state::editing::model::Vec3{
        camera->x - std::sin(yaw) * distance,
        std::floor(camera->y - 1.62f),
        camera->z + std::cos(yaw) * distance,
    };
}

} // namespace playback::editor::ui::overlays
