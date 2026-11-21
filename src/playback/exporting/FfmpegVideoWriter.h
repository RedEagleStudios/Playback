#pragma once

#include "IFrameWriter.h"

#include <filesystem>
#include <memory>

namespace playback::exporting {

class FfmpegVideoWriter final : public IFrameWriter {
public:
    // Frames arrive at full supersampled size, so depth here costs several times what a normalized frame does.
    explicit FfmpegVideoWriter(uint32_t capacity = 2);
    ~FfmpegVideoWriter() override;

    FfmpegVideoWriter(FfmpegVideoWriter const&)            = delete;
    FfmpegVideoWriter& operator=(FfmpegVideoWriter const&) = delete;

    [[nodiscard]] static bool             isAvailable();
    // The bundled ffmpeg.exe, else one on PATH; empty when there is none. Screen layers decode videos with it too.
    [[nodiscard]] static std::filesystem::path const& executable();
    [[nodiscard]] bool                    open(CompiledExportPlan const& plan) override;
    [[nodiscard]] FrameWriterSubmitResult trySubmit(visuals::CapturedFrame& frame) override;
    [[nodiscard]] bool                    requestFinish() override;
    void                                  requestCancel() override;
    void                                  wait() override;
    [[nodiscard]] FrameWriterStatus       status() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace playback::exporting
