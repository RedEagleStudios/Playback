#include "LayerInspector.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/KeyframeControls.h"
#include "playback/editor/ui/components/OverlayEditing.h"
#include "playback/editor/ui/components/PlayerPicker.h"
#include "playback/editor/ui/components/PropertyControls.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/overlay/MimodelRenderer.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/overlay/OverlayRenderer.h"
#include "playback/overlay/VanillaAssets.h"

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
        copyInto(mNameTagBuffer, stored->standIn.nameTag);
        copyInto(mMobBuffer, stored->standIn.mob);
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
    case OverlayKind::Blur:
        drawBlurSection(item);
        break;
    case OverlayKind::Light:
        drawLightSection(item);
        break;
    case OverlayKind::StandIn:
        drawStandInSection(ctx, item);
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
    bool const vanilla = overlay::vanilla::isAsset(model.file);
    if (!property::beginSection(((vanilla ? ICON_VANILLA "  " : ICON_MODEL "  ") + "playback.refactorEditor.layers.model"_tr()).c_str()))
        return;
    label(vanilla ? "playback.refactorEditor.layers.vanillaAsset"_tr() : "playback.refactorEditor.layers.modelFile"_tr());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(
        model.file.empty() ? "-" : vanilla ? overlay::vanilla::displayName(model.file).c_str() : model.file.c_str()
    );
    ImGui::PopTextWrapPos();
    // A layer can show a Mine-imator file or one of Minecraft's own blocks and items; either can replace the other.
    if (property::actionButton((ICON_VANILLA "  " + "playback.refactorEditor.layers.chooseVanilla"_tr()).c_str())) {
        widgets::openAnchoredPopup("##layer-vanilla-picker");
    }
    if (std::string asset; mVanillaPicker.draw("##layer-vanilla-picker", asset)) model.file = asset;
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

void LayerInspector::drawLightSection(OverlayItem& item) {
    using state::editing::model::LightStyle;
    auto& light = item.light;
    if (!property::beginSection((ICON_LIGHT_POINT "  " + "playback.refactorEditor.layers.light"_tr()).c_str())) return;

    label("playback.refactorEditor.layers.lightStyle"_tr());
    std::string const current = std::string(overlays::lightStyleIcon(light.style)) + "  " + overlays::lightStyleName(light.style);
    if (widgets::beginCombo("##light-style", current.c_str())) {
        for (auto const style : {LightStyle::Point, LightStyle::Spot, LightStyle::Strip, LightStyle::Ambient}) {
            std::string const name = std::string(overlays::lightStyleIcon(style)) + "  " + overlays::lightStyleName(style);
            if (ImGui::Selectable(name.c_str(), style == light.style)) light.style = style;
        }
        widgets::endCombo();
    }
    // Level in percent of the picture's own brightness: 100% doubles it, negative darkens.
    keyRow(item, "light-level", ICON_EXPOSURE, "playback.refactorEditor.layers.lightLevel"_tr(), "lightLevel", -100.0f, 2000.0f, "%+.0f%%", 100.0f);
    keyRow(item, "light-hardness", nullptr, "playback.refactorEditor.layers.lightHardness"_tr(), "lightHardness", 0.0f, 100.0f, "%.0f%%", 100.0f);
    keyRow(item, "light-kelvin", ICON_TEMPERATURE, "playback.refactorEditor.layers.lightKelvin"_tr(), "lightKelvin", 1000.0f, 12000.0f, "%.0f K");
    auto const filter = keyframes::colorRow(
        "light-filter",
        ICON_PALETTE,
        "playback.refactorEditor.layers.lightFilter"_tr(),
        item.channels,
        "lightFilter",
        light.filter,
        mLocalTick
    );
    if (filter.seek) mSeek = filter.seek;

    if (light.style != LightStyle::Ambient) {
        keyRow(item, "x", ICON_POSITION, "playback.refactorEditor.layers.positionX"_tr(), "x", -50.0f, 150.0f, "%.1f%%", 100.0f);
        keyRow(item, "y", ICON_POSITION, "playback.refactorEditor.layers.positionY"_tr(), "y", -50.0f, 150.0f, "%.1f%%", 100.0f);
        keyRow(item, "rotation", ICON_ROTATE, "playback.refactorEditor.layers.rotation"_tr(), "rotation", -180.0f, 180.0f, "%.1f°");
    }
    switch (light.style) {
    case LightStyle::Point:
        keyRow(item, "width", ICON_SCALE, "playback.refactorEditor.layers.width"_tr(), "width", 0.5f, 400.0f, "%.1f%%", 100.0f);
        keyRow(item, "height", ICON_SCALE, "playback.refactorEditor.layers.height"_tr(), "height", 0.5f, 400.0f, "%.1f%%", 100.0f);
        break;
    case LightStyle::Spot:
        keyRow(item, "light-spread", nullptr, "playback.refactorEditor.layers.lightSpread"_tr(), "lightSpread", 1.0f, 89.0f, "%.1f°");
        keyRow(item, "height", ICON_SCALE, "playback.refactorEditor.layers.lightReach"_tr(), "height", 0.5f, 400.0f, "%.1f%%", 100.0f);
        keyRow(item, "width", ICON_SCALE, "playback.refactorEditor.layers.lightSource"_tr(), "width", 0.0f, 100.0f, "%.1f%%", 100.0f);
        break;
    case LightStyle::Strip:
        keyRow(item, "height", ICON_SCALE, "playback.refactorEditor.layers.lightThickness"_tr(), "height", 0.5f, 400.0f, "%.1f%%", 100.0f);
        break;
    case LightStyle::Ambient:
        break;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "playback.refactorEditor.layers.lightHint"_tr().c_str());
    ImGui::PopTextWrapPos();
    property::endSection();
}

