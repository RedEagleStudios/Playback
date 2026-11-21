#include "playback/editor/ui/components/Widgets.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <optional>
#include <string>
#include <unordered_map>

#include <type_traits>

namespace playback::editor::ui::widgets {

using namespace ll::i18n_literals;

namespace {

// The icon glyphs live in the 3-byte UTF-8 range; ImGui's own decoder is internal-only.
[[nodiscard]] std::optional<unsigned int> decodeUtf8(char const* text, int& length) {
    length = 1;
    if (!text || *text == '\0') return std::nullopt;
    auto const* bytes = reinterpret_cast<unsigned char const*>(text);
    if (bytes[0] < 0x80) return bytes[0];
    if ((bytes[0] & 0xE0) == 0xC0 && (bytes[1] & 0xC0) == 0x80) {
        length = 2;
        return static_cast<unsigned int>((bytes[0] & 0x1F) << 6 | (bytes[1] & 0x3F));
    }
    if ((bytes[0] & 0xF0) == 0xE0 && (bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80) {
        length = 3;
        return static_cast<unsigned int>((bytes[0] & 0x0F) << 12 | (bytes[1] & 0x3F) << 6 | (bytes[2] & 0x3F));
    }
    if ((bytes[0] & 0xF8) == 0xF0 && (bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80
        && (bytes[3] & 0xC0) == 0x80) {
        length = 4;
        return static_cast<unsigned int>(
            (bytes[0] & 0x07) << 18 | (bytes[1] & 0x3F) << 12 | (bytes[2] & 0x3F) << 6 | (bytes[3] & 0x3F)
        );
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<unsigned int> decodeUtf8(char const* text) {
    int length = 1;
    return decodeUtf8(text, length);
}

struct InkBounds {
    float y0;
    float y1;
};

// Union of glyph ink over a run; CJK ideographs sit below the Latin cap height that 'H' alone reports.
[[nodiscard]] std::optional<InkBounds> textInkBounds(char const* text) {
    if (!text) return std::nullopt;
    ImFontBaked* baked = ImGui::GetFont()->GetFontBaked(ImGui::GetFontSize());
    if (!baked) return std::nullopt;
    std::optional<InkBounds> bounds;
    for (char const* cursor = text; *cursor != '\0';) {
        int        length     = 1;
        auto const codepoint  = decodeUtf8(cursor, length);
        cursor               += length;
        if (!codepoint || *codepoint == ' ') continue;
        ImFontGlyph const* glyph = baked->FindGlyphNoFallback(static_cast<ImWchar>(*codepoint));
        if (!glyph || !glyph->Visible) continue;
        if (!bounds) bounds = InkBounds{glyph->Y0, glyph->Y1};
        else {
            bounds->y0 = std::min(bounds->y0, glyph->Y0);
            bounds->y1 = std::max(bounds->y1, glyph->Y1);
        }
    }
    return bounds;
}

} // namespace

std::string formatTick(int tick) {
    char value[32]{};
    tick                   = std::max(0, tick);
    int const totalSeconds = tick / kTicksPerSecond;
    int const centiseconds = tick % kTicksPerSecond * (100 / kTicksPerSecond);
    std::snprintf(value, sizeof(value), "%02d:%02d.%02d", totalSeconds / 60, totalSeconds % 60, centiseconds);
    return value;
}

std::string formatTickCompact(int tick) {
    char value[32]{};
    tick                   = std::max(0, tick);
    int const totalSeconds = tick / kTicksPerSecond;
    std::snprintf(value, sizeof(value), "%d:%02d", totalSeconds / 60, totalSeconds % 60);
    return value;
}

float iconButtonSize() { return metrics::iconButton(); }

float iconGlyphSize() { return metrics::iconGlyph(); }

void itemTooltip(char const* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    hoverTooltip(text);
}

void hoverTooltip(char const* text) {
    if (!text) return;

    // Panels are separate windows, so a real tooltip window can land behind one; this list draws last.
    ImVec2 const pad     = {6.0f * metrics::scale(), 4.0f * metrics::scale()};
    ImVec2 const display = ImGui::GetIO().DisplaySize;
    // Long hints wrap into a paragraph rather than one line wider than the screen.
    float const  wrapWidth = std::max(1.0f, std::min(metrics::font() * 26.0f, display.x - pad.x * 4.0f));
    ImVec2 const textSize  = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);
    ImVec2 const mouse     = ImGui::GetMousePos();
    float const  width     = textSize.x + pad.x * 2.0f;
    float const  height    = textSize.y + pad.y * 2.0f;
    float const  offset    = 16.0f * metrics::scale();
    ImVec2       origin{mouse.x + offset, mouse.y + offset};
    if (origin.x + width > display.x) origin.x = std::max(0.0f, mouse.x - width - offset * 0.5f);
    if (origin.y + height > display.y) origin.y = std::max(0.0f, mouse.y - height - offset * 0.5f);

    // Lifted well above the panel greys with a shadow: at kBgHeader the box vanished against the panels.
    auto*        drawList = ImGui::GetForegroundDrawList();
    ImVec2 const max{origin.x + width, origin.y + height};
    float const  shadow = 3.0f * metrics::scale();
    drawList->AddRectFilled(
        {origin.x + shadow * 0.5f, origin.y + shadow},
        {max.x + shadow * 0.5f, max.y + shadow},
        theme::kTooltipShadow,
        theme::kFrameRounding * 2.0f
    );
    drawList->AddRectFilled(origin, max, theme::kTooltipBg, theme::kFrameRounding * 2.0f);
    drawList->AddRect(origin, max, theme::kTooltipBorder, theme::kFrameRounding * 2.0f);
    drawList->AddText(nullptr, 0.0f, {origin.x + pad.x, origin.y + pad.y}, theme::kText, text, nullptr, wrapWidth);
}

namespace {

struct ButtonFrame {
    ImVec2 origin;
    float  size;
    bool   clicked;
    ImU32  color;
};

// Square hit box with the shared hover/held/active fill; callers only add the glyph on top.
ButtonFrame squareButton(char const* id, bool enabled, bool active, float size = 0.0f) {
    ButtonFrame frame{};
    frame.size   = size > 0.0f ? size : iconButtonSize();
    frame.origin = ImGui::GetCursorScreenPos();

    ImGui::BeginDisabled(!enabled);
    frame.clicked      = ImGui::InvisibleButton(id, {frame.size, frame.size});
    bool const hovered = ImGui::IsItemHovered();
    bool const held    = ImGui::IsItemActive();
    ImGui::EndDisabled();

    ImU32 fill = active ? theme::withAlpha(theme::kAccent, 0x59) : IM_COL32(0, 0, 0, 0);
    if (enabled && held) fill = active ? theme::kAccent : theme::kButtonActive;
    else if (enabled && hovered)
        fill = active ? theme::withAlpha(theme::kAccent, 0x80) : theme::withAlpha(theme::kButtonHover, 0x80);

    if ((fill & IM_COL32_A_MASK) != 0) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            frame.origin,
            {frame.origin.x + frame.size, frame.origin.y + frame.size},
            fill,
            theme::kFrameRounding
        );
    }
    frame.color = !enabled ? theme::kTextDim : (hovered || active) ? theme::kIconHighlight : theme::kIconInactive;
    return frame;
}

// ImGui::Button centres by advance width, which drifts for monospaced icon glyphs.
bool iconButtonImpl(char const* id, char const* icon, char const* tooltip, bool enabled, bool active, float size = 0.0f) {
    ButtonFrame const frame = squareButton(id, enabled, active, size);
    // A compact button keeps the glyph in the same proportion to its box as the standard one.
    float const glyph = size > 0.0f ? size * metrics::iconGlyph() / iconButtonSize() : 0.0f;
    drawIconCentred(ImGui::GetWindowDrawList(), icon, frame.origin, frame.size, frame.color, glyph);
    itemTooltip(tooltip);
    return frame.clicked;
}

// Play-style triangles: the centroid, not the bounding box, is what the eye reads as the centre.
void addTriangle(
    ImDrawList*   drawList,
    ImVec2 const& c,
    float         left,
    float         right,
    float         h,
    bool          pointsRight,
    ImU32         color
) {
    if (pointsRight) {
        drawList->AddTriangleFilled({c.x + left, c.y - h}, {c.x + left, c.y + h}, {c.x + right, c.y}, color);
    } else {
        drawList->AddTriangleFilled({c.x + right, c.y - h}, {c.x + right, c.y + h}, {c.x + left, c.y}, color);
    }
}

void addBar(ImDrawList* drawList, ImVec2 const& c, float left, float right, float h, ImU32 color) {
    float const rounding = (right - left) * 0.35f;
    drawList->AddRectFilled({c.x + left, c.y - h}, {c.x + right, c.y + h}, color, rounding);
}

} // namespace

float textOffsetInBox(float boxHeight) {
    float const fontSize = ImGui::GetFontSize();
    if (ImFontBaked* baked = ImGui::GetFont()->GetFontBaked(fontSize)) {
        if (ImFontGlyph const* cap = baked->FindGlyphNoFallback('H')) {
            return std::floor((boxHeight - (cap->Y0 + cap->Y1)) * 0.5f + 0.5f);
        }
    }
    return std::floor((boxHeight - fontSize) * 0.5f + 0.5f);
}

float textOffsetInBox(float boxHeight, char const* text) {
    if (auto const ink = textInkBounds(text)) return std::floor((boxHeight - (ink->y0 + ink->y1)) * 0.5f + 0.5f);
    return textOffsetInBox(boxHeight);
}

float textYForCentre(float centreY, char const* text) {
    if (auto const ink = textInkBounds(text)) return std::floor(centreY - (ink->y0 + ink->y1) * 0.5f + 0.5f);
    return std::floor(centreY - ImGui::GetFontSize() * 0.5f + 0.5f);
}

float textInkCentre(float drawY, char const* text) {
    // Optical centre is the middle of the x-height nudged down; ascenders pull the ink box too high.
    if (ImFontBaked* baked = ImGui::GetFont()->GetFontBaked(ImGui::GetFontSize())) {
        if (ImFontGlyph const* x = baked->FindGlyphNoFallback('x')) {
            return drawY + (x->Y0 + x->Y1) * 0.5f + (x->Y1 - x->Y0) * 0.14f;
        }
    }
    if (auto const ink = textInkBounds(text)) return drawY + (ink->y0 + ink->y1) * 0.5f;
    return drawY + ImGui::GetFontSize() * 0.5f;
}

void drawVectorIcon(ImDrawList* drawList, ImVec2 const& centre, float boxSize, ImU32 color, VectorIcon icon) {
    // Tuned to land at the same ink height as the Lucide glyphs beside them.
    float const h      = boxSize * 0.285f;
    float const stroke = std::max(1.5f, boxSize * 0.045f);
    switch (icon) {
    case VectorIcon::Play:
        addTriangle(drawList, centre, -h * 0.62f, h * 1.24f, h, true, color);
        break;
    case VectorIcon::Pause:
        addBar(drawList, centre, -h * 0.9f, -h * 0.3f, h, color);
        addBar(drawList, centre, h * 0.3f, h * 0.9f, h, color);
        break;
    case VectorIcon::StepBack:
        addTriangle(drawList, centre, -h * 1.15f, -h * 0.05f, h * 0.85f, false, color);
        addTriangle(drawList, centre, h * 0.05f, h * 1.15f, h * 0.85f, false, color);
        break;
    case VectorIcon::StepForward:
        addTriangle(drawList, centre, -h * 1.15f, -h * 0.05f, h * 0.85f, true, color);
        addTriangle(drawList, centre, h * 0.05f, h * 1.15f, h * 0.85f, true, color);
        break;
    case VectorIcon::SkipStart:
        addBar(drawList, centre, -h * 1.15f, -h * 0.75f, h, color);
        addTriangle(drawList, centre, -h * 0.5f, h * 1.15f, h, false, color);
        break;
    case VectorIcon::SkipEnd:
        addTriangle(drawList, centre, -h * 1.15f, h * 0.5f, h, true, color);
        addBar(drawList, centre, h * 0.75f, h * 1.15f, h, color);
        break;
    case VectorIcon::Maximize: {
        float const r = h * 0.95f;
        drawList->AddRect({centre.x - r, centre.y - r}, {centre.x + r, centre.y + r}, color, stroke, 0, stroke);
        break;
    }
    case VectorIcon::Restore: {
        // Two offset frames, the front one filled so the overlap reads as depth rather than a grid.
        float const r = h * 0.7f;
        float const o = h * 0.35f;
        drawList->AddRect(
            {centre.x - r + o, centre.y - r - o},
            {centre.x + r + o, centre.y + r - o},
            color,
            stroke,
            0,
            stroke
        );
        drawList->AddRectFilled(
            {centre.x - r - o, centre.y - r + o},
            {centre.x + r - o, centre.y + r + o},
            theme::kBgHeader,
            stroke
        );
        drawList->AddRect(
            {centre.x - r - o, centre.y - r + o},
            {centre.x + r - o, centre.y + r + o},
            color,
            stroke,
            0,
            stroke
        );
        break;
    }
    case VectorIcon::ShadingFlat:
        drawList->AddCircleFilled(centre, h * 1.05f, color, 32);
        break;
    case VectorIcon::ShadingLit: {
        // The lit half filled, the shadow half only outlined.
        float const quarter = std::numbers::pi_v<float> * 0.5f;
        drawList->AddCircle(centre, h * 1.05f - stroke * 0.5f, color, 32, stroke);
        drawList->PathArcTo(centre, h * 1.05f, -quarter, quarter, 16);
        drawList->PathFillConvex(color);
        break;
    }
    case VectorIcon::ShadingRendered:
        // A sphere's outline with its shaded body pulled towards the light's far side.
        drawList->AddCircle(centre, h * 1.05f - stroke * 0.5f, color, 32, stroke);
        drawList->AddCircleFilled({centre.x + h * 0.25f, centre.y + h * 0.25f}, h * 0.68f, color, 32);
        break;
    }
}

bool vectorIconButton(char const* id, VectorIcon icon, char const* tooltip, bool active, bool enabled) {
    ButtonFrame const frame = squareButton(id, enabled, active);
    drawVectorIcon(
        ImGui::GetWindowDrawList(),
        {frame.origin.x + frame.size * 0.5f, frame.origin.y + frame.size * 0.5f},
        frame.size,
        frame.color,
        icon
    );
    itemTooltip(tooltip);
    return frame.clicked;
}

void drawIconCentred(
    ImDrawList*   drawList,
    char const*   icon,
    ImVec2 const& origin,
    float         boxSize,
    ImU32         color,
    float         glyphSize
) {
    float const iconSize = glyphSize > 0.0f ? glyphSize : iconGlyphSize();
    // CalcTextSize reports the line box; the glyph's own X0/Y0/X1/Y1 are the ink bounds to centre on.
    ImFontGlyph const* glyph = nullptr;
    if (auto const codepoint = decodeUtf8(icon)) {
        if (ImFontBaked* baked = ImGui::GetFont()->GetFontBaked(iconSize)) {
            glyph = baked->FindGlyphNoFallback(static_cast<ImWchar>(*codepoint));
        }
    }

    ImVec2 position{origin.x, origin.y};
    if (glyph) {
        position.x += (boxSize - (glyph->X1 + glyph->X0)) * 0.5f;
        position.y += (boxSize - (glyph->Y1 + glyph->Y0)) * 0.5f;
        // Half-pixel positions blur the glyph and read as misalignment next to snapped text.
        position.y = std::floor(position.y + 0.5f);
    } else {
        ImVec2 const fallback  = ImGui::CalcTextSize(icon);
        position.x            += (boxSize - fallback.x) * 0.5f;
        position.y            += (boxSize - fallback.y) * 0.5f;
    }
    drawList->AddText(ImGui::GetFont(), iconSize, position, color, icon);
}

void drawIconAtCentre(ImDrawList* drawList, char const* icon, ImVec2 const& centre, ImU32 color, float glyphSize) {
    float const        iconSize = glyphSize > 0.0f ? glyphSize : iconGlyphSize();
    ImFontGlyph const* glyph    = nullptr;
    if (auto const codepoint = decodeUtf8(icon)) {
        if (ImFontBaked* baked = ImGui::GetFont()->GetFontBaked(iconSize)) {
            glyph = baked->FindGlyphNoFallback(static_cast<ImWchar>(*codepoint));
        }
    }
    if (!glyph) {
        ImVec2 const fallback = ImGui::CalcTextSize(icon);
        drawIconCentred(
            drawList,
            icon,
            {centre.x - fallback.x * 0.5f, centre.y - fallback.y * 0.5f},
            0.0f,
            color,
            iconSize
        );
        return;
    }
    ImVec2 const position{
        std::floor(centre.x - (glyph->X0 + glyph->X1) * 0.5f + 0.5f),
        std::floor(centre.y - (glyph->Y0 + glyph->Y1) * 0.5f + 0.5f)
    };
    drawList->AddText(ImGui::GetFont(), iconSize, position, color, icon);
}

bool iconButton(char const* id, char const* icon, char const* tooltip, bool enabled) {
    return iconButtonImpl(id, icon, tooltip, enabled, false);
}

bool iconToggle(char const* id, char const* icon, char const* tooltip, bool active, bool enabled) {
    return iconButtonImpl(id, icon, tooltip, enabled, active);
}

bool compactIconToggle(char const* id, char const* icon, char const* tooltip, bool active, float size) {
    return iconButtonImpl(id, icon, tooltip, true, active, size);
}

float dropdownChipWidth(char const* label, char const* icon) {
    float const padX    = 8.0f * metrics::scale();
    float const iconBox = icon ? metrics::iconGlyph() + padX * 0.5f : 0.0f;
    return padX + iconBox + ImGui::CalcTextSize(label).x + padX * 0.5f + metrics::iconButton() * 0.5f + padX * 0.5f;
}

bool dropdownChip(char const* id, char const* label, char const* tooltip, bool enabled, char const* icon) {
    float const  height    = metrics::iconButton();
    float const  padX      = 8.0f * metrics::scale();
    float const  iconBox   = icon ? metrics::iconGlyph() : 0.0f;
    float const  caretW    = height * 0.5f;
    float const  textWidth = ImGui::CalcTextSize(label).x;
    float const  width     = dropdownChipWidth(label, icon);
    ImVec2 const origin    = ImGui::GetCursorScreenPos();

    ImGui::BeginDisabled(!enabled);
    bool const clicked = ImGui::InvisibleButton(id, {width, height});
    bool const hovered = ImGui::IsItemHovered();
    bool const held    = ImGui::IsItemActive();
    ImGui::EndDisabled();

    auto*       drawList = ImGui::GetWindowDrawList();
    ImU32 const fill     = held ? theme::kInputBgActive : hovered ? theme::kInputBgHover : theme::kInputBg;
    drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, fill, theme::kFrameRounding);
    ImU32 const textColor = enabled ? theme::kText : theme::kTextDim;
    ImU32 const iconColor = enabled ? (hovered ? theme::kIconHighlight : theme::kIconInactive) : theme::kTextDim;

