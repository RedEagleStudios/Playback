#pragma once

#include "CameraKeyframe.h"
#include "FollowSettings.h"
#include "ShakeSettings.h"
#include "PropertyChannel.h"

#include <map>
#include <optional>
#include <string>

namespace playback::state::editing::model {

struct CameraLimiter {
    Vec3 min{};
    Vec3 max{};
    bool enabled{};
};

// Keyframe tracks interpolate their own poses; person tracks view a replay player between their first and last key.
enum class CameraTrackKind : uint8_t { Keyframe, FirstPerson, ThirdPerson };

struct CameraEntity {
    std::string                   id;
    std::string                   name;
    CameraTrackKind               kind{CameraTrackKind::Keyframe};
    bool                          enabled{true};
    std::map<int, CameraKeyframe> keysByTick;
    std::optional<CameraLimiter>  limiter;
    std::string                   bindingEntityUuid;
    int                           bindingMode{};
    float                         bindingDamping{0.1f};
    bool                          locked{};
    // Third-person tracks only: how the camera frames the subject.
    FollowSettings follow;
    // Keyframe tracks only, besides the Transform keys: the view's FOV and the shake, each a static value here or keyed
    // on its own in `channels` (see CameraProperty). The seed is never keyed.
    float         fov{70.0f};
    float         shakeStrength{};
    float         shakeFrequency{ShakeSettings::DefaultFrequency};
    ShakeSettings shake;
    ValueChannels channels;
};

// The keyed property names of a keyframe track.
namespace CameraProperty {
inline constexpr char const* Fov            = "fov";
inline constexpr char const* ShakeStrength  = "shakeStrength";
inline constexpr char const* ShakeFrequency = "shakeFrequency";
inline constexpr char const* ShakePosition  = "shakePosition";
inline constexpr char const* ShakeRotation  = "shakeRotation";
inline constexpr char const* ShakeRoll      = "shakeRoll";
} // namespace CameraProperty

// A property's static value; unknown names give 0.
[[nodiscard]] inline float cameraStaticValue(CameraEntity const& camera, std::string const& name) {
    if (name == CameraProperty::Fov) return camera.fov;
    if (name == CameraProperty::ShakeStrength) return camera.shakeStrength;
    if (name == CameraProperty::ShakeFrequency) return camera.shakeFrequency;
    if (name == CameraProperty::ShakePosition) return camera.shake.positionAmount;
    if (name == CameraProperty::ShakeRotation) return camera.shake.rotationAmount;
    if (name == CameraProperty::ShakeRoll) return camera.shake.rollAmount;
    return 0.0f;
}

inline void setCameraStaticValue(CameraEntity& camera, std::string const& name, float value) {
    if (name == CameraProperty::Fov) camera.fov = value;
    else if (name == CameraProperty::ShakeStrength) camera.shakeStrength = value;
    else if (name == CameraProperty::ShakeFrequency) camera.shakeFrequency = value;
    else if (name == CameraProperty::ShakePosition) camera.shake.positionAmount = value;
    else if (name == CameraProperty::ShakeRotation) camera.shake.rotationAmount = value;
    else if (name == CameraProperty::ShakeRoll) camera.shake.rollAmount = value;
}

// The property's value at `tick`: keyed when it has keys, else static.
[[nodiscard]] inline float cameraPropertyAt(CameraEntity const& camera, std::string const& name, double tick) {
    return channelValue(camera.channels, name, tick, cameraStaticValue(camera, name));
}

// A keyframe track's properties besides its Transform keys, edited and undone as one.
struct CameraProperties {
    float         fov{70.0f};
    float         shakeStrength{};
    float         shakeFrequency{ShakeSettings::DefaultFrequency};
    ShakeSettings shake;
    ValueChannels channels;

    bool operator==(CameraProperties const&) const = default;
};

[[nodiscard]] inline CameraProperties cameraProperties(CameraEntity const& camera) {
    return {camera.fov, camera.shakeStrength, camera.shakeFrequency, camera.shake, camera.channels};
}

inline void setCameraProperties(CameraEntity& camera, CameraProperties properties) {
    camera.fov            = std::isfinite(properties.fov) ? std::clamp(properties.fov, 1.0f, 179.0f) : camera.fov;
    camera.shakeStrength  = sanitizedShakeStrength(properties.shakeStrength);
    camera.shakeFrequency = sanitizedShakeFrequency(properties.shakeFrequency);
    camera.shake          = properties.shake.sanitized();
    pruneChannels(properties.channels);
    camera.channels = std::move(properties.channels);
}

[[nodiscard]] inline bool hasCameraSource(CameraEntity const& camera) noexcept { return !camera.keysByTick.empty(); }

[[nodiscard]] inline bool isCameraRenderable(CameraEntity const& camera) noexcept {
    return camera.enabled && hasCameraSource(camera);
}

} // namespace playback::state::editing::model
