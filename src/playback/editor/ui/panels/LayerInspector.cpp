#include "LayerInspector.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/KeyframeControls.h"
#include "playback/editor/ui/components/OverlayEditing.h"
#include "playback/editor/ui/components/PropertyControls.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/overlay/MimodelRenderer.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/overlay/OverlayRenderer.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace playback::editor::ui {

using namespace ll::i18n_literals;
using state::editing::model::Color4;
using state::editing::model::OverlayItem;
using state::editing::model::OverlayKind;
using state::editing::model::ShapeKind;
using state::editing::model::TextAlign;

namespace {

constexpr float kTicksPerSecond = static_cast<float>(widgets::kTicksPerSecond);

// A caption over the control that follows, which then spans the inspector.
void label(std::string const& text, char const* icon = nullptr) { widgets::fieldLabel(icon, text.c_str()); }

bool toggle(char const* id, char const* icon, std::string const& text, bool& value) {
    return widgets::toggleRow(id, icon, text.c_str(), &value);
}

bool colour(std::string const& text, char const* id, Color4& value) {
    label(text);
    float rgba[4]{value.r, value.g, value.b, value.a};
    if (!ImGui::ColorEdit4(id, rgba, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) return false;
    value = {rgba[0], rgba[1], rgba[2], rgba[3]};
    return true;
}

// Seconds on the replay's clock, edited in ticks underneath.
bool seconds(std::string const& text, char const* id, int& ticks, int low, int high) {
    label(text);
    float value = static_cast<float>(ticks) / kTicksPerSecond;
    if (!widgets::dragFloat(
            id,
            &value,
            static_cast<float>(low) / kTicksPerSecond,
            static_cast<float>(high) / kTicksPerSecond,
            "%.2f s",
            0.05f
        )) {
        return false;
    }
    ticks = std::clamp(static_cast<int>(std::lround(value * kTicksPerSecond)), low, high);
    return true;
}

bool containsInsensitive(std::string_view value, std::string_view query) {
    if (query.empty()) return true;
    return std::search(value.begin(), value.end(), query.begin(), query.end(), [](char left, char right) {
               return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
           })
        != value.end();
}

void copyInto(auto& buffer, std::string const& text) {
    std::snprintf(buffer.data(), buffer.size(), "%s", text.c_str());
}

} // namespace

std::string LayerInspector::subject(PanelContext const& ctx) {
    auto const* selected = ctx.selection.getAs<state::editing::model::SelectedOverlay>();
    auto const* project  = ctx.project();
    if (!selected || !project) return {};
    auto const* item = overlays::find(*project, selected->overlayId);
    return item ? item->name : std::string{};
}

bool LayerInspector::draw(PanelContext const& ctx) {
    auto const* selected = ctx.selection.getAs<state::editing::model::SelectedOverlay>();
    auto const* project  = ctx.project();
    if (!selected || !project) {
        if (mDraft) overlays::endPreview(ctx);
        mDraft.reset();
        return false;
    }
    auto const* stored = overlays::find(*project, selected->overlayId);
    if (!stored) {
        ImGui::TextDisabled("%s", "playback.refactorEditor.layers.missing"_tr().c_str());
        mDraft.reset();
        return true;
    }
    if (mDraft && mDraft->id != stored->id) mDraft.reset();
    // Text buffers follow the project (an undo, another layer) whenever the user is not typing in them.
    if (mBufferItemId != stored->id || (!mDraft && !ImGui::IsAnyItemActive())) {
        mBufferItemId = stored->id;
        copyInto(mNameBuffer, stored->name);
        copyInto(mTextBuffer, stored->title.text);
    }

    OverlayItem item   = mDraft ? *mDraft : *stored;
    OverlayItem before = item;
    // Keys count from the layer's start.
    mLocalTick = ctx.state.currentTick - item.startTick;
    mSeek.reset();

    drawLayerSection(item, project->totalTicks);
    ImGui::BeginDisabled(item.locked);
    switch (item.kind) {
    case OverlayKind::Title:
        drawTitleSection(item);
        drawTransformSection(item);
        break;
    case OverlayKind::Shape:
        drawShapeSection(item);
        drawTransformSection(item);
        break;
    case OverlayKind::Model:
        drawModelSection(item);
        break;
    }
    ImGui::EndDisabled();

    if (mSeek) ctx.commands.seekTo(item.startTick + *mSeek);
    if (!(item == before)) {
        mDraft = item;
        overlays::previewItem(ctx, item);
    }
    // Held sliders keep previewing; once nothing is held the draft becomes one undo step.
    if (mDraft && !ImGui::IsAnyItemActive()) {
        overlays::commitItem(ctx, *mDraft);
        mDraft.reset();
    }
    return true;
}

void LayerInspector::drawLayerSection(OverlayItem& item, int totalTicks) {
    if (!property::beginSection((ICON_CLOCK "  " + "playback.refactorEditor.layers.layer"_tr()).c_str())) return;
    ImGui::BeginDisabled(item.locked);
    label("playback.refactorEditor.layers.name"_tr());
    if (ImGui::InputText("##layer-name", mNameBuffer.data(), mNameBuffer.size()) && mNameBuffer[0] != '\0') {
        item.name = mNameBuffer.data();
    }
    int const lastTick = std::max(1, totalTicks);
    // The start is the layer's start edge; its keys stay where they are on the timeline.
    int start = item.startTick;
    if (seconds("playback.refactorEditor.layers.start"_tr(), "##layer-start", start, 0, item.endTick - 1)) {
        state::editing::model::setOverlayStart(item, start);
    }
    seconds("playback.refactorEditor.layers.end"_tr(), "##layer-end", item.endTick, item.startTick + 1, lastTick);
    int const length = item.endTick - item.startTick;
    seconds("playback.refactorEditor.layers.fadeIn"_tr(), "##layer-fade-in", item.fadeInTicks, 0, length);
    seconds("playback.refactorEditor.layers.fadeOut"_tr(), "##layer-fade-out", item.fadeOutTicks, 0, length);
    keyRow(item, "opacity", ICON_OPACITY, "playback.refactorEditor.layers.opacity"_tr(), "opacity", 0.0f, 100.0f, "%.0f%%", 100.0f);
    toggle("##layer-visible", ICON_EYE, "playback.refactorEditor.layers.visible"_tr(), item.visible);
    ImGui::EndDisabled();
    toggle("##layer-locked", ICON_LOCK, "playback.refactorEditor.timeline.lockLayer"_tr(), item.locked);
    property::endSection();
}

void LayerInspector::drawTitleSection(OverlayItem& item) {
    auto& title = item.title;
    if (!property::beginSection((ICON_TITLE "  " + "playback.refactorEditor.layers.text"_tr()).c_str())) return;
    label("playback.refactorEditor.layers.text"_tr());
    if (ImGui::InputTextMultiline(
            "##title-text",
            mTextBuffer.data(),
            mTextBuffer.size(),
            {-1.0f, ImGui::GetTextLineHeight() * 3.5f}
        )) {
        title.text = mTextBuffer.data();
    }

    label("playback.refactorEditor.layers.font"_tr());
    if (widgets::beginCombo("##title-font", title.fontFamily.c_str())) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint(
            "##title-font-filter",
            "playback.refactorEditor.layers.searchFonts"_tr().c_str(),
            mFontFilter.data(),
            mFontFilter.size()
        );
        for (auto const& family : overlay::systemFontFamilies()) {
            if (!containsInsensitive(family, mFontFilter.data())) continue;
            if (ImGui::Selectable(family.c_str(), family == title.fontFamily)) title.fontFamily = family;
        }
        widgets::endCombo();
    }
    // Sizes are fractions of the frame's height, shown in percent.
    keyRow(item, "font-size", ICON_FONT, "playback.refactorEditor.layers.size"_tr(), "fontSize", 1.0f, 40.0f, "%.1f%%", 100.0f);
    toggle("##title-bold", ICON_BOLD, "playback.refactorEditor.layers.bold"_tr(), title.bold);
    toggle("##title-italic", ICON_ITALIC, "playback.refactorEditor.layers.italic"_tr(), title.italic);