void LayerInspector::drawStandInSection(PanelContext const& ctx, OverlayItem& item) {
    using state::editing::model::StandInLook;
    auto& standIn = item.standIn;
    if (!property::beginSection((ICON_STANDIN "  " + "playback.refactorEditor.standIn.title"_tr()).c_str())) return;

    // What it plays: the source player from the recording's sourceTick, one recorded tick per tick of the layer.
    label("playback.refactorEditor.standIn.source"_tr());
    ImGui::TextUnformatted(
        (standIn.subjectLabel.empty() ? playerSubjectLabel(ctx.state.replayPlayers, standIn.subject) : standIn.subjectLabel)
            .c_str()
    );
    int source = standIn.sourceTick;
    if (seconds("playback.refactorEditor.standIn.sourceStart"_tr(), "##standin-source", source, 0, std::max(0, ctx.state.totalTicks - 1))) {
        standIn.sourceTick = source;
    }
    widgets::itemTooltip("playback.refactorEditor.standIn.sourceStartHint"_tr().c_str());

    label("playback.refactorEditor.standIn.look"_tr());
    std::string const looks[] = {
        "playback.refactorEditor.standIn.lookSame"_tr(),
        "playback.refactorEditor.standIn.lookPlayer"_tr(),
        "playback.refactorEditor.standIn.lookFile"_tr(),
        "playback.refactorEditor.standIn.lookMob"_tr(),
    };
    int look = static_cast<int>(standIn.look);
    if (widgets::beginCombo("##standin-look", looks[std::clamp(look, 0, 3)].c_str())) {
        for (int index = 0; index < 4; ++index) {
            if (ImGui::Selectable(looks[index].c_str(), index == look)) standIn.look = static_cast<StandInLook>(index);
        }
        widgets::endCombo();
    }
    switch (standIn.look) {
    case StandInLook::SameSkin:
        break;
    case StandInLook::PlayerSkin: {
        label("playback.refactorEditor.standIn.skinOf"_tr(playerSubjectLabel(ctx.state.replayPlayers, standIn.skinPlayer)));
        PlayerPickerResult picked;
        if (drawPlayerList(
                "##standin-skin-player",
                PlayerPickerMode::SegmentSubject,
                ctx.state.replayPlayers,
                standIn.skinPlayer,
                false,
                picked
            )
            && picked.picked) {
            standIn.skinPlayer = picked.uuid;
        }
        break;
    }
    case StandInLook::SkinFile:
        label("playback.refactorEditor.standIn.skinFile"_tr());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(standIn.skinFile.empty() ? "-" : standIn.skinFile.c_str());
        ImGui::PopTextWrapPos();
        if (property::actionButton("playback.refactorEditor.layers.browse"_tr().c_str())) {
            if (auto const file = overlays::pickSkinFile()) standIn.skinFile = *file;
        }
        toggle("##standin-slim", ICON_USER, "playback.refactorEditor.standIn.slim"_tr(), standIn.slim);
        break;
    case StandInLook::Mob: {
        label("playback.refactorEditor.standIn.mob"_tr());
        // Common mobs to pick from; any entity type can be typed.
        static constexpr std::array<char const*, 20> Mobs{
            "minecraft:zombie",        "minecraft:skeleton",  "minecraft:creeper",  "minecraft:spider",
            "minecraft:enderman",      "minecraft:villager_v2", "minecraft:iron_golem", "minecraft:pillager",
            "minecraft:vindicator",    "minecraft:witch",     "minecraft:piglin",   "minecraft:wither_skeleton",
            "minecraft:husk",          "minecraft:drowned",   "minecraft:wolf",     "minecraft:fox",
            "minecraft:pig",           "minecraft:cow",       "minecraft:sheep",    "minecraft:allay",
        };
        if (widgets::beginCombo("##standin-mob", standIn.mob.c_str())) {
            for (char const* mob : Mobs) {
                if (ImGui::Selectable(mob, standIn.mob == mob)) {
                    standIn.mob = mob;
                    copyInto(mMobBuffer, standIn.mob);
                }
            }
            widgets::endCombo();
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##standin-mob-id", "minecraft:zombie", mMobBuffer.data(), mMobBuffer.size())
            && mMobBuffer[0] != '\0') {
            standIn.mob = mMobBuffer.data();
        }
        break;
    }
    }

    // What it wears and holds: as recorded, nothing (armour off shows the skin), or chosen items.
    using state::editing::model::StandInGear;
    std::string const gears[] = {
        "playback.refactorEditor.standIn.gearRecorded"_tr(),
        "playback.refactorEditor.standIn.gearNone"_tr(),
        "playback.refactorEditor.standIn.gearCustom"_tr(),
    };
    auto const gearCombo = [&](char const* id, std::string const& text, StandInGear& gear) {
        label(text);
        int current = static_cast<int>(gear);
        if (widgets::beginCombo(id, gears[std::clamp(current, 0, 2)].c_str())) {
            for (int index = 0; index < 3; ++index) {
                if (ImGui::Selectable(gears[index].c_str(), index == current)) gear = static_cast<StandInGear>(index);
            }
            widgets::endCombo();
        }
    };
    // A slot's item: common ones to pick, or any item id typed in; "-" is an empty slot.
    auto const itemField = [&](char const* id, std::string const& text, std::string& value,
                               std::initializer_list<char const*> suggestions) {
        ImGui::TextDisabled("%s", text.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (!widgets::beginCombo(id, value.empty() ? "-" : value.c_str())) return;
        if (ImGui::IsWindowAppearing()) copyInto(mGearBuffer, value);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##item-id", "minecraft:...", mGearBuffer.data(), mGearBuffer.size(),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            value = mGearBuffer.data();
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::Selectable("-", value.empty())) value.clear();
        for (char const* suggestion : suggestions) {
            if (ImGui::Selectable(suggestion, value == suggestion)) value = suggestion;
        }
        widgets::endCombo();
    };
    gearCombo("##standin-armor", "playback.refactorEditor.standIn.armor"_tr(), standIn.armor);
    if (standIn.armor == StandInGear::Custom) {
        itemField("##standin-head", "playback.refactorEditor.standIn.head"_tr(), standIn.armorItems[0],
                  {"minecraft:leather_helmet", "minecraft:chainmail_helmet", "minecraft:iron_helmet",
                   "minecraft:golden_helmet", "minecraft:diamond_helmet", "minecraft:netherite_helmet",
                   "minecraft:turtle_helmet", "minecraft:carved_pumpkin"});
        itemField("##standin-chest", "playback.refactorEditor.standIn.chest"_tr(), standIn.armorItems[1],
                  {"minecraft:leather_chestplate", "minecraft:chainmail_chestplate", "minecraft:iron_chestplate",
                   "minecraft:golden_chestplate", "minecraft:diamond_chestplate", "minecraft:netherite_chestplate",
                   "minecraft:elytra"});
        itemField("##standin-legs", "playback.refactorEditor.standIn.legs"_tr(), standIn.armorItems[2],
                  {"minecraft:leather_leggings", "minecraft:chainmail_leggings", "minecraft:iron_leggings",
                   "minecraft:golden_leggings", "minecraft:diamond_leggings", "minecraft:netherite_leggings"});
        itemField("##standin-feet", "playback.refactorEditor.standIn.feet"_tr(), standIn.armorItems[3],
                  {"minecraft:leather_boots", "minecraft:chainmail_boots", "minecraft:iron_boots",
                   "minecraft:golden_boots", "minecraft:diamond_boots", "minecraft:netherite_boots"});
    }
    gearCombo("##standin-hands", "playback.refactorEditor.standIn.hands"_tr(), standIn.hands);
    if (standIn.hands == StandInGear::Custom) {
        itemField("##standin-main-hand", "playback.refactorEditor.standIn.mainHand"_tr(), standIn.mainHand,
                  {"minecraft:diamond_sword", "minecraft:netherite_sword", "minecraft:iron_sword", "minecraft:bow",
                   "minecraft:crossbow", "minecraft:trident", "minecraft:mace", "minecraft:diamond_pickaxe",
                   "minecraft:diamond_axe", "minecraft:torch", "minecraft:fishing_rod", "minecraft:totem_of_undying"});
        itemField("##standin-off-hand", "playback.refactorEditor.standIn.offHand"_tr(), standIn.offHand,
                  {"minecraft:shield", "minecraft:totem_of_undying", "minecraft:torch", "minecraft:arrow",
                   "minecraft:map"});
    }

    label("playback.refactorEditor.standIn.nameTag"_tr());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint(
            "##standin-name-tag",
            "playback.refactorEditor.standIn.nameTagHint"_tr().c_str(),
            mNameTagBuffer.data(),
            mNameTagBuffer.size()
        )) {
        standIn.nameTag = mNameTagBuffer.data();
    }

    // Where it plays, from where the source stood: moved in blocks and turned about that spot.
    keyRow(item, "standin-x", ICON_POSITION, "playback.refactorEditor.standIn.offsetX"_tr(), "standInX", 0.0f, 0.0f, "%+.2f");
    keyRow(item, "standin-y", ICON_POSITION, "playback.refactorEditor.standIn.offsetY"_tr(), "standInY", 0.0f, 0.0f, "%+.2f");
    keyRow(item, "standin-z", ICON_POSITION, "playback.refactorEditor.standIn.offsetZ"_tr(), "standInZ", 0.0f, 0.0f, "%+.2f");
    keyRow(item, "standin-turn", ICON_ROTATE, "playback.refactorEditor.standIn.turn"_tr(), "standInTurn", -180.0f, 180.0f, "%.1f°");
    toggle("##standin-blocks", ICON_VANILLA, "playback.refactorEditor.standIn.blocks"_tr(), standIn.blocks);
    widgets::itemTooltip("playback.refactorEditor.standIn.blocksHint"_tr().c_str());

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "playback.refactorEditor.standIn.layerHint"_tr().c_str());
    ImGui::PopTextWrapPos();
    property::endSection();
}

