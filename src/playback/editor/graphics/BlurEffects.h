#pragma once

#include "playback/state/editing/models/OverlayItem.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace playback::editor::graphics {

// One blur as the GPU applies it to a finished frame. Positions are in the frame: the largest centred rectangle of
// the frame aspect inside the texture (u, v from 0 to 1, v down). Lengths are fractions of the frame's height, so
// preview and export of any size look the same.
struct BlurPass {
    state::editing::model::BlurStyle style{state::editing::model::BlurStyle::Gaussian};
    // Gaussian: radius (3 sigma). Mosaic: block size. Motion: streak length. All fractions of the frame height.
    // Radial: how far towards the centre each pixel smears, as a fraction of its distance to it (0 to 1).
    float amount{};
    // Motion: the streak's direction in degrees, 0 is horizontal, positive turns clockwise (v down).
    float angle{};
    // The area's centre (also the Radial blur's centre) in frame units, its width and height as fractions of the
    // frame's height (as shapes are sized), and its rotation in degrees, clockwise.
    float centreX{0.5f};
    float centreY{0.5f};
    float width{0.3f};
    float height{0.3f};
    float rotation{};
    // Edge softness of the area, a fraction of the frame height; 0 is a hard edge.
    float                           feather{};
    state::editing::model::BlurArea area{state::editing::model::BlurArea::Frame};
    // Blurs outside the area instead of inside it.
    bool invert{};
    // How much of the blurred picture replaces the original, 0 to 1 (opacity and fades).
    float mix{1.0f};
};

// The project's blur layers; any thread. The controller sets them whenever the project changes.
void setBlurLayers(std::vector<state::editing::model::OverlayItem> layers);

// The tick to evaluate at when the rendered view carries no sample time of its own; any thread.
void setBlurFallbackTick(double tick);

// The preview's frame aspect (width / height); any thread.
void setBlurPreviewAspect(float aspect);

// While an export runs, its output aspect; nullopt when it ends. Any thread.
void setBlurExportAspect(std::optional<float> aspect);

// The blurs to apply to the frame being presented now, in layer order: evaluated at the rendered view's sample time
// (the camera projection's), so keyed values and fades follow the frame exactly in preview and export. Empty when
// nothing blurs.
[[nodiscard]] std::vector<BlurPass> currentBlurPasses();

// The frame aspect the passes are laid out in: the export's while one runs, else the preview's.
[[nodiscard]] float blurFrameAspect();

} // namespace playback::editor::graphics
