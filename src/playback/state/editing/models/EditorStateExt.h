#pragma once

#include "CameraEntity.h"
#include "Track.h"
#include "PoseClip.h"
#include "WorldActor.h"
#include "WorldSettings.h"

#include <string>
#include <vector>

namespace playback::state::editing::model {

struct EditorStateExt {
    std::string projectName;
    std::string projectPath;

    int   currentTick{};
    int   totalTicks{};
    bool  playing{};
    float playbackSpeed{1.0f};

    WorldActor                worldActor;
    std::vector<CameraEntity> cameras;

    std::vector<Track> videoTracks;
    int                activeVideoTrackIdx{};

    std::vector<Transition> transitions;

    std::vector<Marker> markers;

    std::vector<PoseKey> poseKeys;

    WorldSettings world;
    // The replay's recorded camera views were added as camera tracks once; deleting them must not bring them back.
    bool recordedCameraTracksAdded{};

    float  fps{60.0f};
    size_t memoryUsageBytes{};
};

} // namespace playback::state::editing::model
