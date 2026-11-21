#pragma once

#include "playback/state/editing/models/OverlayItem.h"

#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace playback::replay {

// A picture of a Screen layer, decoded at the size its screen shows and fitted to it: RGBA rows with any transparency
// already laid over black, so every alpha is 255.
struct MediaFrame {
    uint32_t             width{};
    uint32_t             height{};
    // The frame's number in its video (0 for a still).
    int64_t              index{};
    std::vector<uint8_t> rgba;
};

struct MediaInfo {
    double   duration{};
    double   fps{};
    uint32_t width{};
    uint32_t height{};
    // A single picture: an image file, or a video stream with no duration.
    bool     still{};
};

// Image files WIC decodes as one picture. GIFs are not among them: they play as videos.
[[nodiscard]] bool isStillImageFile(std::filesystem::path const& file);

// A UTF-8 path as the project stores it.
[[nodiscard]] std::filesystem::path mediaPath(std::string const& utf8);

// Reads the file's duration, frame rate and size with the bundled FFmpeg. Blocks for up to a few seconds.
[[nodiscard]] std::optional<MediaInfo> probeMedia(std::filesystem::path const& file);

// The file's first picture fitted into width x height (WIC; FFmpeg for what WIC cannot read). Null when unreadable.
[[nodiscard]] std::shared_ptr<MediaFrame const>
loadStillFrame(std::filesystem::path const& file, uint32_t width, uint32_t height, state::editing::model::ScreenFit fit);

// Decodes an encoded image (PNG...) held in memory to straight RGBA.
[[nodiscard]] bool
decodeImageBytes(std::span<uint8_t const> bytes, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba);

// One video decoded by an FFmpeg child process, already scaled and fitted to width x height at a constant frame rate,
// read in order on a worker thread. Moving backwards or far ahead starts the decoder again at the frame asked for.
class VideoStream {
public:
    VideoStream(
        std::filesystem::path            file,
        MediaInfo const&                 info,
        uint32_t                         width,
        uint32_t                         height,
        state::editing::model::ScreenFit fit
    );
    ~VideoStream();
    VideoStream(VideoStream const&)            = delete;
    VideoStream& operator=(VideoStream const&) = delete;

    // The frame showing at `seconds` into the video (held at the last frame past its end). `exact` waits for that frame
    // (up to a timeout, for exports); otherwise the newest decoded frame at or before it, or the last one handed out.
    // Null until the first frame is decoded.
    [[nodiscard]] std::shared_ptr<MediaFrame const> frameAt(double seconds, bool exact);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

// What the files the editor names hold, probed once each on a background thread. Any thread.
class MediaProbeCache {
public:
    struct Result {
        // False while the probe runs.
        bool                     ready{};
        std::optional<MediaInfo> info;
    };

    // Starts the probe on first ask; never blocks.
    [[nodiscard]] Result find(std::string const& file);
    // Waits for the probe.
    [[nodiscard]] std::optional<MediaInfo> get(std::string const& file);
    // Forgets every result, waiting for running probes.
    void clear();

private:
    [[nodiscard]] std::shared_future<std::optional<MediaInfo>> probe(std::string const& file);

    std::mutex                                                                   mMutex;
    std::unordered_map<std::string, std::shared_future<std::optional<MediaInfo>>> mProbes;
};

} // namespace playback::replay
