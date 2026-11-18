#pragma once

#include "playback/keyframe/CameraRenderState.h"
#include "playback/visuals/ReplaySampleTime.h"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <optional>

namespace bgfx {
struct Frame;
}

namespace playback::editor::graphics {

struct RenderCameraProjection {
    ::glm::dmat4 viewProjection{1.0};
    float        nearClipW{0.001f};
    // Sample time this view was rendered at, so overlays sample the timeline at the same instant.
    std::optional<visuals::ReplaySampleTime> sampleTime;
};

[[nodiscard]] bool                                       hookCameraRender(bool enable);
[[nodiscard]] bool                                       isCameraRenderInstalled();
[[nodiscard]] std::optional<keyframe::CameraRenderState> currentRendererCameraState();
[[nodiscard]] std::optional<RenderCameraProjection>      currentRenderCameraProjection();
void                                                     useSubmittedCameraProjection(bgfx::Frame const* frame);

// The project's FOV in degrees for every view the replay renders, preview and export; nullopt lets each view keep its
// own. The first-person hand keeps the game's. Any thread.
void setFovOverride(std::optional<float> degrees);

} // namespace playback::editor::graphics
