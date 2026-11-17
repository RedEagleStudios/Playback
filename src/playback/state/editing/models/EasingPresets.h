#pragma once

#include "MathTypes.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace playback::state::editing::model {

enum class EasingFamily : uint8_t { Sine, Quad, Cubic, Quart, Quint, Expo, Circ, Back };
enum class EasingDirection : uint8_t { In, Out, InOut };

// A cubic Bézier keyframe curve under a familiar name. Applying one sets the key to CubicBezier with these control
// points, so projects store plain control points and need no format change.
struct EasingPreset {
    EasingFamily    family;
    EasingDirection direction;
    Vec2            control1;
    Vec2            control2;
};

// The usual Bézier fits of the Penner easings (easings.net). Back's control points leave 0..1, so it overshoots.
inline constexpr std::array<EasingPreset, 24> EasingPresets{
    {
     {EasingFamily::Sine, EasingDirection::In, {0.12f, 0.0f}, {0.39f, 0.0f}},
     {EasingFamily::Sine, EasingDirection::Out, {0.61f, 1.0f}, {0.88f, 1.0f}},
     {EasingFamily::Sine, EasingDirection::InOut, {0.37f, 0.0f}, {0.63f, 1.0f}},
     {EasingFamily::Quad, EasingDirection::In, {0.11f, 0.0f}, {0.5f, 0.0f}},
     {EasingFamily::Quad, EasingDirection::Out, {0.5f, 1.0f}, {0.89f, 1.0f}},
     {EasingFamily::Quad, EasingDirection::InOut, {0.45f, 0.0f}, {0.55f, 1.0f}},
     {EasingFamily::Cubic, EasingDirection::In, {0.32f, 0.0f}, {0.67f, 0.0f}},
     {EasingFamily::Cubic, EasingDirection::Out, {0.33f, 1.0f}, {0.68f, 1.0f}},
     {EasingFamily::Cubic, EasingDirection::InOut, {0.65f, 0.0f}, {0.35f, 1.0f}},
     {EasingFamily::Quart, EasingDirection::In, {0.5f, 0.0f}, {0.75f, 0.0f}},
     {EasingFamily::Quart, EasingDirection::Out, {0.25f, 1.0f}, {0.5f, 1.0f}},
     {EasingFamily::Quart, EasingDirection::InOut, {0.76f, 0.0f}, {0.24f, 1.0f}},
     {EasingFamily::Quint, EasingDirection::In, {0.64f, 0.0f}, {0.78f, 0.0f}},
     {EasingFamily::Quint, EasingDirection::Out, {0.22f, 1.0f}, {0.36f, 1.0f}},
     {EasingFamily::Quint, EasingDirection::InOut, {0.83f, 0.0f}, {0.17f, 1.0f}},
     {EasingFamily::Expo, EasingDirection::In, {0.7f, 0.0f}, {0.84f, 0.0f}},
     {EasingFamily::Expo, EasingDirection::Out, {0.16f, 1.0f}, {0.3f, 1.0f}},
     {EasingFamily::Expo, EasingDirection::InOut, {0.87f, 0.0f}, {0.13f, 1.0f}},
     {EasingFamily::Circ, EasingDirection::In, {0.55f, 0.0f}, {1.0f, 0.45f}},
     {EasingFamily::Circ, EasingDirection::Out, {0.0f, 0.55f}, {0.45f, 1.0f}},
     {EasingFamily::Circ, EasingDirection::InOut, {0.85f, 0.0f}, {0.15f, 1.0f}},
     {EasingFamily::Back, EasingDirection::In, {0.36f, 0.0f}, {0.66f, -0.56f}},
     {EasingFamily::Back, EasingDirection::Out, {0.34f, 1.56f}, {0.64f, 1.0f}},
     {EasingFamily::Back, EasingDirection::InOut, {0.68f, -0.6f}, {0.32f, 1.6f}},
     }
};

// Index of the preset with these control points, if one matches.
inline std::optional<std::size_t> findEasingPreset(Vec2 const& control1, Vec2 const& control2) {
    auto const near = [](Vec2 const& a, Vec2 const& b) {
        return std::abs(a.x - b.x) < 1e-4f && std::abs(a.y - b.y) < 1e-4f;
    };
    for (std::size_t index = 0; index < EasingPresets.size(); ++index) {
        auto const& preset = EasingPresets[index];
        if (near(preset.control1, control1) && near(preset.control2, control2)) return index;
    }
    return std::nullopt;
}

} // namespace playback::state::editing::model
