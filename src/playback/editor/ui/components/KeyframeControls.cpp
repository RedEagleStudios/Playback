#include "KeyframeControls.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

namespace playback::editor::ui::keyframes {

using namespace ll::i18n_literals;
using state::editing::model::KeyEasing;
using state::editing::model::ValueChannel;
using state::editing::model::ValueChannels;
using state::editing::model::ValueKey;

namespace {

// A small square button drawn from primitives, so the diamond and arrows share one weight at every scale.
bool smallButton(char const* id, float size, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    bool const clicked = ImGui::InvisibleButton(id, {size, size});
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImGui::GetItemRectMin(),
            ImGui::GetItemRectMax(),
            theme::withAlpha(theme::kButtonHover, 0xc0),
            theme::kFrameRounding
        );
    }
    return clicked && enabled;
}

void drawDiamond(ImVec2 centre, float radius, bool filled, ImU32 color) {
    auto*        drawList = ImGui::GetWindowDrawList();
    ImVec2 const points[]{
        {centre.x, centre.y - radius},
        {centre.x + radius, centre.y},
        {centre.x, centre.y + radius},
        {centre.x - radius, centre.y},
    };
    if (filled) drawList->AddConvexPolyFilled(points, 4, color);
    else drawList->AddPolyline(points, 4, color, ImDrawFlags_Closed, std::max(1.0f, metrics::scale() * 1.2f));
}

ValueChannel const* channelOf(ValueChannels const& channels, std::string const& name) {
    auto const channel = channels.find(name);
    return channel == channels.end() || channel->second.empty() ? nullptr : &channel->second;
}

// The label line: name on the left, then ◀ ◆ ▶ on the right. Returns what the buttons did.
struct HeaderClicks {
    bool               toggle{};
    std::optional<int> seek;
    bool               easingMenu{};
};

