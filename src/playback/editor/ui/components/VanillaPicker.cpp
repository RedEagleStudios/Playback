#include "VanillaPicker.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/editor/ui/components/Widgets.h"
#include "playback/editor/ui/iconfont.h"
#include "playback/overlay/VanillaAssets.h"

#include "ll/api/i18n/I18n.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

namespace {

bool containsInsensitive(std::string_view value, std::string_view query) {
    if (query.empty()) return true;
    return std::ranges::search(value, query, [](char left, char right) {
               return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
           }).begin()
        != value.end();
}

} // namespace

bool VanillaPicker::draw(char const* popupId, std::string& picked) {
    if (!widgets::beginAnchoredPopup(popupId, 320.0f * metrics::scale())) return false;
    bool chosen = false;

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint(
        "##vanilla-search",
        (ICON_SEARCH "  " + "playback.refactorEditor.layers.searchVanilla"_tr()).c_str(),
        mSearch.data(),
        mSearch.size()
    );
    std::string const filters[] = {
        "playback.refactorEditor.layers.vanillaAll"_tr(),
        "playback.refactorEditor.layers.vanillaBlocks"_tr(),
        "playback.refactorEditor.layers.vanillaItems"_tr(),
    };
    for (int index = 0; index < 3; ++index) {
        if (index > 0) ImGui::SameLine();
        if (ImGui::RadioButton(filters[index].c_str(), mFilter == index)) mFilter = index;
    }

    // The catalog is read from the game's packs the first time this opens.
    auto const&                                   assets = overlay::vanilla::catalog();
    std::vector<overlay::vanilla::Asset const*>   shown;
    shown.reserve(assets.size());
    for (auto const& asset : assets) {
        if ((mFilter == 1 && !asset.block) || (mFilter == 2 && asset.block)) continue;
        if (!containsInsensitive(asset.name, mSearch.data()) && !containsInsensitive(asset.id, mSearch.data())) continue;
        shown.push_back(&asset);
    }
    if (assets.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", "playback.refactorEditor.layers.vanillaUnavailable"_tr().c_str());
        ImGui::PopTextWrapPos();
    }
    if (ImGui::BeginChild("##vanilla-list", {0.0f, 360.0f * metrics::scale()})) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                auto const&       asset = *shown[static_cast<size_t>(row)];
                std::string const label =
                    std::string(asset.block ? ICON_VANILLA : ICON_VANILLA_ITEM) + "  " + asset.name + "##" + asset.id;
                if (ImGui::Selectable(label.c_str())) {
                    picked = asset.id;
                    chosen = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
    }
    ImGui::EndChild();
    widgets::endAnchoredPopup();
    return chosen;
}

} // namespace playback::editor::ui
