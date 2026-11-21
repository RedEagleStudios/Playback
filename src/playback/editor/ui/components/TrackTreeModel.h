#pragma once

#include "playback/state/editing/models/CameraEntity.h"
#include "playback/state/editing/models/OverlayItem.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace playback::state::editing::model {
struct EditorStateExt;
}

namespace playback::editor::ui {

// Overlay layers (topmost first), camera tracks, one lane per entity that has pose keyframes, the World's keyed
// settings, then the replay itself. An expanded camera, layer or World row is followed by one Property row per keyed
// property.
enum class TrackRowKind { Camera, Pose, Overlay, World, Property, Replay };

// What a Property row's keys belong to.
enum class PropertyOwner { Camera, Overlay, World };

// A keyable property's name as the user reads it ("fov" reads "Field of view").
[[nodiscard]] std::string propertyLabel(std::string_view property);

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
    // Overlay lanes: the layer and its index in EditorStateExt::overlays.
    std::string                        overlayId;
    int                                overlayIndex{-1};
    state::editing::model::OverlayKind overlayKind{state::editing::model::OverlayKind::Title};
    // Rows whose properties are keyed can expand into one Property row each.
    bool expandable{};
    bool expanded{};
    // Property rows: the channel's name and what owns it (ownerId is the camera or layer id; empty for the World).
    std::string   property;
    PropertyOwner owner{PropertyOwner::Camera};
    std::string   ownerId;
};

class TrackTreeModel {
public:
    void setSearch(std::string_view query);
    // `expanded` holds the ids of rows shown with their property rows.
    void rebuild(state::editing::model::EditorStateExt const& state, std::set<std::string> const& expanded);
    [[nodiscard]] std::vector<TrackTreeRow> const& rows() const;

private:
    std::string               mSearch;
    std::vector<TrackTreeRow> mRows;
};

} // namespace playback::editor::ui
