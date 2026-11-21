#include "ExportCoordinator.h"

#include "ExportPlanCompiler.h"
#include "FfmpegVideoWriter.h"
#include "FrameWorkerPool.h"
#include "PngSequenceWriter.h"
#include "FrameWriterUtils.h"

#include "playback/audio/ExportAudioCapture.h"
#include "playback/editor/graphics/CameraRenderHooks.h"
#include "playback/overlay/OverlayRenderer.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace playback::exporting {

namespace {

[[nodiscard]] std::unique_ptr<IFrameWriter> makeDefaultWriter(ExportFormat format) {
    if (format == ExportFormat::Mp4Video) {
        return std::make_unique<FfmpegVideoWriter>();
    }
    return std::make_unique<PngSequenceWriter>();
}

// Motion blur: the samples of the output frame being built, summed per byte; the average is the frame written. Every
// sample has the same layout, so the bytes add up channel by channel.
struct MotionBlend {
    std::vector<uint16_t> sums;
    uint32_t              count{};
    uint32_t              width{};
    uint32_t              height{};
    uint32_t              rowPitch{};
    // The driver offers a frame again after backpressure; one already summed is not added twice.
    uint64_t lastRenderIndex{std::numeric_limits<uint64_t>::max()};
    // The averaged frame until the writer takes it.
    std::optional<visuals::CapturedFrame> output;
};

[[nodiscard]] bool addSample(MotionBlend& blend, visuals::CapturedFrame const& frame) {
    if (frame.pixels.size() != static_cast<size_t>(frame.rowPitch) * frame.height || frame.pixels.empty()) return false;
    if (blend.count == 0) {
        blend.width    = frame.width;
        blend.height   = frame.height;
        blend.rowPitch = frame.rowPitch;
        blend.sums.assign(frame.pixels.size(), 0);
    } else if (frame.width != blend.width || frame.height != blend.height || frame.rowPitch != blend.rowPitch) {
        return false;
    }
    auto const* source = reinterpret_cast<uint8_t const*>(frame.pixels.data());
    std::function<void(uint32_t, uint32_t)> const rows = [&](uint32_t firstRow, uint32_t lastRow) {
        size_t const end = static_cast<size_t>(lastRow) * blend.rowPitch;
        for (size_t i = static_cast<size_t>(firstRow) * blend.rowPitch; i < end; ++i) blend.sums[i] += source[i];
    };
    frameWorkerPool().runRows(frame.height, rows);
    ++blend.count;
    return true;
}

// The output frame: the last sample's layout and presentation time, the average of all samples' pixels.
[[nodiscard]] visuals::CapturedFrame averageSamples(MotionBlend const& blend, visuals::CapturedFrame const& last) {
    visuals::CapturedFrame output;
    output.ticket            = last.ticket;
    output.ticket.frameIndex = last.ticket.frameIndex / blend.count;
    output.submission        = last.submission;
    output.width             = last.width;
    output.height            = last.height;
    output.rowPitch          = last.rowPitch;
    output.pixelFormat       = last.pixelFormat;
    output.colorSpace        = last.colorSpace;
    output.pixels.resize(blend.sums.size());
    auto* const    target = reinterpret_cast<uint8_t*>(output.pixels.data());
    uint32_t const count  = blend.count;
    std::function<void(uint32_t, uint32_t)> const rows = [&](uint32_t firstRow, uint32_t lastRow) {
        size_t const end = static_cast<size_t>(lastRow) * blend.rowPitch;
        for (size_t i = static_cast<size_t>(firstRow) * blend.rowPitch; i < end; ++i) {
            target[i] = static_cast<uint8_t>((blend.sums[i] + count / 2) / count);
        }
    };
    frameWorkerPool().runRows(blend.height, rows);
    return output;
}

// The camera's picture in output pixels: the writer keeps the centre of the captured frame at the output's shape and
// scales it to the output size, so the capture's edges (NDC +-1) land outside a narrower output.
[[nodiscard]] overlay::OverlayCamera
overlayCameraFor(editor::graphics::RenderCameraProjection const& projection, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t targetWidth, uint32_t targetHeight) {
    overlay::OverlayCamera camera;
    camera.viewProjection = projection.viewProjection;
    camera.nearClipW      = projection.nearClipW;
    bool const wider =
        static_cast<uint64_t>(sourceWidth) * targetHeight > static_cast<uint64_t>(sourceHeight) * targetWidth;
    double const cropWidth =
        wider ? static_cast<double>(sourceHeight) * targetWidth / targetHeight : static_cast<double>(sourceWidth);
    double const cropHeight =
        wider ? static_cast<double>(sourceHeight) : static_cast<double>(sourceWidth) * targetHeight / targetWidth;
    double const scale    = static_cast<double>(targetWidth) / std::max(1.0, cropWidth);
    double const cropLeft = (sourceWidth - cropWidth) * 0.5;
    double const cropTop  = (sourceHeight - cropHeight) * 0.5;
    camera.ndcLeft        = static_cast<float>(-cropLeft * scale);
    camera.ndcTop         = static_cast<float>(-cropTop * scale);
    camera.ndcRight       = static_cast<float>((sourceWidth - cropLeft) * scale);
    camera.ndcBottom      = static_cast<float>((sourceHeight - cropTop) * scale);
    return camera;
}

} // namespace

