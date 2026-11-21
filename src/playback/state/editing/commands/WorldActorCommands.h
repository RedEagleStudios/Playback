#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::state::editing::command {

class SplitWorldActorAtPlayhead final : public model::IEditCommand {
public:
    explicit SplitWorldActorAtPlayhead(int tick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    std::optional<model::EditorStateExt> mBefore;
};

class TrimWorldActorSegment final : public model::IEditCommand {
public:
    TrimWorldActorSegment(std::string id, int start, int end);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    int                                  mStart;
    int                                  mEnd;
    std::optional<model::EditorStateExt> mBefore;
};

class DeleteWorldActorSegment final : public model::IEditCommand {
public:
    explicit DeleteWorldActorSegment(std::string id);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    std::optional<model::EditorStateExt> mBefore;
};

// Locks or unlocks every segment of the replay layer, so it can be neither cut nor trimmed.
class SetWorldActorLocked final : public model::IEditCommand {
public:
    explicit SetWorldActorLocked(bool locked);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    bool                                 mLocked;
    std::optional<std::vector<bool>>     mBefore;
};

} // namespace playback::state::editing::command