    label("playback.refactorEditor.layers.align"_tr());
    std::string const aligns[] = {
        "playback.refactorEditor.layers.alignLeft"_tr(),
        "playback.refactorEditor.layers.alignCentre"_tr(),
        "playback.refactorEditor.layers.alignRight"_tr(),
    };
    int align = static_cast<int>(title.align);
    if (widgets::beginCombo("##title-align", aligns[std::clamp(align, 0, 2)].c_str())) {
        for (int index = 0; index < 3; ++index) {
            if (ImGui::Selectable(aligns[index].c_str(), index == align)) title.align = static_cast<TextAlign>(index);
        }
        widgets::endCombo();
    }
    colour("playback.refactorEditor.layers.colour"_tr(), "##title-colour", title.color);

    keyRow(
        item,
        "outline",
        nullptr,
        "playback.refactorEditor.layers.outlineWidth"_tr(),
        "outlineWidth",
        0.0f,
        3.0f,
        "%.2f%%",
        100.0f
    );
    colour("playback.refactorEditor.layers.outlineColour"_tr(), "##title-outline-colour", title.outlineColor);
    property::endSection();
}

void LayerInspector::drawShapeSection(OverlayItem& item) {
    auto& shape = item.shape;
    if (!property::beginSection((ICON_SHAPE "  " + "playback.refactorEditor.layers.shape"_tr()).c_str())) return;
    label("playback.refactorEditor.layers.shapeKind"_tr());
    std::string const kinds[] = {
        "playback.refactorEditor.layers.rectangle"_tr(),
        "playback.refactorEditor.layers.ellipse"_tr(),
        "playback.refactorEditor.layers.polygon"_tr(),
    };
    int kind = static_cast<int>(shape.kind);
    if (widgets::beginCombo("##shape-kind", kinds[std::clamp(kind, 0, 2)].c_str())) {
        for (int index = 0; index < 3; ++index) {
            if (ImGui::Selectable(kinds[index].c_str(), index == kind)) shape.kind = static_cast<ShapeKind>(index);
        }
        widgets::endCombo();
    }
    if (shape.kind == ShapeKind::Polygon) {
        // 3 is a triangle; more corners approach a circle.
        label("playback.refactorEditor.layers.points"_tr());
        widgets::dragInt("##shape-points", &shape.points, 3, 64);
        widgets::itemTooltip("playback.refactorEditor.layers.pointsHint"_tr().c_str());
    }
    colour("playback.refactorEditor.layers.fill"_tr(), "##shape-fill", shape.fill);
    keyRow(
        item,
        "stroke",
        nullptr,
        "playback.refactorEditor.layers.outlineWidth"_tr(),
        "strokeWidth",
        0.0f,
        5.0f,
        "%.2f%%",
        100.0f
    );
    colour("playback.refactorEditor.layers.outlineColour"_tr(), "##shape-stroke-colour", shape.stroke);
    if (shape.kind == ShapeKind::Rectangle) {
        keyRow(
            item,
            "radius",
            nullptr,
            "playback.refactorEditor.layers.cornerRadius"_tr(),
            "cornerRadius",
            0.0f,
            25.0f,
            "%.1f%%",
            100.0f
        );
    }
    property::endSection();
}

