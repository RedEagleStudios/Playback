#include "EasingPresetPicker.h"

#include "playback/editor/ui/components/Widgets.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

namespace playback::editor::ui {
using namespace ll::i18n_literals;
using state::editing::model::EasingDirection;
using state::editing::model::EasingFamily;
using state::editing::model::EasingPreset;
using state::editing::model::EasingPresets;

namespace {

// The table lists each family's In, Out and In-Out in that order.
constexpr std::size_t DirectionsPerFamily = 3;
static_assert(EasingPresets.size() % DirectionsPerFamily == 0);

std::string familyName(EasingFamily family) {
    switch (family) {
    case EasingFamily::Sine:
        return "playback.refactorEditor.details.easing.sine"_tr();
    case EasingFamily::Quad:
        return "playback.refactorEditor.details.easing.quad"_tr();
    case EasingFamily::Cubic:
        return "playback.refactorEditor.details.easing.cubic"_tr();
    case EasingFamily::Quart:
        return "playback.refactorEditor.details.easing.quart"_tr();
    case EasingFamily::Quint:
        return "playback.refactorEditor.details.easing.quint"_tr();
    case EasingFamily::Expo:
        return "playback.refactorEditor.details.easing.expo"_tr();
    case EasingFamily::Circ:
        return "playback.refactorEditor.details.easing.circ"_tr();
    case EasingFamily::Back:
    default:
        return "playback.refactorEditor.details.easing.back"_tr();
    }
}

std::string directionName(EasingDirection direction) {
    switch (direction) {
    case EasingDirection::In:
        return "playback.refactorEditor.details.easing.in"_tr();
    case EasingDirection::Out:
        return "playback.refactorEditor.details.easing.out"_tr();
    case EasingDirection::InOut:
    default:
        return "playback.refactorEditor.details.easing.inOut"_tr();
    }
}

} // namespace

std::string easingPresetName(EasingPreset const& preset) {
    return familyName(preset.family) + " " + directionName(preset.direction);
}

std::optional<EasingPreset> drawEasingPresetMenus(std::optional<std::size_t> current) {
    std::optional<EasingPreset> picked;
    for (std::size_t first = 0; first < EasingPresets.size(); first += DirectionsPerFamily) {
        if (!ImGui::BeginMenu(familyName(EasingPresets[first].family).c_str())) continue;
        for (std::size_t index = first; index < first + DirectionsPerFamily; ++index) {
            if (ImGui::MenuItem(directionName(EasingPresets[index].direction).c_str(), nullptr, current == index)) {
                picked = EasingPresets[index];
            }
        }
        ImGui::EndMenu();
    }
    return picked;
}

std::optional<EasingPick>
drawEasingPresetCombo(char const* id, char const* preview, std::optional<std::size_t> current, bool noneCurrent) {
    std::optional<EasingPick> picked;
    if (!widgets::beginCombo(id, preview)) return picked;
    if (ImGui::Selectable("playback.refactorEditor.details.easingPresetNone"_tr().c_str(), noneCurrent)) {
        picked = EasingPick{};
    }
    ImGui::Separator();
    for (std::size_t index = 0; index < EasingPresets.size(); ++index) {
        auto const& preset = EasingPresets[index];
        if (index % DirectionsPerFamily == 0) {
            if (index > 0) ImGui::Separator();
            ImGui::TextDisabled("%s", familyName(preset.family).c_str());
        }
        if (ImGui::Selectable(easingPresetName(preset).c_str(), current == index)) picked = EasingPick{preset};
    }
    widgets::endCombo();
    return picked;
}

} // namespace playback::editor::ui