struct ExportCoordinator::Impl {
    explicit Impl(WriterFactory writerFactory) : factory(std::move(writerFactory)) {}

    WriterFactory                     factory;
    std::unique_ptr<IFrameWriter>     writer;
    std::optional<CompiledExportPlan> compiledPlan;
    std::mutex                        operationMutex;
    mutable std::mutex                mutex;
    ExportStatus                      currentStatus;
    ExportError                       pendingError{ExportError::None};
    std::string                       pendingMessage;
    // Touched only under operationMutex.
    MotionBlend                       blend;
    // The project's layers, drawn over every output frame. Touched only under operationMutex.
    std::vector<state::editing::model::OverlayItem> overlays;
    overlay::OverlayImage                           overlayImage;
    // The output frame the layers were last drawn on; the driver offers a frame again after backpressure.
    std::optional<int64_t> compositedPts;

    // Draws the layers onto an output frame at the output's size, once. False when the frame could not be sized.
    [[nodiscard]] bool compositeOverlays(visuals::CapturedFrame& frame) {
        if (overlays.empty() || !compiledPlan || compositedPts == frame.ticket.ptsNumerator) return true;
        compositedPts        = frame.ticket.ptsNumerator;
        auto const& settings = compiledPlan->settings;
        // Averaged frames carry their output index; unblended ones their render index, which is the same.
        uint64_t const outputIndex = frame.ticket.frameIndex;
        auto const     plan        = compiledPlan->frame(outputIndex * settings.motionBlurSamples);
        if (!plan) return true;
        double const tick =
            static_cast<double>(plan->replayTickNumerator) / static_cast<double>(plan->replayTickDenominator);
        if (!overlay::hasActiveOverlays(overlays, tick)) return true;

        uint32_t const sourceWidth  = frame.width;
        uint32_t const sourceHeight = frame.height;
        if (!detail::normalizeFrame(frame, settings.resolutionX, settings.resolutionY)) return false;
        std::optional<overlay::OverlayCamera> camera;
        if (auto const projection = editor::graphics::currentRenderCameraProjection()) {
            camera = overlayCameraFor(*projection, sourceWidth, sourceHeight, frame.width, frame.height);
        }
        if (!overlay::renderOverlays(overlays, tick, frame.width, frame.height, camera, overlayImage)) return true;
        overlay::compositeOver(
            reinterpret_cast<uint8_t*>(frame.pixels.data()),
            frame.width,
            frame.height,
            frame.rowPitch,
            frame.pixelFormat == visuals::FramePixelFormat::Bgra8,
            overlayImage
        );
        return true;
    }

