#pragma once

#include "imgui.h"

#include <string>

namespace playback::editor::ui::widgets {

constexpr int kTicksPerSecond = 20;

// mm:ss.cc, shared by every panel that shows a timeline position.
[[nodiscard]] std::string formatTick(int tick);

// Ruler labels: always m:ss so the unit is unambiguous ("0:05", "1:05").
[[nodiscard]] std::string formatTickCompact(int tick);

[[nodiscard]] float iconButtonSize();

bool iconButton(char const* id, char const* icon, char const* tooltip, bool enabled = true);

// Icon button that stays lit while `active`, for view toggles in toolbars.
bool iconToggle(char const* id, char const* icon, char const* tooltip, bool active, bool enabled = true);

// An icon toggle of an explicit square `size`, its glyph scaled to match, for bands shorter than a toolbar.
bool compactIconToggle(char const* id, char const* icon, char const* tooltip, bool active, float size);

// Icon glyphs are drawn larger than body text so they read as buttons rather than characters.
[[nodiscard]] float iconGlyphSize();

// Centres an icon glyph on its ink box inside a square of `boxSize`; `glyphSize` 0 is the standard icon size.
void drawIconCentred(
    ImDrawList*   drawList,
    char const*   icon,
    ImVec2 const& origin,
    float         boxSize,
    ImU32         color,
    float         glyphSize = 0.0f
);

// Y offset that puts the cap height of body text on a box's centre line.
[[nodiscard]] float textOffsetInBox(float boxHeight);

// Same, but centres the run's real glyph ink: CJK ideographs sit well below the Latin cap height.
[[nodiscard]] float textOffsetInBox(float boxHeight, char const* text);

// Draw Y putting the run's ink centre on `centreY`; prefer it when the box origin is fractional.
[[nodiscard]] float textYForCentre(float centreY, char const* text);

// Optical centre of a run drawn at `drawY`; align icons to this rather than to the row.
[[nodiscard]] float textInkCentre(float drawY, char const* text);

// Icon whose ink centre lands on `centre`, for rows whose height is not a whole number of pixels.
void drawIconAtCentre(ImDrawList* drawList, char const* icon, ImVec2 const& centre, ImU32 color, float glyphSize = 0.0f);

// Transport and window glyphs drawn from primitives so they share one weight and exact centring. The shading ones are
// the viewport's view modes, as in a 3D package: a flat disc, a half-lit sphere and a shaded sphere.
enum class VectorIcon {
    SkipStart,
    StepBack,
    Play,
    Pause,
    StepForward,
    SkipEnd,
    Maximize,
    Restore,
    ShadingFlat,
    ShadingLit,
    ShadingRendered,
};

void drawVectorIcon(ImDrawList* drawList, ImVec2 const& centre, float boxSize, ImU32 color, VectorIcon icon);

bool vectorIconButton(char const* id, VectorIcon icon, char const* tooltip, bool active = false, bool enabled = true);

// Tooltip for the last item, drawn on the foreground list so panel windows never cover it.
void itemTooltip(char const* text);

// Same, for hit regions tested manually rather than through an ImGui item.
void hoverTooltip(char const* text);

// Flat combo-looking chip, exactly one icon button tall; returns true when clicked so callers can open a popup.
bool dropdownChip(
    char const* id,
    char const* label,
    char const* tooltip = nullptr,
    bool        enabled = true,
    char const* icon    = nullptr
);

// Width the chip will occupy, for callers that centre a group of controls.
[[nodiscard]] float dropdownChipWidth(char const* label, char const* icon = nullptr);

// Opens `popupId` pinned under the last item instead of at the mouse; call right after the trigger item.
void openAnchoredPopup(char const* popupId);
// Same, pinned under an explicit screen rectangle (a drawn timeline segment rather than an ImGui item).
void openAnchoredPopup(char const* popupId, ImVec2 anchorMin, ImVec2 anchorMax);

// Width of the control the popup was anchored to, or 0 when this window did not open it.
[[nodiscard]] float anchoredPopupTriggerWidth(char const* popupId);

// Begins a popup opened with openAnchoredPopup, padded like a menu; call endAnchoredPopup only when it returns true.
// `minWidth` widens it past the trigger (a list of names under a narrow chip); it never goes off screen.
bool beginAnchoredPopup(char const* popupId, float minWidth = 0.0f);
void endAnchoredPopup();

// ImGui::BeginMenu / BeginCombo with the same menu padding; pair with endMenu / endCombo only when they return true.
bool beginMenu(char const* label, bool enabled = true);
void endMenu();
bool beginCombo(char const* id, char const* preview);
void endCombo();

// Draws the shared "no active project" placeholder.
void noActiveProjectPlaceholder();

// Number fields dragged left and right with the mouse (Shift faster, Alt finer); a double-click or Ctrl+click types a
// value. With low < high the value is clamped, crossing the field's width sweeps the whole range and a fill shows
// where the value sits; otherwise `speed` is the change per pixel. `speed` 0 picks it from the range.
bool dragFloat(char const* id, float* value, float low, float high, char const* format, float speed = 0.0f);
bool dragInt(char const* id, int* value, int low, int high, char const* format = "%d", float speed = 0.0f);
// The same, for values committed once the drag or typing ends: returns true then, with the final value in `value`.
bool dragFloatOnRelease(char const* id, float* value, float low, float high, char const* format, float speed = 0.0f);
bool dragIntOnRelease(char const* id, int* value, int low, int high, char const* format = "%d", float speed = 0.0f);
// `count` such fields side by side (a position, a size), sharing the next item width.
bool dragFloatN(
    char const* id,
    float*      values,
    int         count,
    float       speed,
    char const* format,
    float       low  = 0.0f,
    float       high = 0.0f
);

// A flat full-width row: an icon, a label and an on/off switch; the whole row toggles. Returns true when clicked.
bool toggleRow(char const* id, char const* icon, char const* label, bool* value, char const* tooltip = nullptr);

// A dim caption, with an optional icon, over the control that follows; that control then spans the row.
void fieldLabel(char const* icon, char const* text);

} // namespace playback::editor::ui::widgets
