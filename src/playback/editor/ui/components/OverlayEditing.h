#pragma once

#include "playback/editor/ui/PanelContext.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include <optional>
#include <string>
#include <vector>

// Shared by the timeline and the inspector: making, finding and committing title, shape, model, blur and light layers.
namespace playback::editor::ui::overlays {

using state::editing::model::BlurStyle;
using state::editing::model::LightStyle;
using state::editing::model::OverlayItem;
using state::editing::model::OverlayKind;
using state::editing::model::ShapeKind;

// A new layer of `kind` from `tick` for five seconds, named "<kind> <n>". A model stands a few blocks in front of the
// camera, a shape of `shape` (with `points` corners for a polygon) sits in the frame's centre, a blur of `blur` covers
// the whole frame, a light of `light` shines in from the upper left.
[[nodiscard]] OverlayItem makeLayer(
    state::editing::model::EditorStateExt const& project,
    OverlayKind                                  kind,
    int                                          tick,
    ShapeKind                                    shape  = ShapeKind::Rectangle,
    int                                          points = 3,
    BlurStyle                                    blur   = BlurStyle::Gaussian,
    LightStyle                                   light  = LightStyle::Point
);

[[nodiscard]] OverlayItem const* find(state::editing::model::EditorStateExt const& project, std::string const& id);

// One undo step replacing the project's layers.
void commit(PanelContext const& ctx, std::vector<OverlayItem> layers);
// The project's layers with `item` replaced by id (appended when it is new): as one undo step, or shown live while a
// control is dragged.
void commitItem(PanelContext const& ctx, OverlayItem const& item);
void previewItem(PanelContext const& ctx, OverlayItem const& item);
// Ends a live preview without committing it.
void endPreview(PanelContext const& ctx);

// The layer's kind as a timeline glyph.
[[nodiscard]] char const* icon(OverlayItem const& item);

// A blur style's name and glyph, for menus and the inspector.
[[nodiscard]] std::string blurStyleName(BlurStyle style);
[[nodiscard]] char const* blurStyleIcon(BlurStyle style);
// A light style's name and glyph.
[[nodiscard]] std::string lightStyleName(LightStyle style);
[[nodiscard]] char const* lightStyleIcon(LightStyle style);

// Asks for a .mimodel file; its UTF-8 path, or none when cancelled.
[[nodiscard]] std::optional<std::string> pickModelFile();

// Where the camera looks, `distance` blocks ahead and down to about foot height, for a model placed from the view.
[[nodiscard]] std::optional<state::editing::model::Vec3> pointInFrontOfCamera(float distance);

} // namespace playback::editor::ui::overlays