    // Icon aligns to the text's optical middle, not the chip's, so chips match timeline rows.
    ImVec2 const textPos{
        origin.x + padX + (icon ? iconBox + padX * 0.5f : 0.0f),
        textYForCentre(origin.y + height * 0.5f, label)
    };
    float x = origin.x + padX;
    if (icon) {
        drawIconAtCentre(drawList, icon, {x + iconBox * 0.5f, textInkCentre(textPos.y, label)}, iconColor);
        x += iconBox + padX * 0.5f;
    }
    drawList->AddText(textPos, textColor, label);
    x += textWidth + padX * 0.5f;

    // The caret is a stroked chevron so it matches the other vector marks instead of a text glyph.
    float const  stroke = std::max(1.5f, height * 0.045f);
    ImVec2 const c{x + caretW * 0.5f, origin.y + height * 0.5f};
    float const  w = height * 0.11f;
    drawList->AddLine({c.x - w, c.y - w * 0.5f}, {c.x, c.y + w * 0.5f}, iconColor, stroke);
    drawList->AddLine({c.x, c.y + w * 0.5f}, {c.x + w, c.y - w * 0.5f}, iconColor, stroke);

    itemTooltip(tooltip);
    return clicked;
}

namespace {

struct PopupAnchor {
    ImVec2 min;
    ImVec2 max;
    ImVec2 size; // Last measured popup size, so a menu that would run off screen can flip above its trigger.
};

std::unordered_map<ImGuiID, PopupAnchor>& popupAnchors() {
    static std::unordered_map<ImGuiID, PopupAnchor> anchors;
    return anchors;
}

// Whole pixels: a constrained popup size is truncated, so fractional padding would leave the popup a fraction shorter
// than its content and bring up a scrollbar.
[[nodiscard]] ImVec2 menuWindowPadding() {
    return {std::round(10.0f * metrics::scale()), std::round(6.0f * metrics::scale())};
}

void pushMenuItemSpacing() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10.0f * metrics::scale(), 6.0f * metrics::scale()});
}

} // namespace

