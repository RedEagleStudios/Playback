#include "TrackTreeModel.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace playback::editor::ui {
using namespace ll::i18n_literals;
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

std::string propertyLabel(std::string_view property) {
    std::string const key = "playback.refactorEditor.properties." + std::string{property};
    return std::string{ll::i18n::getInstance().get(key, {})};
}

void TrackTreeModel::setSearch(std::string_view query) { mSearch = query; }

void TrackTreeModel::rebuild(const EditorStateExt& state, std::set<std::string> const& expanded) {
    mRows.clear();
    mRows.reserve(state.overlays.size() + state.cameras.size() + 1);

    float const cameraHeight = metrics::cameraRow();
    // The row just pushed expands into one row per keyed property when it has any and is expanded.
    auto const addProperties = [&](ValueChannels const& channels, PropertyOwner owner, std::string const& ownerId) {
        auto& parent      = mRows.back();
        parent.expandable = !channels.empty();
        parent.expanded   = parent.expandable && expanded.contains(parent.id);
        if (!parent.expanded) return;
        TrackTreeRow const copy = parent;
        for (auto const& [name, channel] : channels) {
            if (channel.empty()) continue;
            TrackTreeRow row{TrackRowKind::Property, copy.id + "/" + name, propertyLabel(name)};
            row.height       = std::floor(cameraHeight * 1.25f);
            row.cameraId     = copy.cameraId;
            row.cameraIndex  = copy.cameraIndex;
            row.locked       = copy.locked;
            row.enabled      = copy.enabled;
            row.overlayId    = copy.overlayId;
            row.overlayIndex = copy.overlayIndex;
            row.overlayKind  = copy.overlayKind;
            row.property     = name;
            row.owner        = owner;
            row.ownerId      = ownerId;
            mRows.push_back(std::move(row));
        }
    };

    // Layers stack over the picture, so the topmost (last drawn) lists first, as in a compositor.
    for (int index = static_cast<int>(state.overlays.size()) - 1; index >= 0; --index) {
        auto const& item = state.overlays[static_cast<size_t>(index)];
        if (!containsInsensitive(item.name, mSearch)) continue;
        TrackTreeRow row{TrackRowKind::Overlay, "overlay:" + item.id, item.name};
        row.height       = cameraHeight;
        row.locked       = item.locked;
        row.enabled      = item.visible;
        row.overlayId    = item.id;
        row.overlayIndex = index;
        row.overlayKind  = item.kind;
        mRows.push_back(std::move(row));
        addProperties(item.channels, PropertyOwner::Overlay, item.id);
    }

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
        addProperties(camera.channels, PropertyOwner::Camera, camera.id);
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

    // The World's keyed settings: shown only once something there is keyed.
    std::string const worldLabel = "playback.refactorEditor.timeline.worldLayer"_tr();
    if (!state.world.channels.empty() && containsInsensitive(worldLabel, mSearch)) {
        TrackTreeRow world{TrackRowKind::World, "world", worldLabel};
        world.height = cameraHeight;
        mRows.push_back(std::move(world));
        addProperties(state.world.channels, PropertyOwner::World, {});
    }

    // The replay itself is the bottom layer: its segments are what plays, the gaps between them are cut out.
    std::string const replayLabel = "playback.refactorEditor.timeline.replayLayer"_tr();
    if (containsInsensitive(replayLabel, mSearch)) {
        TrackTreeRow replay{TrackRowKind::Replay, "replay", replayLabel};
        replay.height = cameraHeight;
        replay.locked = !state.worldActor.segments.empty()
                     && std::ranges::all_of(state.worldActor.segments, [](auto const& segment) {
                            return segment.locked;
                        });
        mRows.push_back(std::move(replay));
    }
}

const std::vector<TrackTreeRow>& TrackTreeModel::rows() const { return mRows; }

} // namespace playback::editor::ui
