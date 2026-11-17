#include "WorldCommands.h"

namespace playback::state::editing::command {

SetWorldSettings::SetWorldSettings(model::WorldSettings settings) : mSettings(settings.sanitized()) {}

void SetWorldSettings::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.world == mSettings) return;
    mBefore     = state;
    state.world = mSettings;
}

void SetWorldSettings::undo(model::EditorStateExt& state) {
    if (mBefore) state = *mBefore;
}

std::string SetWorldSettings::label() const { return "Change World Settings"; }

} // namespace playback::state::editing::command