void openAnchoredPopup(char const* popupId) {
    openAnchoredPopup(popupId, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

void openAnchoredPopup(char const* popupId, ImVec2 anchorMin, ImVec2 anchorMax) {
    popupAnchors()[ImGui::GetID(popupId)] = PopupAnchor{anchorMin, anchorMax, {}};
    ImGui::OpenPopup(popupId);
}

float anchoredPopupTriggerWidth(char const* popupId) {
    auto const anchor = popupAnchors().find(ImGui::GetID(popupId));
    if (anchor == popupAnchors().end()) return 0.0f;
    return std::max(0.0f, anchor->second.max.x - anchor->second.min.x);
}

bool beginAnchoredPopup(char const* popupId, float minWidth) {
    ImGuiID const id     = ImGui::GetID(popupId);
    auto&         store  = popupAnchors();
    auto const    anchor = store.find(id);
    if (anchor != store.end() && ImGui::IsPopupOpen(popupId)) {
        auto const*  viewport = ImGui::GetMainViewport();
        float const  gap      = 2.0f * metrics::scale();
        float const  width    = std::min(
            std::max(anchor->second.max.x - anchor->second.min.x, minWidth),
            std::max(1.0f, viewport->WorkSize.x)
        );
        ImVec2 const size    = anchor->second.size;
        ImVec2 const workMin = viewport->WorkPos;
        ImVec2 const workMax{viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y};
        ImVec2       pos{anchor->second.min.x, anchor->second.max.y + gap};
        // Menus near the bottom edge (the timeline transport) open upwards rather than off screen; one that fits on
        // neither side slides up just far enough to stay on screen.
        if (size.y > 0.0f && pos.y + size.y > workMax.y) {
            float const above = anchor->second.min.y - gap - size.y;
            pos.y             = above >= workMin.y ? above : std::max(workMin.y, workMax.y - size.y);
        }
        // The known minimum width keeps the first frame on screen before the real size is measured.
        float const right = std::max(size.x, width);
        if (pos.x + right > workMax.x) pos.x = std::max(workMin.x, workMax.x - right);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        // Never larger than the viewport: a menu with more items than fit scrolls instead of running off screen.
        ImGui::SetNextWindowSizeConstraints(
            {width, 0.0f},
            {std::max(width, viewport->WorkSize.x), std::max(1.0f, viewport->WorkSize.y)}
        );
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, menuWindowPadding());
    bool const open = ImGui::BeginPopup(popupId);
    ImGui::PopStyleVar();
    if (!open) {
        if (anchor != store.end() && !ImGui::IsPopupOpen(popupId)) store.erase(anchor);
        return false;
    }
    if (anchor != store.end()) anchor->second.size = ImGui::GetWindowSize();
    pushMenuItemSpacing();
    return true;
}

void endAnchoredPopup() {
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

bool beginMenu(char const* label, bool enabled) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, menuWindowPadding());
    bool const open = ImGui::BeginMenu(label, enabled);
    ImGui::PopStyleVar();
    if (open) pushMenuItemSpacing();
    return open;
}

void endMenu() {
    ImGui::PopStyleVar();
    ImGui::EndMenu();
}

bool beginCombo(char const* id, char const* preview) {
    // Combo popups take their horizontal padding from FramePadding.x to line up with the preview text.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, menuWindowPadding());
    ImGui::PushStyleVarX(ImGuiStyleVar_FramePadding, menuWindowPadding().x);
    bool const open = ImGui::BeginCombo(id, preview);
    ImGui::PopStyleVar(2);
    if (open) pushMenuItemSpacing();
    return open;
}

void endCombo() {
    ImGui::PopStyleVar();
    ImGui::EndCombo();
}

void noActiveProjectPlaceholder() {
    ImGui::TextDisabled("%s", "playback.refactorEditor.common.noActiveProject"_tr().c_str());
}

namespace {

// A number field dragged left and right, as in a 3D package: crossing the field's width sweeps a bounded value's
// whole range (Shift is faster, Alt finer), a double-click or Ctrl+click types a value, and a bounded field shows
// where the value sits in its range as a fill behind the number.
bool scrubber(
    char const*      id,
    ImGuiDataType    type,
    void*            value,
    double           current,
    void const*      low,
    void const*      high,
    double           lowValue,
    double           highValue,
    float            speed,
    char const*      format,
    ImGuiSliderFlags flags
) {
    bool const  bounded = highValue > lowValue;
    float const width   = ImGui::CalcItemWidth();
    if (speed <= 0.0f) {
        speed = bounded ? static_cast<float>((highValue - lowValue) / std::max(48.0, static_cast<double>(width))) : 0.05f;
        if (type == ImGuiDataType_S32) speed = std::max(speed, 0.02f);
    }
    auto*              drawList = ImGui::GetWindowDrawList();
    ImDrawListSplitter splitter;
    splitter.Split(drawList, 2);
    splitter.SetCurrentChannel(drawList, 1);
    // The frame is drawn underneath with the fill, so the number stays on top of both.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    bool const changed = ImGui::DragScalar(
        id,
        type,
        value,
        speed,
        bounded ? low : nullptr,
        bounded ? high : nullptr,
        format,
        flags | (bounded ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None)
    );
    ImGui::PopStyleColor(3);
    ImVec2 const min     = ImGui::GetItemRectMin();
    ImVec2 const max     = ImGui::GetItemRectMax();
    bool const   hovered = ImGui::IsItemHovered();
    bool const   active  = ImGui::IsItemActive();
    bool const   typing  = active && ImGui::GetIO().WantTextInput;

    splitter.SetCurrentChannel(drawList, 0);
    float const rounding = ImGui::GetStyle().FrameRounding;
    ImU32 const frame    = active ? theme::kInputBgActive : hovered ? theme::kInputBgHover : theme::kInputBg;
    drawList->AddRectFilled(min, max, ImGui::GetColorU32(frame), rounding);
    if (bounded && !typing) {
        double const t = std::clamp((current - lowValue) / (highValue - lowValue), 0.0, 1.0);
        if (t > 0.0) {
            ImU32 const fill = theme::withAlpha(theme::kAccent, active ? 0x78 : hovered ? 0x5c : 0x40);
            drawList->AddRectFilled(
                min,
                {min.x + (max.x - min.x) * static_cast<float>(t), max.y},
                ImGui::GetColorU32(fill),
                rounding
            );
        }
    }
    splitter.Merge(drawList);
    if ((hovered || active) && !typing) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    return changed;
}

} // namespace