    void setFailureLocked(ExportError error, std::string message) {
        currentStatus.state   = ExportState::Faulted;
        currentStatus.error   = error;
        currentStatus.message = std::move(message);
        pendingError          = ExportError::None;
        pendingMessage.clear();
    }

    void requestFailureLocked(ExportError error, std::string message) {
        if (pendingError == ExportError::None) {
            pendingError   = error;
            pendingMessage = std::move(message);
        }
        currentStatus.state = writer ? ExportState::Cancelling : ExportState::Faulted;
        currentStatus.error = ExportError::None;
    }

    void refreshWriterStatusLocked() {
        if (!writer) return;
        auto const writerStatus       = writer->status();
        currentStatus.format          = compiledPlan ? compiledPlan->settings.format : currentStatus.format;
        currentStatus.submittedFrames = writerStatus.submittedFrames;
        currentStatus.writtenFrames   = writerStatus.writtenFrames;
        currentStatus.latestFramePath = writerStatus.latestFramePath;

        if ((currentStatus.state == ExportState::Running || currentStatus.state == ExportState::Finalizing)
            && writerStatus.error != ExportError::None) {
            requestFailureLocked(writerStatus.error, writerStatus.message);
            writer->requestCancel();
            return;
        }
        if (currentStatus.state == ExportState::Finalizing && writerStatus.state == FrameWriterState::Completed) {
            currentStatus.state = ExportState::Completed;
            currentStatus.error = ExportError::None;
            return;
        }
        if (currentStatus.state != ExportState::Cancelling) return;

        if (writerStatus.state == FrameWriterState::Completed) {
            currentStatus.state = ExportState::Completed;
            currentStatus.error = ExportError::None;
        } else if (writerStatus.state == FrameWriterState::Cancelled) {
            if (pendingError != ExportError::None) {
                setFailureLocked(pendingError, pendingMessage);
            } else {
                currentStatus.state   = ExportState::Cancelled;
                currentStatus.error   = ExportError::Cancelled;
                currentStatus.message = "Export cancelled";
            }
        } else if (writerStatus.state == FrameWriterState::Faulted) {
            setFailureLocked(
                pendingError != ExportError::None ? pendingError : writerStatus.error,
                !pendingMessage.empty()
                    ? pendingMessage
                    : (writerStatus.message.empty() ? "The export writer failed" : writerStatus.message)
            );
        }
    }
};

ExportCoordinator::ExportCoordinator() : mImpl(std::make_unique<Impl>(makeDefaultWriter)) {}

ExportCoordinator::ExportCoordinator(WriterFactory factory)
: mImpl(std::make_unique<Impl>(factory ? std::move(factory) : WriterFactory(makeDefaultWriter))) {}

ExportCoordinator::~ExportCoordinator() { reset(); }

bool ExportCoordinator::start(ExportSettings settings, state::editing::model::EditorStateExt const& project) {
    std::scoped_lock              operationLock(mImpl->operationMutex);
    std::unique_ptr<IFrameWriter> previousWriter;
    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->refreshWriterStatusLocked();
        if (isExportActive(mImpl->currentStatus.state)) return false;
        previousWriter = std::move(mImpl->writer);
        mImpl->compiledPlan.reset();
        mImpl->pendingError = ExportError::None;
        mImpl->pendingMessage.clear();
        mImpl->blend         = {};
        mImpl->overlays      = project.overlays;
        mImpl->compositedPts.reset();
        mImpl->currentStatus        = {};
        mImpl->currentStatus.state  = ExportState::Preparing;
        mImpl->currentStatus.format = settings.format;
    }
    if (previousWriter) {
        previousWriter->requestCancel();
        previousWriter->wait();
    }

    auto compiled = ExportPlanCompiler::compile(settings, project);
    if (!compiled) {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->setFailureLocked(compiled.error, std::move(compiled.message));
        return false;
    }
    compiled.plan->outputPath = findAvailableExportPath(compiled.plan->outputPath);

    auto writer = mImpl->factory ? mImpl->factory(compiled.plan->settings.format) : nullptr;
    if (!writer) {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->setFailureLocked(ExportError::WriterUnavailable, "No export frame writer is available");
        return false;
    }
    if (!writer->open(*compiled.plan)) {
        auto const       writerStatus = writer->status();
        std::scoped_lock lock(mImpl->mutex);
        mImpl->setFailureLocked(
            writerStatus.error == ExportError::None ? ExportError::WriterUnavailable : writerStatus.error,
            writerStatus.message.empty() ? "Unable to open the export frame writer" : writerStatus.message
        );
        return false;
    }

    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->compiledPlan              = std::move(compiled.plan);
        mImpl->writer                    = std::move(writer);
        mImpl->currentStatus.state       = ExportState::Running;
        mImpl->currentStatus.format      = mImpl->compiledPlan->settings.format;
        mImpl->currentStatus.outputPath  = mImpl->compiledPlan->outputPath;
        mImpl->currentStatus.totalFrames = mImpl->compiledPlan->frameCount;
    }
    return true;
}

