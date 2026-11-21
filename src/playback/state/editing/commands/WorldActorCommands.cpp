#include "WorldActorCommands.h"

#include <algorithm>
#include <vector>

#include "playback/state/editing/WorldActorOps.h"

#include <utility>

namespace playback::state::editing::command {
namespace {

void restore(std::optional<model::EditorStateExt> const& before, model::EditorStateExt& state) {
    if (before) state = *before;
}

} // namespace

SplitWorldActorAtPlayhead::SplitWorldActorAtPlayhead(int tick) : mTick(tick) {}

void SplitWorldActorAtPlayhead::execute(model::EditorStateExt& state) {
    auto before = state;
    if (WorldActorOps::splitAt(state.worldActor, mTick).empty()) {
        mBefore.reset();
        return;
    }
    mBefore = std::move(before);
}

void        SplitWorldActorAtPlayhead::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SplitWorldActorAtPlayhead::label() const { return "Split World Actor"; }

TrimWorldActorSegment::TrimWorldActorSegment(std::string id, int start, int end)
: mId(std::move(id)),
  mStart(start),
  mEnd(end) {}

void TrimWorldActorSegment::execute(model::EditorStateExt& state) {
    auto before = state;
    if (!WorldActorOps::trimSegment(state.worldActor, mId, mStart, mEnd, state.totalTicks)) {
        mBefore.reset();
        return;
    }
    mBefore = std::move(before);
}

void        TrimWorldActorSegment::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string TrimWorldActorSegment::label() const { return "Trim World Actor"; }


DeleteWorldActorSegment::DeleteWorldActorSegment(std::string id) : mId(std::move(id)) {}

void DeleteWorldActorSegment::execute(model::EditorStateExt& state) {
    auto before = state;
    if (!WorldActorOps::deleteSegment(state.worldActor, mId)) {
        mBefore.reset();
        return;
    }
    mBefore = std::move(before);
}

void        DeleteWorldActorSegment::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteWorldActorSegment::label() const { return "Cut Replay Segment"; }

SetWorldActorLocked::SetWorldActorLocked(bool locked) : mLocked(locked) {}

void SetWorldActorLocked::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto& segments = state.worldActor.segments;
    if (std::ranges::all_of(segments, [this](auto const& segment) { return segment.locked == mLocked; })) return;
    std::vector<bool> before;
    before.reserve(segments.size());
    for (auto& segment : segments) {
        before.push_back(segment.locked);
        segment.locked = mLocked;
    }
    mBefore = std::move(before);
}

void SetWorldActorLocked::undo(model::EditorStateExt& state) {
    if (!mBefore) return;
    auto& segments = state.worldActor.segments;
    for (size_t index = 0; index < segments.size() && index < mBefore->size(); ++index) {
        segments[index].locked = (*mBefore)[index];
    }
}

std::string SetWorldActorLocked::label() const { return mLocked ? "Lock Replay Layer" : "Unlock Replay Layer"; }

} // namespace playback::state::editing::command
