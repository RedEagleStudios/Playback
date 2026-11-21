#pragma once

#include <string>

namespace playback::editor::ui {

// What the Stand-in page is about to hire, shared with the timeline: while `active`, the Replay lane shows the In and
// Out marks of the recording to take, and they drag there as on the page. UI thread only.
struct StandInDraft {
    bool active{};
    int  in{-1};
    int  out{-1};
    // The recorded player whose actions it takes: a UUID, empty for the recorder.
    std::string subject;
};

[[nodiscard]] inline StandInDraft& standInDraft() {
    static StandInDraft draft;
    return draft;
}

} // namespace playback::editor::ui
