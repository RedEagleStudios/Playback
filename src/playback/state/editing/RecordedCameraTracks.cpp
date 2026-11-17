#include "RecordedCameraTracks.h"

#include <cmath>
#include <cstddef>

namespace playback::state::editing {

namespace {

using keyframe::CameraRenderState;
using model::CameraInterpolationType;

// How far Linear interpolation between two kept keys may stray from a recorded view.
constexpr float PositionTolerance = 0.05f; // blocks
constexpr float AngleTolerance    = 0.5f;  // degrees, rotation and FOV
// A change this large within one tick is a cut, not a move.
constexpr float CutDistance = 4.0f;  // blocks
constexpr float CutAngle    = 45.0f; // degrees

float angleDelta(float from, float to) { return std::remainder(to - from, 360.0f); }

float distance(CameraRenderState const& a, CameraRenderState const& b) {
    return std::hypot(b.x - a.x, b.y - a.y, b.z - a.z);
}

// The pose Linear keys at a and b give at `amount` (0 to 1), turning the short way as the keyframe track does.
CameraRenderState lerp(CameraRenderState const& a, CameraRenderState const& b, float amount) {
    return {
        a.x + (b.x - a.x) * amount,
        a.y + (b.y - a.y) * amount,
        a.z + (b.z - a.z) * amount,
        a.yaw + angleDelta(a.yaw, b.yaw) * amount,
        a.pitch + angleDelta(a.pitch, b.pitch) * amount,
        a.roll + angleDelta(a.roll, b.roll) * amount,
        a.fov + (b.fov - a.fov) * amount,
    };
}

bool near(CameraRenderState const& a, CameraRenderState const& b) {
    return distance(a, b) <= PositionTolerance && std::abs(angleDelta(a.yaw, b.yaw)) <= AngleTolerance
        && std::abs(angleDelta(a.pitch, b.pitch)) <= AngleTolerance
        && std::abs(angleDelta(a.roll, b.roll)) <= AngleTolerance && std::abs(b.fov - a.fov) <= AngleTolerance;
}

bool isCut(CameraRenderState const& a, CameraRenderState const& b) {
    return distance(a, b) > CutDistance || std::abs(angleDelta(a.yaw, b.yaw)) > CutAngle
        || std::abs(angleDelta(a.pitch, b.pitch)) > CutAngle || std::abs(angleDelta(a.roll, b.roll)) > CutAngle;
}

model::CameraKeyframe keyOf(CameraRenderState const& view) {
    model::CameraKeyframe key;
    key.position          = {view.x, view.y, view.z};
    key.yaw               = view.yaw;
    key.pitch             = view.pitch;
    key.roll              = view.roll;
    key.fov               = view.fov;
    key.interpolationType = CameraInterpolationType::Linear;
    return key;
}

// Keys for the views [first, last] of one cut-free stretch: a key ends a stretch of Linear motion as soon as the next
// view would leave the straight line from the previous key.
void addSimplifiedKeys(
    std::span<RecordedCameraView const>   views,
    std::size_t                           first,
    std::size_t                           last,
    std::map<int, model::CameraKeyframe>& keys
) {
    std::size_t anchor       = first;
    keys[views[anchor].tick] = keyOf(views[anchor].view);
    for (std::size_t end = first + 2; end <= last; ++end) {
        auto const& from = views[anchor];
        auto const& to   = views[end];
        auto const  span = static_cast<float>(to.tick - from.tick);
        for (std::size_t inner = anchor + 1; inner < end; ++inner) {
            auto const expected = lerp(from.view, to.view, static_cast<float>(views[inner].tick - from.tick) / span);
            if (near(expected, views[inner].view)) continue;
            anchor                   = end - 1;
            keys[views[anchor].tick] = keyOf(views[anchor].view);
            break;
        }
    }
    if (last != anchor) keys[views[last].tick] = keyOf(views[last].view);
}

} // namespace

std::vector<std::map<int, model::CameraKeyframe>> recordedCameraTracks(std::span<RecordedCameraView const> views) {
    std::vector<std::map<int, model::CameraKeyframe>> tracks;
    std::size_t                                       runStart = 0;
    for (std::size_t next = 1; next <= views.size(); ++next) {
        if (next < views.size() && views[next].tick == views[next - 1].tick + 1) continue;

        // [runStart, next) is one unbroken run of ticks: one track, split into stretches at its cuts.
        auto&       keys         = tracks.emplace_back();
        std::size_t stretchStart = runStart;
        for (std::size_t index = runStart; index < next; ++index) {
            bool const lastInRun = index + 1 == next;
            bool const cutAfter  = !lastInRun && isCut(views[index].view, views[index + 1].view);
            if (!lastInRun && !cutAfter) continue;
            addSimplifiedKeys(views, stretchStart, index, keys);
            if (cutAfter) keys[views[index].tick].interpolationType = CameraInterpolationType::Hold;
            stretchStart = index + 1;
        }
        runStart = next;
    }
    return tracks;
}

} // namespace playback::state::editing
