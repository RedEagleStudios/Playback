#pragma once

#include <array>
#include <string>

namespace playback::editor::ui {

// A searchable list of Minecraft's own blocks and items, in a popup the caller opens (widgets::openAnchoredPopup) from
// the same window.
class VanillaPicker {
public:
    // Draws the popup while it is open; true on the frame one is picked, with its id ("minecraft:block/stone") in
    // `picked`.
    bool draw(char const* popupId, std::string& picked);

private:
    std::array<char, 128> mSearch{};
    // 0 everything, 1 blocks, 2 items.
    int mFilter{};
};

} // namespace playback::editor::ui