FrameWriterSubmitResult ExportCoordinator::trySubmit(visuals::CapturedFrame& frame) {
    std::scoped_lock operationLock(mImpl->operationMutex);
    IFrameWriter*    writer = nullptr;
    bool             invalidTicket{};
    uint32_t         samples{1};
    {
        std::scoped_lock lock(mImpl->mutex);
        if (mImpl->currentStatus.state != ExportState::Running || !mImpl->writer || !mImpl->compiledPlan) {
            return mImpl->currentStatus.state == ExportState::Faulted ? FrameWriterSubmitResult::Failed
                                                                      : FrameWriterSubmitResult::Closed;
        }
        auto const expected = mImpl->compiledPlan->frame(frame.ticket.frameIndex);
        if (!expected || frame.ticket.ptsNumerator != expected->ticket.ptsNumerator
            || frame.ticket.ptsDenominator != expected->ticket.ptsDenominator) {
            mImpl->requestFailureLocked(
                ExportError::InvalidFrame,
                "The captured frame ticket does not match the export plan"
            );
            invalidTicket = true;
        }
        writer  = mImpl->writer.get();
        samples = mImpl->compiledPlan->settings.motionBlurSamples;
    }
    if (invalidTicket) {
        writer->requestCancel();
        return FrameWriterSubmitResult::Failed;
    }

    // With motion blur every sample but each output frame's last is only summed; the last completes the average,
    // which is held until the writer takes it.
    visuals::CapturedFrame* output = &frame;
    if (samples > 1) {
        auto& blend = mImpl->blend;
        if (!blend.output) {
            bool added = frame.ticket.frameIndex == blend.lastRenderIndex;
            if (!added) {
                added                 = addSample(blend, frame);
                blend.lastRenderIndex = frame.ticket.frameIndex;
            }
            if (!added) {
                {
                    std::scoped_lock lock(mImpl->mutex);
                    mImpl->requestFailureLocked(
                        ExportError::InvalidFrame,
                        "A motion blur sample does not match the size of the frame it is averaged into"
                    );
                }
                writer->requestCancel();
                return FrameWriterSubmitResult::Failed;
            }
            if (blend.count < samples) return FrameWriterSubmitResult::Accepted;
            blend.output = averageSamples(blend, frame);
        }
        output = &*blend.output;
    }

    if (!mImpl->compositeOverlays(*output)) {
        {
            std::scoped_lock lock(mImpl->mutex);
            mImpl->requestFailureLocked(
                ExportError::InvalidFrame,
                "The captured frame could not be sized to draw the layers on it"
            );
        }
        writer->requestCancel();
        return FrameWriterSubmitResult::Failed;
    }

    auto const result = writer->trySubmit(*output);
    // Advance the export audio ledger exactly once per accepted output frame (warm-up and extra renders never reach
    // here).
    if (result == FrameWriterSubmitResult::Accepted) {
        playback::audio::exportcap::onFrameAccepted(output->ticket.frameIndex);
        if (samples > 1) {
            mImpl->blend.output.reset();
            mImpl->blend.count = 0;
        }
    }
    auto const writerStatus = writer->status();
    bool       writerFailed = result == FrameWriterSubmitResult::Failed || writerStatus.error != ExportError::None;
    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->currentStatus.submittedFrames = writerStatus.submittedFrames;
        mImpl->currentStatus.writtenFrames   = writerStatus.writtenFrames;
        mImpl->currentStatus.latestFramePath = writerStatus.latestFramePath;
        if (writerFailed) {
            mImpl->requestFailureLocked(
                writerStatus.error == ExportError::None ? ExportError::WriteFailed : writerStatus.error,
                writerStatus.message.empty() ? "The export frame writer failed" : writerStatus.message
            );
        }
    }
    if (writerFailed) writer->requestCancel();
    return result;
}

