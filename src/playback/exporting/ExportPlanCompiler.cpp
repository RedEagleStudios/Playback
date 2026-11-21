#include "ExportPlanCompiler.h"

#include "playback/state/editing/models/EditorStateExt.h"

#include <cctype>
#include <limits>
#include <utility>
#include <vector>

namespace playback::exporting {

namespace {

constexpr int64_t  ReplayTicksPerSecond = 20;
constexpr int64_t  MaxFrameRateValue    = 1'000'000;
constexpr uint64_t MaxExportFrames      = 1'000'000;
constexpr uint32_t MaxResolution        = MaxExportResolution;
constexpr uint32_t MaxSsaa              = MaxExportSsaa;
constexpr uint32_t MaxWarmupFrames      = 3'600;
constexpr uint64_t MaxFramePixels       = MaxExportPixels;

[[nodiscard]] int64_t gcd(int64_t left, int64_t right) {
    while (right != 0) {
        int64_t const remainder = left % right;
        left                    = right;
        right                   = remainder;
    }
    return left == 0 ? 1 : left;
}

[[nodiscard]] bool checkedMultiply(uint64_t left, uint64_t right, uint64_t& result) {
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) return false;
    result = left * right;
    return true;
}

[[nodiscard]] bool checkedAdd(uint64_t left, uint64_t right, uint64_t& result) {
    if (right > std::numeric_limits<uint64_t>::max() - left) return false;
    result = left + right;
    return true;
}

[[nodiscard]] bool checkedSignedMultiply(int64_t left, int64_t right, int64_t& result) {
    if (left == 0 || right == 0) {
        result = 0;
        return true;
    }
    if (left == -1 && right == std::numeric_limits<int64_t>::min()) return false;
    if (right == -1 && left == std::numeric_limits<int64_t>::min()) return false;
    if (left > 0 && right > 0 && left > std::numeric_limits<int64_t>::max() / right) return false;
    if (left < 0 && right < 0 && left < std::numeric_limits<int64_t>::max() / right) return false;
    if (left > 0 && right < 0 && right < std::numeric_limits<int64_t>::min() / left) return false;
    if (left < 0 && right > 0 && left < std::numeric_limits<int64_t>::min() / right) return false;
    result = left * right;
    return true;
}

[[nodiscard]] bool checkedSignedAdd(int64_t left, int64_t right, int64_t& result) {
    if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right)
        || (right < 0 && left < std::numeric_limits<int64_t>::min() - right)) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool validOutputName(std::string const& name) {
    if (name.empty() || name.size() > 128 || name == "." || name == ".." || name.back() == '.' || name.back() == ' ')
        return false;
    for (unsigned char const character : name) {
        if (std::iscntrl(character) || character == '/' || character == '\\' || character == ':' || character == '*'
            || character == '?' || character == '"' || character == '<' || character == '>' || character == '|') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasMp4Extension(std::string const& name) {
    if (name.size() < 4) return false;
    auto const offset = name.size() - 4;
    return name[offset] == '.' && std::tolower(static_cast<unsigned char>(name[offset + 1])) == 'm'
        && std::tolower(static_cast<unsigned char>(name[offset + 2])) == 'p' && name[offset + 3] == '4';
}

[[nodiscard]] FrameRate normalizeFrameRate(FrameRate rate) {
    if (rate.numerator <= 0 || rate.denominator <= 0) return rate;
    auto const divisor  = gcd(rate.numerator, rate.denominator);
    rate.numerator     /= divisor;
    rate.denominator   /= divisor;
    return rate;
}

[[nodiscard]] std::string outputBaseName(std::string name) {
    if (hasMp4Extension(name)) name.resize(name.size() - 4);
    return name;
}

[[nodiscard]] std::filesystem::path utf8Path(std::string const& value) {
    auto const* begin = reinterpret_cast<char8_t const*>(value.data());
    return std::filesystem::path(std::u8string{begin, begin + value.size()});
}

[[nodiscard]] ExportPlanCompileResult failure(ExportError error, char const* message) {
    return ExportPlanCompileResult{std::nullopt, error, message};
}

} // namespace

std::filesystem::path buildExportOutputPath(ExportSettings const& settings) {
    auto const baseName = outputBaseName(settings.outputName);
    if (settings.format == ExportFormat::Mp4Video) return settings.outputDirectory / utf8Path(baseName + ".mp4");
    if (settings.stillFrame) return settings.outputDirectory / utf8Path(baseName + ".png");
    // A frame sequence is many files, so it still needs a folder of its own to live in.
    return settings.outputDirectory / utf8Path(baseName);
}

std::filesystem::path findAvailableExportPath(std::filesystem::path const& desired) {
    std::error_code error;
    if (!std::filesystem::exists(desired, error) && !error) return desired;

    auto const parent    = desired.parent_path();
    auto const extension = desired.extension();
    auto const stem      = desired.stem();
    for (int index = 1; index < 10'000; ++index) {
        auto suffix    = std::filesystem::path{" (" + std::to_string(index) + ")"};
        auto candidate = parent / (stem.native() + suffix.native() + extension.native());
        if (!std::filesystem::exists(candidate, error) || error) return candidate;
    }
    return desired;
}

std::optional<ExportFramePlan> CompiledExportPlan::frame(uint64_t renderIndex) const {
    uint64_t const samples = settings.motionBlurSamples;
    if (samples == 0 || renderIndex >= renderFrameCount()) return std::nullopt;
    uint64_t const frameIndex = renderIndex / samples;
    auto const     sample     = static_cast<int64_t>(renderIndex % samples);

    int64_t frameOffset = 0;
    if (frameIndex > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
    if (!checkedSignedMultiply(static_cast<int64_t>(frameIndex), replayTickStepNumerator, frameOffset)) {
        return std::nullopt;
    }

    int64_t timelineTickNumerator = 0;
    if (!checkedSignedAdd(replayTickStartNumerator, frameOffset, timelineTickNumerator)) return std::nullopt;
    // The output's clock runs through the kept stretches end to end; the replay time is where that lands in them.
    int64_t replayTickNumerator = timelineTickNumerator;
    int64_t remaining           = frameOffset;
    for (size_t index = 0; index < keptRanges.size(); ++index) {
        auto const [first, last] = keptRanges[index];
        int64_t span             = 0;
        int64_t firstNumerator   = 0;
        if (!checkedSignedMultiply(last - first, replayTickDenominator, span)
            || !checkedSignedMultiply(first, replayTickDenominator, firstNumerator)) {
            return std::nullopt;
        }
        if (remaining < span || index + 1 == keptRanges.size()) {
            if (!checkedSignedAdd(firstNumerator, remaining, replayTickNumerator)) return std::nullopt;
            break;
        }
        remaining -= span;
    }
    int64_t tickDenominator = replayTickDenominator;
    if (samples > 1) {
        // Sample k of n sits k/n of the way through the open shutter, the first on the frame's own time. Exact over
        // denominator * n * 360, so no sample drifts from its frame.
        int64_t const scale        = static_cast<int64_t>(samples) * 360;
        int64_t       sampleOffset = 0;
        if (!checkedSignedMultiply(replayTickNumerator, scale, replayTickNumerator)
            || !checkedSignedMultiply(timelineTickNumerator, scale, timelineTickNumerator)
            || !checkedSignedMultiply(
                replayTickStepNumerator,
                sample * static_cast<int64_t>(settings.motionBlurShutter),
                sampleOffset
            )
            || !checkedSignedAdd(replayTickNumerator, sampleOffset, replayTickNumerator)
            || !checkedSignedAdd(timelineTickNumerator, sampleOffset, timelineTickNumerator)
            || !checkedSignedMultiply(replayTickDenominator, scale, tickDenominator)) {
            return std::nullopt;
        }
    }

    int64_t ptsNumerator = 0;
    if (!checkedSignedMultiply(static_cast<int64_t>(frameIndex), settings.frameRate.denominator, ptsNumerator)) {
        return std::nullopt;
    }

    ExportFramePlan result;
    result.replayTickNumerator   = replayTickNumerator;
    result.replayTickDenominator = tickDenominator;
    result.timelineTickNumerator = timelineTickNumerator;
    result.ticket.frameIndex     = renderIndex;
    result.ticket.ptsNumerator   = ptsNumerator;
    result.ticket.ptsDenominator = settings.frameRate.numerator;
    return result;
}

ExportPlanCompileResult
ExportPlanCompiler::compile(ExportSettings const& settings, state::editing::model::EditorStateExt const& project) {
    if (settings.format != ExportFormat::PngSequence && settings.format != ExportFormat::Mp4Video) {
        return failure(ExportError::InvalidSettings, "The requested export format is not supported");
    }
    if (settings.outputDirectory.empty() || !validOutputName(settings.outputName)
        || outputBaseName(settings.outputName).empty()) {
        return failure(ExportError::InvalidOutputPath, "The export output path is invalid");
    }
    if (settings.startTick < 0 || settings.endTick <= settings.startTick) {
        return failure(ExportError::InvalidTimeline, "The export timeline must have a positive duration");
    }
    if (project.totalTicks < 0 || settings.endTick > project.totalTicks) {
        return failure(ExportError::InvalidTimeline, "The export timeline exceeds the replay duration");
    }
    if (settings.frameRate.numerator <= 0 || settings.frameRate.denominator <= 0
        || settings.frameRate.numerator > MaxFrameRateValue || settings.frameRate.denominator > MaxFrameRateValue) {
        return failure(ExportError::InvalidSettings, "The export frame rate is invalid");
    }
    if ((settings.resolutionX == 0) != (settings.resolutionY == 0)) {
        return failure(ExportError::InvalidSettings, "The export resolution must specify both width and height");
    }
    if (settings.resolutionX == 0 && settings.ssaa != 1) {
        return failure(ExportError::InvalidSettings, "SSAA requires an explicit output resolution");
    }
    if (settings.resolutionX > MaxResolution || settings.resolutionY > MaxResolution || settings.ssaa == 0
        || settings.ssaa > MaxSsaa) {
        return failure(ExportError::InvalidSettings, "The export resolution or SSAA value is invalid");
    }
    if (settings.resolutionX != 0) {
        if (settings.resolutionX < 16 || settings.resolutionY < 16) {
            return failure(ExportError::InvalidSettings, "The export resolution is too small");
        }
        auto const scaledWidth  = static_cast<uint64_t>(settings.resolutionX) * settings.ssaa;
        auto const scaledHeight = static_cast<uint64_t>(settings.resolutionY) * settings.ssaa;
        if (scaledWidth > MaxResolution || scaledHeight > MaxResolution
            || static_cast<uint64_t>(settings.resolutionX) * settings.resolutionY > MaxFramePixels
            || scaledWidth * scaledHeight > MaxFramePixels) {
            return failure(ExportError::InvalidSettings, "The supersampled render resolution is too large");
        }
    }
    if (settings.warmupFrames > MaxWarmupFrames) {
        return failure(ExportError::InvalidSettings, "The export warm-up frame count is too large");
    }
    if (settings.extraRenders > MaxExportExtraRenders) {
        return failure(ExportError::InvalidSettings, "The export extra render count is too large");
    }
    if (settings.motionBlurSamples == 0 || settings.motionBlurSamples > MaxMotionBlurSamples
        || settings.motionBlurShutter == 0 || settings.motionBlurShutter > MaxMotionBlurShutter) {
        return failure(ExportError::InvalidSettings, "The motion blur sample count or shutter angle is invalid");
    }
    if (settings.videoCrf > MaxExportCrf || settings.encodeSpeed > VideoEncodeSpeed::Fastest) {
        return failure(ExportError::InvalidSettings, "The video encoder quality or speed setting is invalid");
    }

    ExportSettings normalized = settings;
    normalized.frameRate      = normalizeFrameRate(normalized.frameRate);

    if (settings.stillFrame && settings.format != ExportFormat::PngSequence) {
        return failure(ExportError::InvalidSettings, "A still frame is written as PNG");
    }

    // The replay layer's segments are what plays; a cut between two is left out of the output.
    std::vector<std::pair<int64_t, int64_t>> keptRanges;
    if (project.worldActor.segments.empty() || settings.stillFrame) {
        keptRanges.emplace_back(settings.startTick, settings.endTick);
    } else {
        for (auto const& segment : project.worldActor.segments) {
            int64_t const first = std::max<int64_t>(settings.startTick, segment.startTick);
            int64_t const last  = std::min<int64_t>(settings.endTick, segment.endTick);
            if (first < last) keptRanges.emplace_back(first, last);
        }
    }
    if (keptRanges.empty()) {
        return failure(ExportError::InvalidTimeline, "The whole export range is cut out of the replay layer");
    }
    // The replay starts the export where the output's first frame is.
    normalized.startTick = keptRanges.front().first;
    uint64_t durationTicks = 0;
    for (auto const& [first, last] : keptRanges) durationTicks += static_cast<uint64_t>(last - first);
    uint64_t       denominator   = 0;
    if (!checkedMultiply(
            static_cast<uint64_t>(ReplayTicksPerSecond),
            static_cast<uint64_t>(normalized.frameRate.denominator),
            denominator
        )) {
        return failure(ExportError::InvalidSettings, "The export frame rate denominator is too large");
    }

    uint64_t wholeFrames         = durationTicks / denominator;
    uint64_t remainder           = durationTicks % denominator;
    uint64_t fractionalNumerator = 0;
    if (!checkedMultiply(remainder, static_cast<uint64_t>(normalized.frameRate.numerator), fractionalNumerator)) {
        return failure(ExportError::InvalidTimeline, "The export duration is too large");
    }
    uint64_t fractionalFrames = fractionalNumerator / denominator;
    if (fractionalNumerator % denominator != 0) ++fractionalFrames;

    uint64_t frameCount = 0;
    if (!checkedMultiply(wholeFrames, static_cast<uint64_t>(normalized.frameRate.numerator), frameCount)
        || !checkedAdd(frameCount, fractionalFrames, frameCount) || frameCount == 0 || frameCount > MaxExportFrames) {
        return failure(ExportError::InvalidTimeline, "The export timeline produces too many frames");
    }
    if (settings.stillFrame) frameCount = 1;
    if (frameCount - 1 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return failure(ExportError::InvalidTimeline, "The export frame index is too large");
    }

    int64_t replayStartNumerator = 0;
    if (!checkedSignedMultiply(keptRanges.front().first, normalized.frameRate.numerator, replayStartNumerator)) {
        return failure(ExportError::InvalidTimeline, "The export start tick is too large");
    }
    int64_t replayStepNumerator = 0;
    if (!checkedSignedMultiply(ReplayTicksPerSecond, normalized.frameRate.denominator, replayStepNumerator)) {
        return failure(ExportError::InvalidSettings, "The export frame rate is too large");
    }
    int64_t lastOffset = 0;
    if (!checkedSignedMultiply(static_cast<int64_t>(frameCount - 1), replayStepNumerator, lastOffset)) {
        return failure(ExportError::InvalidTimeline, "The export timeline is too large");
    }
    int64_t lastSample = 0;
    if (!checkedSignedAdd(replayStartNumerator, lastOffset, lastSample)) {
        return failure(ExportError::InvalidTimeline, "The export timeline is too large");
    }
    (void)lastSample;

    CompiledExportPlan plan;
    plan.settings                 = std::move(normalized);
    plan.outputPath               = buildExportOutputPath(plan.settings);
    plan.frameCount               = frameCount;
    plan.replayTickStartNumerator = replayStartNumerator;
    plan.replayTickStepNumerator  = replayStepNumerator;
    plan.replayTickDenominator    = plan.settings.frameRate.numerator;
    plan.keptRanges               = std::move(keptRanges);
    if (!plan.frame(plan.renderFrameCount() - 1)) {
        return failure(ExportError::InvalidTimeline, "The export timeline is too large for its motion blur samples");
    }
    return ExportPlanCompileResult{std::move(plan), ExportError::None, {}};
}

} // namespace playback::exporting
