#include "OverlayCommands.h"

#include <algorithm>
#include <utility>

namespace playback::state::editing::command {

SetOverlays::SetOverlays(std::vector<model::OverlayItem> overlays) : mOverlays(std::move(overlays)) {
    for (auto& item : mOverlays) {
        item.startTick = std::max(0, item.startTick);
        item.endTick   = std::max(item.startTick + 1, item.endTick);
    }
}

void SetOverlays::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.overlays == mOverlays) return;
    mBefore        = state.overlays;
    state.overlays = mOverlays;
}

void SetOverlays::undo(model::EditorStateExt& state) {
    if (mBefore) state.overlays = *mBefore;
}

std::string SetOverlays::label() const { return "Change Layers"; }

} // namespace playback::state::editing::command