void LayerInspector::drawBlurSection(OverlayItem& item) {
    using state::editing::model::BlurArea;
    using state::editing::model::BlurStyle;
    auto& blur = item.blur;
    if (!property::beginSection((ICON_BLUR "  " + "playback.refactorEditor.layers.blur"_tr()).c_str())) return;

    label("playback.refactorEditor.layers.blurStyle"_tr());
    std::string const current = std::string(overlays::blurStyleIcon(blur.style)) + "  " + overlays::blurStyleName(blur.style);
    if (widgets::beginCombo("##blur-style", current.c_str())) {
        for (auto const style : {BlurStyle::Gaussian, BlurStyle::Mosaic, BlurStyle::Motion, BlurStyle::Radial}) {
            std::string const name = std::string(overlays::blurStyleIcon(style)) + "  " + overlays::blurStyleName(style);
            if (ImGui::Selectable(name.c_str(), style == blur.style)) blur.style = style;
        }
        widgets::endCombo();
    }
    // Lengths are in percent of the frame's height; Radial's amount is how far each pixel smears towards the centre.
    switch (blur.style) {
    case BlurStyle::Gaussian:
        keyRow(item, "blur-amount", ICON_BLUR, "playback.refactorEditor.layers.blurRadius"_tr(), "blurAmount", 0.0f, 20.0f, "%.1f%%", 100.0f);
        break;
    case BlurStyle::Mosaic:
        keyRow(item, "blur-amount", ICON_MOSAIC, "playback.refactorEditor.layers.blurBlock"_tr(), "blurAmount", 0.0f, 20.0f, "%.1f%%", 100.0f);
        break;
    case BlurStyle::Motion:
        keyRow(item, "blur-amount", ICON_MOTION, "playback.refactorEditor.layers.blurLength"_tr(), "blurAmount", 0.0f, 50.0f, "%.1f%%", 100.0f);
        keyRow(item, "blur-angle", ICON_ROTATE, "playback.refactorEditor.layers.blurAngle"_tr(), "blurAngle", -180.0f, 180.0f, "%.1f°");
        break;
    case BlurStyle::Radial:
        keyRow(item, "blur-amount", ICON_RADIAL, "playback.refactorEditor.layers.blurStrength"_tr(), "blurAmount", 0.0f, 100.0f, "%.0f%%", 100.0f);
        break;
    }

    label("playback.refactorEditor.layers.blurArea"_tr());
    std::string const areas[] = {
        "playback.refactorEditor.layers.blurAreaFrame"_tr(),
        "playback.refactorEditor.layers.rectangle"_tr(),
        "playback.refactorEditor.layers.ellipse"_tr(),
    };
    int area = static_cast<int>(blur.area);
    if (widgets::beginCombo("##blur-area", areas[std::clamp(area, 0, 2)].c_str())) {
        for (int index = 0; index < 3; ++index) {
            if (ImGui::Selectable(areas[index].c_str(), index == area)) blur.area = static_cast<BlurArea>(index);
        }
        widgets::endCombo();
    }
    bool const placed = blur.area != BlurArea::Frame;
    // A Radial blur zooms towards the area's centre, so the centre shows even over the whole frame.
    if (placed || blur.style == BlurStyle::Radial) {
        keyRow(item, "x", ICON_POSITION, "playback.refactorEditor.layers.positionX"_tr(), "x", -50.0f, 150.0f, "%.1f%%", 100.0f);
        keyRow(item, "y", ICON_POSITION, "playback.refactorEditor.layers.positionY"_tr(), "y", -50.0f, 150.0f, "%.1f%%", 100.0f);
    }
    if (placed) {
        keyRow(item, "width", ICON_SCALE, "playback.refactorEditor.layers.width"_tr(), "width", 0.5f, 400.0f, "%.1f%%", 100.0f);
        keyRow(item, "height", ICON_SCALE, "playback.refactorEditor.layers.height"_tr(), "height", 0.5f, 400.0f, "%.1f%%", 100.0f);
        keyRow(item, "rotation", ICON_ROTATE, "playback.refactorEditor.layers.rotation"_tr(), "rotation", -180.0f, 180.0f, "%.1f°");
        keyRow(item, "blur-feather", nullptr, "playback.refactorEditor.layers.blurFeather"_tr(), "blurFeather", 0.0f, 20.0f, "%.1f%%", 100.0f);
        toggle("##blur-invert", ICON_HIDE, "playback.refactorEditor.layers.blurInvert"_tr(), blur.invert);
        widgets::itemTooltip("playback.refactorEditor.layers.blurInvertHint"_tr().c_str());
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "playback.refactorEditor.layers.blurHint"_tr().c_str());
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
    // As an edit in a row: a key at the playhead, and the value without keys follows it.
    if (float* field = state::editing::model::overlayProperty(item, name)) *field = value;
    item.channels[name][mLocalTick].value = value;
}

} // namespace playback::editor::ui