bool ExportCoordinator::finish() {
    std::scoped_lock operationLock(mImpl->operationMutex);
    IFrameWriter*    writer = nullptr;
    bool             frameCountMismatch{};
    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->refreshWriterStatusLocked();
        if (mImpl->currentStatus.state != ExportState::Running || !mImpl->writer || !mImpl->compiledPlan) return false;
        if (mImpl->currentStatus.submittedFrames != mImpl->compiledPlan->frameCount) {
            mImpl->requestFailureLocked(
                ExportError::FrameCountMismatch,
                "The export ended before all frames were submitted"
            );
            frameCountMismatch = true;
        } else {
            mImpl->currentStatus.state = ExportState::Finalizing;
        }
        writer = mImpl->writer.get();
    }
    if (frameCountMismatch) {
        writer->requestCancel();
        return false;
    }
    return writer->requestFinish();
}

void ExportCoordinator::fail(ExportError error, std::string message) {
    std::scoped_lock operationLock(mImpl->operationMutex);
    IFrameWriter*    writer = nullptr;
    {
        std::scoped_lock lock(mImpl->mutex);
        if (!isExportActive(mImpl->currentStatus.state)) {
            mImpl->setFailureLocked(error, std::move(message));
            return;
        }
        mImpl->requestFailureLocked(error, std::move(message));
        writer = mImpl->writer.get();
    }
    if (writer) writer->requestCancel();
}

void ExportCoordinator::cancel() {
    std::scoped_lock operationLock(mImpl->operationMutex);
    IFrameWriter*    writer = nullptr;
    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->refreshWriterStatusLocked();
        if (!isExportActive(mImpl->currentStatus.state) || !mImpl->writer) return;
        mImpl->pendingError = ExportError::None;
        mImpl->pendingMessage.clear();
        mImpl->currentStatus.state   = ExportState::Cancelling;
        mImpl->currentStatus.error   = ExportError::None;
        mImpl->currentStatus.message = "Cancelling export";
        writer                       = mImpl->writer.get();
    }
    writer->requestCancel();
}

void ExportCoordinator::reset() {
    std::scoped_lock              operationLock(mImpl->operationMutex);
    std::unique_ptr<IFrameWriter> writer;
    {
        std::scoped_lock lock(mImpl->mutex);
        writer = std::move(mImpl->writer);
        mImpl->compiledPlan.reset();
        mImpl->pendingError = ExportError::None;
        mImpl->pendingMessage.clear();
        mImpl->blend         = {};
        mImpl->currentStatus = {};
    }
    if (writer) {
        writer->requestCancel();
        writer->wait();
    }
}

ExportStatus ExportCoordinator::status() const {
    std::scoped_lock lock(mImpl->mutex);
    mImpl->refreshWriterStatusLocked();
    return mImpl->currentStatus;
}

std::optional<CompiledExportPlan> ExportCoordinator::plan() const {
    std::scoped_lock lock(mImpl->mutex);
    return mImpl->compiledPlan;
}

bool ExportCoordinator::isFormatAvailable(ExportFormat format) {
    return format == ExportFormat::PngSequence || FfmpegVideoWriter::isAvailable();
}

} // namespace playback::exporting