bool dragFloat(char const* id, float* value, float low, float high, char const* format, float speed) {
    return scrubber(id, ImGuiDataType_Float, value, *value, &low, &high, low, high, speed, format, ImGuiSliderFlags_None);
}

bool dragInt(char const* id, int* value, int low, int high, char const* format, float speed) {
    return scrubber(id, ImGuiDataType_S32, value, *value, &low, &high, low, high, speed, format, ImGuiSliderFlags_None);
}

bool dragFloatN(char const* id, float* values, int count, float speed, char const* format, float low, float high) {
    // Each component is its own scrubber, side by side, as ImGui's DragFloat2/3 lay them out.
    float const spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    float const total   = ImGui::CalcItemWidth();
    float const each    = std::max(1.0f, (total - spacing * static_cast<float>(count - 1)) / static_cast<float>(count));
    bool        changed = false;
    ImGui::PushID(id);
    for (int index = 0; index < count; ++index) {
        if (index > 0) ImGui::SameLine(0.0f, spacing);
        ImGui::PushID(index);
        ImGui::SetNextItemWidth(each);
        changed |= dragFloat("##component", &values[index], low, high, format, speed);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

bool toggleRow(char const* id, char const* icon, char const* label, bool* value, char const* tooltip) {
    float const  scale   = metrics::scale();
    float const  height  = std::floor(ImGui::GetFrameHeight() + 4.0f * scale);
    ImVec2 const origin  = ImGui::GetCursorScreenPos();
    float const  width   = std::max(height * 3.0f, ImGui::GetContentRegionAvail().x);
    bool const   clicked = ImGui::InvisibleButton(id, {width, height});
    bool const   hovered = ImGui::IsItemHovered();
    if (clicked) *value = !*value;
    if (tooltip) itemTooltip(tooltip);

    auto*       drawList = ImGui::GetWindowDrawList();
    float const centreY  = std::floor(origin.y + height * 0.5f + 0.5f);
    if (hovered) {
        drawList->AddRectFilled(
            origin,
            {origin.x + width, origin.y + height},
            ImGui::GetColorU32(theme::withAlpha(theme::kButtonHover, 0x90)),
            theme::kFrameRounding * scale
        );
    }
    float       x     = origin.x + 4.0f * scale;
    float const glyph = std::floor(metrics::font() * 1.05f);
    if (icon) {
        ImU32 const iconColor = *value ? theme::kIconActive : theme::kIconInactive;
        drawIconAtCentre(drawList, icon, {x + glyph * 0.5f, centreY}, ImGui::GetColorU32(iconColor), glyph);
        x += glyph + 8.0f * scale;
    }
    // The switch: a flat pill, accent with the knob right when on, grey with the knob left when off.
    float const  pillH = std::floor(height * 0.6f);
    float const  pillW = std::floor(pillH * 1.8f);
    ImVec2 const pillMin{origin.x + width - pillW - 4.0f * scale, centreY - pillH * 0.5f};
    ImVec2 const pillMax{pillMin.x + pillW, pillMin.y + pillH};
    drawList->AddRectFilled(
        pillMin,
        pillMax,
        ImGui::GetColorU32(*value ? theme::kAccent : theme::kButtonActive),
        pillH * 0.5f
    );
    float const knob  = pillH * 0.5f - 2.0f * scale;
    float const knobX = *value ? pillMax.x - pillH * 0.5f : pillMin.x + pillH * 0.5f;
    drawList->AddCircleFilled({knobX, centreY}, knob, ImGui::GetColorU32(theme::kIconHighlight));

    ImVec4 const clip{x, origin.y, pillMin.x - 6.0f * scale, origin.y + height};
    drawList->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        {x, textYForCentre(centreY, label)},
        ImGui::GetColorU32(theme::kText),
        label,
        nullptr,
        0.0f,
        &clip
    );
    return clicked;
}

void fieldLabel(char const* icon, char const* text) {
    if (icon) ImGui::TextDisabled("%s  %s", icon, text);
    else ImGui::TextDisabled("%s", text);
    ImGui::SetNextItemWidth(-1.0f);
}

namespace {

// The value a held field works on: the caller's value is re-read from the project every frame and only changes once
// the edit is committed, so the drag accumulates here until release.
template <typename T, typename Draw>
bool onRelease(char const* id, T* value, Draw&& draw) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID const key     = ImGui::GetID(id);
    ImGuiID const heldKey = key ^ 0x9e3779b9u;
    bool const    held    = storage->GetBool(heldKey, false);
    T             working = *value;
    if (held) {
        if constexpr (std::is_same_v<T, float>) working = storage->GetFloat(key, *value);
        else working = storage->GetInt(key, *value);
    }
    draw(&working);
    bool const active = ImGui::IsItemActive();
    storage->SetBool(heldKey, active);
    if constexpr (std::is_same_v<T, float>) storage->SetFloat(key, working);
    else storage->SetInt(key, working);
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    *value = working;
    return true;
}

} // namespace

bool dragFloatOnRelease(char const* id, float* value, float low, float high, char const* format, float speed) {
    return onRelease(id, value, [&](float* working) { dragFloat(id, working, low, high, format, speed); });
}

bool dragIntOnRelease(char const* id, int* value, int low, int high, char const* format, float speed) {
    return onRelease(id, value, [&](int* working) { dragInt(id, working, low, high, format, speed); });
}

} // namespace playback::editor::ui::widgets
