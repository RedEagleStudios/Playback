#include "EasingCurveEditor.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/keyframe/InterpolationHelpers.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playback::editor::ui {

namespace {

using state::editing::model::Vec2;

// The Back presets reach about -0.56 and 1.56; the graph shows that much room around the unit square.
constexpr float MinProgress = -0.6f;
constexpr float MaxProgress = 1.6f;
// The preview dot crosses the curve in LoopSeconds, then rests at the end for RestSeconds.
constexpr float LoopSeconds = 1.6f;
constexpr float RestSeconds = 0.5f;
// After a commit the key shows the old controls until the editor publishes the edit; the draft covers that gap.
constexpr int PendingFrames = 15;

bool same(Vec2 const& left, Vec2 const& right) { return left.x == right.x && left.y == right.y; }

struct Graph {
    ImVec2 min;
    ImVec2 max;

    [[nodiscard]] ImVec2 toScreen(Vec2 const& point) const {
        float const progress = (point.y - MinProgress) / (MaxProgress - MinProgress);
        return {min.x + point.x * (max.x - min.x), max.y - progress * (max.y - min.y)};
    }

    [[nodiscard]] Vec2 fromScreen(ImVec2 const& point) const {
        float const time     = (point.x - min.x) / (max.x - min.x);
        float const progress = (max.y - point.y) / (max.y - min.y);
        return {time, MinProgress + progress * (MaxProgress - MinProgress)};
    }
};

Vec2 bezierPoint(Vec2 const& control1, Vec2 const& control2, float amount) {
    using keyframe::detail::cubicBezier;
    return {
        cubicBezier(0.0f, control1.x, control2.x, 1.0f, amount),
        cubicBezier(0.0f, control1.y, control2.y, 1.0f, amount)
    };
}

float distanceSquared(ImVec2 const& left, ImVec2 const& right) {
    float const x = left.x - right.x;
    float const y = left.y - right.y;
    return x * x + y * y;
}

} // namespace

