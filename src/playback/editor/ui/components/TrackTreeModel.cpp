#include "TrackTreeModel.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include <algorithm>
#include <cctype>

namespace playback::editor::ui {

using namespace playback::state::editing::model;

namespace {

bool containsInsensitive(std::string_view value, std::string_view query) {
    if (query.empty()) return true;
    return std::search(
               value.begin(),
               value.end(),
               query.begin(),
               query.end(),
               [](char left, char right) {
                   return std::tolower(static_cast<unsigned char>(left))
                       == std::tolower(static_cast<unsigned char>(right));
               }
           )
        != value.end();
}

bool cameraMatchesSearch(const CameraEntity& camera, const WorldActor& worldActor, std::string_view query) {
    if (containsInsensitive(camera.name, query)) return true;
    auto subActor =
        std::find_if(worldActor.subActors.begin(), worldActor.subActors.end(), [&camera](const SubActor& actor) {
            return actor.id == camera.bindingEntityUuid;
        });
    return subActor != worldActor.subActors.end() && containsInsensitive(subActor->name, query);
}

} // namespace

void TrackTreeModel::setSearch(std::string_view query) { mSearch = query; }

void TrackTreeModel::rebuild(const EditorStateExt& state) {
    mRows.clear();
    mRows.reserve(state.cameras.size());

    float const cameraHeight = metrics::cameraRow();

    for (int index = 0; index < static_cast<int>(state.cameras.size()); ++index) {
        const auto& camera = state.cameras[index];
        if (!cameraMatchesSearch(camera, state.worldActor, mSearch)) continue;
        mRows.push_back(
            {TrackRowKind::Camera,
             "camera:" + camera.id,
             camera.name,
             camera.id,
             index,
             cameraHeight,
             camera.locked,
             camera.enabled,
             camera.kind}
        );
    }

    // One lane per posed entity, in the order its first key was made, named as its latest key names it.
    size_t const firstPoseRow = mRows.size();
    for (auto const& key : state.poseKeys) {
        auto row = std::find_if(mRows.begin() + static_cast<std::ptrdiff_t>(firstPoseRow), mRows.end(), [&](auto const& existing) {
            return existing.poseSubject == key.subject;
        });
        std::string const label = key.subjectLabel.empty() ? key.subject : key.subjectLabel;
        if (row != mRows.end()) {
            row->name = label;
            continue;
        }
        TrackTreeRow pose{TrackRowKind::Pose, "pose:" + key.subject, label};
        pose.height      = cameraHeight;
        pose.trackKind   = CameraTrackKind::FirstPerson;
        pose.poseSubject = key.subject;
        mRows.push_back(std::move(pose));
    }
    std::erase_if(mRows, [&](TrackTreeRow const& row) {
        return row.kind == TrackRowKind::Pose && !containsInsensitive(row.name, mSearch);
    });
}

const std::vector<TrackTreeRow>& TrackTreeModel::rows() const { return mRows; }

} // namespace playback::editor::ui