HeaderClicks header(
    char const*         id,
    char const*         icon,
    std::string const&  label,
    ValueChannel const* channel,
    bool                keyedHere,
    int                 tick
) {
    HeaderClicks clicks;
    ImGui::PushID(id);
    if (icon) ImGui::TextDisabled("%s  %s", icon, label.c_str());
    else ImGui::TextDisabled("%s", label.c_str());

    float const size  = std::floor(ImGui::GetTextLineHeight() + 2.0f * metrics::scale());
    float const right = ImGui::GetContentRegionMax().x;
    ImGui::SameLine(std::max(0.0f, right - size * 3.0f - 4.0f * metrics::scale()), 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {2.0f * metrics::scale(), 0.0f});

    std::optional<int> previous;
    std::optional<int> next;
    if (channel) {
        auto const after = channel->upper_bound(tick);
        if (after != channel->end()) next = after->first;
        auto const before = channel->lower_bound(tick);
        if (before != channel->begin()) previous = std::prev(before)->first;
    }
    float const glyph = std::floor(size * 0.8f);
    if (smallButton("##previous", size, previous.has_value())) clicks.seek = previous;
    widgets::drawIconAtCentre(
        ImGui::GetWindowDrawList(),
        ICON_CHEVRON_LEFT,
        {(ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f},
        previous ? theme::kIconInactive : theme::withAlpha(theme::kTextDim, 0x60),
        glyph
    );
    widgets::itemTooltip("playback.refactorEditor.keyframes.previous"_tr().c_str());
    ImGui::SameLine();
    if (smallButton("##key", size, true)) clicks.toggle = true;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && keyedHere) clicks.easingMenu = true;
    ImVec2 const centre{
        (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
        (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f,
    };
    drawDiamond(
        centre,
        size * 0.32f,
        keyedHere,
        keyedHere ? theme::kKeyframeSelected : channel ? theme::kKeyframe : theme::kIconInactive
    );
    widgets::itemTooltip(
        (keyedHere ? "playback.refactorEditor.keyframes.removeKey"_tr() : "playback.refactorEditor.keyframes.addKey"_tr()).c_str()
    );
    ImGui::SameLine();
    if (smallButton("##next", size, next.has_value())) clicks.seek = next;
    widgets::drawIconAtCentre(
        ImGui::GetWindowDrawList(),
        ICON_CHEVRON_RIGHT,
        {(ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f},
        next ? theme::kIconInactive : theme::withAlpha(theme::kTextDim, 0x60),
        glyph
    );
    widgets::itemTooltip("playback.refactorEditor.keyframes.next"_tr().c_str());
    ImGui::PopStyleVar();
    ImGui::PopID();
    return clicks;
}

// Right-click on a key's diamond: how the value travels from this key to the next.
bool easingPopup(char const* id, ValueChannels& channels, std::vector<std::string> const& names, int tick, bool open) {
    std::string const popup = std::string("##easing-") + id;
    if (open) ImGui::OpenPopup(popup.c_str());
    if (!ImGui::BeginPopup(popup.c_str())) return false;
    bool       changed = false;
    auto const current = [&] {
        auto const channel = channels.find(names.front());
        if (channel == channels.end()) return KeyEasing::EaseInOut;
        auto const key = channel->second.find(tick);
        return key == channel->second.end() ? KeyEasing::EaseInOut : key->second.easing;
    }();
    for (int index = 0; index <= static_cast<int>(KeyEasing::Hold); ++index) {
        auto const easing = static_cast<KeyEasing>(index);
        if (!ImGui::MenuItem(easingName(easing).c_str(), nullptr, easing == current)) continue;
        for (auto const& name : names) {
            if (auto channel = channels.find(name); channel != channels.end()) {
                if (auto key = channel->second.find(tick); key != channel->second.end()) key->second.easing = easing;
            }
        }
        changed = true;
    }
    ImGui::EndPopup();
    return changed;
}

} // namespace

std::string easingName(KeyEasing easing) {
    switch (easing) {
    case KeyEasing::Linear:
        return "playback.refactorEditor.keyframes.easingLinear"_tr();
    case KeyEasing::EaseInOut:
        return "playback.refactorEditor.keyframes.easingEaseInOut"_tr();
    case KeyEasing::EaseIn:
        return "playback.refactorEditor.keyframes.easingEaseIn"_tr();
    case KeyEasing::EaseOut:
        return "playback.refactorEditor.keyframes.easingEaseOut"_tr();
    case KeyEasing::Hold:
        return "playback.refactorEditor.keyframes.easingHold"_tr();
    }
    return {};
}

bool diamondButton(char const* id, bool keyedHere, bool keyed, char const* tooltip) {
    float const size    = std::floor(ImGui::GetTextLineHeight() + 2.0f * metrics::scale());
    bool const  clicked = smallButton(id, size, true);
    ImVec2 const centre{
        (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
        (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f,
    };
    drawDiamond(
        centre,
        size * 0.32f,
        keyedHere,
        keyedHere ? theme::kKeyframeSelected : keyed ? theme::kKeyframe : theme::kIconInactive
    );
    if (tooltip) widgets::itemTooltip(tooltip);
    return clicked;
}

RowResult propertyRow(
    char const*         id,
    char const*         icon,
    std::string const&  label,
    ValueChannels&      channels,
    std::string const&  name,
    float&              staticValue,
    int                 tick,
    float               low,
    float               high,
    char const*         format,
    float               displayScale
) {
    RowResult         result;
    ValueChannel const* channel   = channelOf(channels, name);
    bool const          keyedHere = channel && channel->contains(tick);
    float const current = channel ? state::editing::model::sampleChannel(*channel, tick).value_or(staticValue) : staticValue;

    auto const clicks = header(id, icon, label, channel, keyedHere, tick);
    if (clicks.toggle) {
        if (keyedHere) {
            channels[name].erase(tick);
            state::editing::model::pruneChannels(channels);
        } else {
            channels[name].insert_or_assign(tick, ValueKey{current});
        }
        result.changed = true;
    }
    result.seek = clicks.seek;
    if (easingPopup(id, channels, {name}, tick, clicks.easingMenu)) result.changed = true;

    float shown = current * displayScale;
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushID(id);
    bool const edited = widgets::dragFloat("##value", &shown, low, high, format);
    ImGui::PopID();
    if (edited) {
        float const value = shown / displayScale;
        // Every edit is a key at the playhead. The value without keys follows the latest edit, so removing every key
        // leaves the property as it was last set.
        staticValue                = value;
        channels[name][tick].value = value;
        result.changed             = true;
    }
    return result;
}

RowResult colorRow(
    char const*        id,
    char const*        icon,
    std::string const& label,
    ValueChannels&     channels,
    std::string const& name,
    std::array<float, 3>& staticValue,
    int                tick
) {
    RowResult         result;
    std::string const names[3]{name + "R", name + "G", name + "B"};
    ValueChannel const* first     = channelOf(channels, names[0]);
    bool const          keyedHere = first && first->contains(tick);
    float               current[3];
    for (int index = 0; index < 3; ++index) {
        auto const* channel = channelOf(channels, names[index]);
        current[index] =
            channel ? state::editing::model::sampleChannel(*channel, tick).value_or(staticValue[index]) : staticValue[index];
    }

    auto const clicks = header(id, icon, label, first, keyedHere, tick);
    if (clicks.toggle) {
        for (int index = 0; index < 3; ++index) {
            if (keyedHere) channels[names[index]].erase(tick);
            else channels[names[index]].insert_or_assign(tick, ValueKey{current[index]});
        }
        state::editing::model::pruneChannels(channels);
        result.changed = true;
    }
    result.seek = clicks.seek;
    if (easingPopup(id, channels, {names[0], names[1], names[2]}, tick, clicks.easingMenu)) result.changed = true;

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::PushID(id);
    bool const edited = ImGui::ColorEdit3("##value", current, ImGuiColorEditFlags_Float);
    ImGui::PopID();
    if (edited) {
        for (int index = 0; index < 3; ++index) {
            staticValue[index]                 = current[index];
            channels[names[index]][tick].value = current[index];
        }
        result.changed = true;
    }
    return result;
}

} // namespace playback::editor::ui::keyframes