std::optional<EasingCurveEdit> drawEasingCurveEditor(char const* id, Vec2 control1, Vec2 control2) {
    ImGui::PushID(id);
    auto&         storage    = *ImGui::GetStateStorage();
    ImGuiID const handleKey  = ImGui::GetID("handle");
    ImGuiID const pendingKey = ImGui::GetID("pending");
    ImGuiID const draftKeys[4]{ImGui::GetID("x1"), ImGui::GetID("y1"), ImGui::GetID("x2"), ImGui::GetID("y2")};
    auto const    loadDraft = [&] {
        return std::array{
            Vec2{storage.GetFloat(draftKeys[0]), storage.GetFloat(draftKeys[1])},
            Vec2{storage.GetFloat(draftKeys[2]), storage.GetFloat(draftKeys[3])},
        };
    };
    auto const storeDraft = [&](std::array<Vec2, 2> const& draft) {
        storage.SetFloat(draftKeys[0], draft[0].x);
        storage.SetFloat(draftKeys[1], draft[0].y);
        storage.SetFloat(draftKeys[2], draft[1].x);
        storage.SetFloat(draftKeys[3], draft[1].y);
    };

    int                 handle   = storage.GetInt(handleKey, 0);
    std::array<Vec2, 2> controls = {control1, control2};
    if (handle != 0) {
        controls = loadDraft();
    } else if (int const pendingUntil = storage.GetInt(pendingKey, 0); pendingUntil > ImGui::GetFrameCount()) {
        auto const draft = loadDraft();
        if (same(draft[0], control1) && same(draft[1], control2)) storage.SetInt(pendingKey, 0);
        else controls = draft;
    }

    float const  font   = ImGui::GetFontSize();
    float const  width  = std::max(ImGui::GetContentRegionAvail().x, font * 8.0f);
    float const  height = std::clamp(width * 0.85f, font * 7.0f, font * 15.0f);
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##graph", {width, height});
    bool const  hovered = ImGui::IsItemHovered();
    float const pad     = font * 0.6f;
    Graph const graph{
        {origin.x + pad,         origin.y + pad         },
        {origin.x + width - pad, origin.y + height - pad}
    };

    ImVec2 const mouse       = ImGui::GetIO().MousePos;
    float const  grabRadius  = font * 0.8f;
    auto const   handleUnder = [&] {
        int   nearest = 0;
        float best    = grabRadius * grabRadius;
        for (int index = 0; index < 2; ++index) {
            float const distance = distanceSquared(mouse, graph.toScreen(controls[index]));
            if (distance <= best) {
                best    = distance;
                nearest = index + 1;
            }
        }
        return nearest;
    };

    std::optional<EasingCurveEdit> result;
    if (ImGui::IsItemActivated()) {
        handle = handleUnder();
        storage.SetInt(handleKey, handle);
        if (handle != 0) storeDraft(controls);
    }
    if (handle != 0 && ImGui::IsItemActive()) {
        auto point = graph.fromScreen(mouse);
        point.x    = std::clamp(point.x, 0.0f, 1.0f);
        point.y    = std::clamp(point.y, MinProgress, MaxProgress);
        if (!same(point, controls[handle - 1])) {
            controls[handle - 1] = point;
            storeDraft(controls);
            result = EasingCurveEdit{controls, false};
        }
    }
    if (handle != 0 && ImGui::IsItemDeactivated()) {
        storage.SetInt(handleKey, 0);
        // A click without a move changes nothing and must not add an undo step.
        if (!same(controls[0], control1) || !same(controls[1], control2)) {
            storage.SetInt(pendingKey, ImGui::GetFrameCount() + PendingFrames);
            result = EasingCurveEdit{controls, true};
        }
        handle = 0;
    }
    int const hoveredHandle = handle != 0 ? handle : (hovered ? handleUnder() : 0);
    if (handle != 0) ImGui::SetMouseCursor(theme::kCursorGrabbing);
    else if (hoveredHandle != 0) ImGui::SetMouseCursor(theme::kCursorGrab);

    auto&        draw     = *ImGui::GetWindowDrawList();
    ImVec2 const unitMin  = graph.toScreen({0.0f, 1.0f});
    ImVec2 const unitMax  = graph.toScreen({1.0f, 0.0f});
    float const  rounding = font * 0.25f;
    draw.AddRectFilled(origin, {origin.x + width, origin.y + height}, theme::kInputBg, rounding);
    draw.AddRectFilled(unitMin, unitMax, theme::kInputBgHover);
    for (float const step : {0.25f, 0.5f, 0.75f}) {
        draw.AddLine(graph.toScreen({step, 0.0f}), graph.toScreen({step, 1.0f}), theme::kBorder);
        draw.AddLine(graph.toScreen({0.0f, step}), graph.toScreen({1.0f, step}), theme::kBorder);
    }
    draw.AddRect(unitMin, unitMax, theme::kBorder);
    // Linear reference: no easing.
    draw.AddLine(graph.toScreen({0.0f, 0.0f}), graph.toScreen({1.0f, 1.0f}), theme::withAlpha(theme::kTextDim, 0x60));

    draw.AddLine(graph.toScreen({0.0f, 0.0f}), graph.toScreen(controls[0]), theme::kTextDim, 1.0f);
    draw.AddLine(graph.toScreen({1.0f, 1.0f}), graph.toScreen(controls[1]), theme::kTextDim, 1.0f);

    constexpr int Segments = 64;
    ImVec2        points[Segments + 1];
    for (int index = 0; index <= Segments; ++index) {
        points[index] = graph.toScreen(bezierPoint(controls[0], controls[1], static_cast<float>(index) / Segments));
    }
    draw.AddPolyline(points, Segments + 1, theme::kAccent, ImDrawFlags_None, 2.0f);

    // The motion: where a camera eased this way is, over the time between the keys.
    float const phase    = std::fmod(static_cast<float>(ImGui::GetTime()), LoopSeconds + RestSeconds);
    float const time     = std::min(phase / LoopSeconds, 1.0f);
    float const progress = keyframe::detail::cubicBezierEase(time, controls[0], controls[1]);
    draw.AddCircleFilled(
        graph.toScreen({time, std::clamp(progress, MinProgress, MaxProgress)}),
        font * 0.22f,
        theme::kSelected
    );

    for (int index = 0; index < 2; ++index) {
        ImVec2 const center = graph.toScreen(controls[index]);
        bool const   lit    = hoveredHandle == index + 1;
        draw.AddCircleFilled(center, font * (lit ? 0.38f : 0.3f), lit ? theme::kSelected : theme::kText);
        draw.AddCircle(center, font * (lit ? 0.38f : 0.3f), theme::kBgPanel, 0, 1.5f);
    }

    char readout[64];
    std::snprintf(
        readout,
        sizeof(readout),
        "%.2f, %.2f, %.2f, %.2f",
        controls[0].x,
        controls[0].y,
        controls[1].x,
        controls[1].y
    );
    ImGui::TextDisabled("%s", readout);
    ImGui::PopID();
    return result;
}

} // namespace playback::editor::ui