void LayerInspector::drawTransformSection(OverlayItem& item) {
    auto& transform = item.transform;
    if (!property::beginSection((ICON_POSITION "  " + "playback.refactorEditor.layers.transform"_tr()).c_str())) return;
    // Each number keys on its own, so moving a layer never adds keys to its size or rotation.
    keyRow(item, "x", ICON_POSITION, "playback.refactorEditor.layers.positionX"_tr(), "x", -50.0f, 150.0f, "%.1f%%", 100.0f);
    keyRow(item, "y", ICON_POSITION, "playback.refactorEditor.layers.positionY"_tr(), "y", -50.0f, 150.0f, "%.1f%%", 100.0f);
    if (item.kind == OverlayKind::Shape) {
        keyRow(item, "width", ICON_SCALE, "playback.refactorEditor.layers.width"_tr(), "width", 0.5f, 400.0f, "%.1f%%", 100.0f);
        keyRow(item, "height", ICON_SCALE, "playback.refactorEditor.layers.height"_tr(), "height", 0.5f, 400.0f, "%.1f%%", 100.0f);
    }
    keyRow(item, "rotation", ICON_ROTATE, "playback.refactorEditor.layers.rotation"_tr(), "rotation", -180.0f, 180.0f, "%.1f°");
    keyRow(item, "skew-x", nullptr, "playback.refactorEditor.layers.skewX"_tr(), "skewX", -60.0f, 60.0f, "%.1f°");
    keyRow(item, "skew-y", nullptr, "playback.refactorEditor.layers.skewY"_tr(), "skewY", -60.0f, 60.0f, "%.1f°");
    widgets::itemTooltip("playback.refactorEditor.layers.skew2dHint"_tr().c_str());
    keyRow(item, "tilt-x", nullptr, "playback.refactorEditor.layers.tiltX"_tr(), "tiltX", -80.0f, 80.0f, "%.1f°");
    keyRow(item, "tilt-y", nullptr, "playback.refactorEditor.layers.tiltY"_tr(), "tiltY", -80.0f, 80.0f, "%.1f°");
    widgets::itemTooltip("playback.refactorEditor.layers.skew3dHint"_tr().c_str());
    if (property::actionButton("playback.refactorEditor.layers.resetTransform"_tr().c_str())) {
        float const opacity = transform.opacity;
        float const width   = transform.width;
        float const height  = transform.height;
        transform           = {};
        transform.opacity   = opacity;
        transform.width     = width;
        transform.height    = height;
        for (char const* name : {"x", "y", "rotation", "skewX", "skewY", "tiltX", "tiltY"}) item.channels.erase(name);
    }
    property::endSection();
}

