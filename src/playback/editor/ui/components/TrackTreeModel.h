#pragma once

#include "playback/state/editing/models/CameraEntity.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace playback::state::editing::model {
struct EditorStateExt;
}

namespace playback::editor::ui {

// Camera tracks, then one lane per entity that has pose keyframes.
enum class TrackRowKind { Camera, Pose };

struct TrackTreeRow {
    TrackRowKind                           kind;
    std::string                            id;
    std::string                            name;
    std::string                            cameraId;
    int                                    cameraIndex{-1};
    float                                  height{};
    bool                                   locked{};
    bool                                   enabled{true};
    state::editing::model::CameraTrackKind trackKind{state::editing::model::CameraTrackKind::Keyframe};
    // Pose lanes: the entity their keys pose.
    std::string poseSubject;
};

class TrackTreeModel {
public:
    void                                           setSearch(std::string_view query);
    void                                           rebuild(state::editing::model::EditorStateExt const& state);
    [[nodiscard]] std::vector<TrackTreeRow> const& rows() const;

private:
    std::string               mSearch;
    std::vector<TrackTreeRow> mRows;
};

} // namespace playback::editor::ui
