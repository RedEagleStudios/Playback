#pragma once

#include "CameraKeyframe.h"
#include "FollowSettings.h"
#include "ShakeSettings.h"
#include "PropertyChannel.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>

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
    // The view's FOV, a static value here or keyed in `channels` (see CameraProperty). A keyframe track always shows
    // it; a person track only with overrideFov, and otherwise the followed player's own (spyglass zoom included).
    bool  overrideFov{};
    float fov{70.0f};
    // Keyframe tracks only: the shake, each value static here or keyed on its own in `channels`. The seed is never
    // keyed.
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
// The shake keys as one: a shake key holds all five values at its tick. Not a channel itself.
inline constexpr char const* Shake = "shake";
// The channels a Shake key sets, in the order the inspector lists them.
inline constexpr std::array<char const*, 5> ShakeMembers{ShakeStrength, ShakeFrequency, ShakePosition, ShakeRotation, ShakeRoll};
} // namespace CameraProperty

[[nodiscard]] inline bool isShakeProperty(std::string_view name) {
    return std::ranges::any_of(CameraProperty::ShakeMembers, [name](char const* member) { return name == member; });
}

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

// A camera track's properties besides its keys, edited and undone as one. Person tracks only use the FOV.
struct CameraProperties {
    bool          overrideFov{};
    float         fov{70.0f};
    float         shakeStrength{};
    float         shakeFrequency{ShakeSettings::DefaultFrequency};
    ShakeSettings shake;
    ValueChannels channels;

    bool operator==(CameraProperties const&) const = default;
};

[[nodiscard]] inline CameraProperties cameraProperties(CameraEntity const& camera) {
    return {camera.overrideFov, camera.fov, camera.shakeStrength, camera.shakeFrequency, camera.shake, camera.channels};
}

inline void setCameraProperties(CameraEntity& camera, CameraProperties properties) {
    camera.overrideFov    = properties.overrideFov;
    camera.fov            = std::isfinite(properties.fov) ? std::clamp(properties.fov, 1.0f, 179.0f) : camera.fov;
    camera.shakeStrength  = sanitizedShakeStrength(properties.shakeStrength);
    camera.shakeFrequency = sanitizedShakeFrequency(properties.shakeFrequency);
    camera.shake          = properties.shake.sanitized();
    pruneChannels(properties.channels);
    camera.channels = std::move(properties.channels);
}

// The FOV the track gives the view at `tick`; none where a person track leaves the followed player's own.
[[nodiscard]] inline std::optional<float> cameraFovAt(CameraEntity const& camera, double tick) {
    if (camera.kind != CameraTrackKind::Keyframe && !camera.overrideFov) return std::nullopt;
    return cameraPropertyAt(camera, CameraProperty::Fov, tick);
}

[[nodiscard]] inline bool hasCameraSource(CameraEntity const& camera) noexcept { return !camera.keysByTick.empty(); }

[[nodiscard]] inline bool isCameraRenderable(CameraEntity const& camera) noexcept {
    return camera.enabled && hasCameraSource(camera);
}

} // namespace playback::state::editing::model