void LayerInspector::drawModelSection(OverlayItem& item) {
    auto& model = item.model;
    if (!property::beginSection((ICON_MODEL "  " + "playback.refactorEditor.layers.model"_tr()).c_str())) return;
    label("playback.refactorEditor.layers.modelFile"_tr());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(model.file.empty() ? "-" : model.file.c_str());
    ImGui::PopTextWrapPos();
    if (property::actionButton("playback.refactorEditor.layers.browse"_tr().c_str())) {
        if (auto const file = overlays::pickModelFile()) model.file = *file;
    }
    if (model.file != mModelErrorFile) {
        mModelErrorFile = model.file;
        mModelError.clear();
        if (!model.file.empty()) (void)overlay::probeMimodel(model.file, mModelError);
    }
    if (!mModelError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::kError);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(mModelError.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    keyRow(item, "model-x", ICON_POSITION, "X", "modelX", 0.0f, 0.0f, "%.2f");
    keyRow(item, "model-y", ICON_POSITION, "Y", "modelY", 0.0f, 0.0f, "%.2f");
    keyRow(item, "model-z", ICON_POSITION, "Z", "modelZ", 0.0f, 0.0f, "%.2f");
    if (property::actionButton("playback.refactorEditor.layers.placeInView"_tr().c_str())) {
        if (auto const point = overlays::pointInFrontOfCamera(4.0f)) {
            setKeyed(item, "modelX", point->x);
            setKeyed(item, "modelY", point->y);
            setKeyed(item, "modelZ", point->z);
        }
    }
    keyRow(item, "yaw", ICON_ROTATE, "playback.refactorEditor.layers.yaw"_tr(), "yaw", -180.0f, 180.0f, "%.1f°");
    keyRow(item, "pitch", ICON_ROTATE, "playback.refactorEditor.layers.pitch"_tr(), "pitch", -180.0f, 180.0f, "%.1f°");
    keyRow(item, "roll", ICON_ROTATE, "playback.refactorEditor.layers.roll"_tr(), "roll", -180.0f, 180.0f, "%.1f°");
    keyRow(item, "scale", ICON_SCALE, "playback.refactorEditor.layers.scale"_tr(), "scale", 0.01f, 16.0f, "%.2fx");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "playback.refactorEditor.layers.modelHint"_tr().c_str());
    ImGui::PopTextWrapPos();
    property::endSection();
}


void LayerInspector::keyRow(
    OverlayItem&       item,
    char const*        id,
    char const*        icon,
    std::string const& text,
    char const*        name,
    float              low,
    float              high,
    char const*        format,
    float              displayScale
) {
    float* field = state::editing::model::overlayProperty(item, name);
    if (!field) return;
    auto const result =
        keyframes::propertyRow(id, icon, text, item.channels, name, *field, mLocalTick, low, high, format, displayScale);
    if (result.seek) mSeek = result.seek;
}

void LayerInspector::setKeyed(OverlayItem& item, char const* name, float value) {
    if (state::editing::model::hasKeys(item.channels, name)) {
        item.channels[name][mLocalTick].value = value;
    } else if (float* field = state::editing::model::overlayProperty(item, name)) {
        *field = value;
    }
}
} // namespace playback::editor::ui
