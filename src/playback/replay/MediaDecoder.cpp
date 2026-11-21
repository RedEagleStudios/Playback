#include "MediaDecoder.h"

#include "playback/Playback.h"
#include "playback/exporting/FfmpegVideoWriter.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <deque>
#include <format>
#include <limits>
#include <regex>
#include <thread>
#include <utility>

namespace playback::replay {

namespace {

using Microsoft::WRL::ComPtr;
using state::editing::model::ScreenFit;

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

[[nodiscard]] std::string utf8(std::filesystem::path const& file) {
    auto const text = file.u8string();
    return {reinterpret_cast<char const*>(text.data()), text.size()};
}

constexpr uint32_t MaxImageDimension = 16384;
// Frames decoded ahead of the one showing.
constexpr size_t   FramesAhead       = 6;
// A jump further ahead than this starts the decoder again at the target instead of decoding up to it.
constexpr double   SkipAheadSeconds  = 2.0;
constexpr auto     ExactFrameTimeout = std::chrono::seconds{10};
constexpr auto     ProbeTimeout      = std::chrono::seconds{15};
constexpr double   MaxFrameRate      = 60.0;

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : mHandle(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(UniqueHandle const&)            = delete;
    UniqueHandle& operator=(UniqueHandle const&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : mHandle(std::exchange(other.mHandle, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(std::exchange(other.mHandle, nullptr));
        return *this;
    }

    [[nodiscard]] HANDLE get() const { return mHandle; }
    void                 reset(HANDLE handle = nullptr) {
        if (mHandle && mHandle != INVALID_HANDLE_VALUE) CloseHandle(mHandle);
        mHandle = handle;
    }
    [[nodiscard]] explicit operator bool() const { return mHandle && mHandle != INVALID_HANDLE_VALUE; }

private:
    HANDLE mHandle{};
};

class ComScope {
public:
    ComScope() : mNeedsUninit(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() {
        if (mNeedsUninit) CoUninitialize();
    }
    ComScope(ComScope const&)            = delete;
    ComScope& operator=(ComScope const&) = delete;

private:
    bool mNeedsUninit;
};

// Decoders die with the game: every one is in this job, which kills them when its handle closes.
HANDLE decoderJob() {
    static HANDLE const job = [] {
        HANDLE created = CreateJobObjectW(nullptr, nullptr);
        if (!created) return HANDLE{};
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(created);
            return HANDLE{};
        }
        return created;
    }();
    return job;
}

[[nodiscard]] std::wstring quoteArgument(std::wstring const& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
    std::wstring result = L"\"";
    size_t       slashes = 0;
    for (wchar_t const character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

struct ChildProcess {
    UniqueHandle process;
    // The child's stdout, or its stderr when it was started to report.
    UniqueHandle output;
};

// Starts ffmpeg with no console window. Only the pipe and NUL are handed to it, so an export's encoder started at the
// same moment does not inherit this pipe (and the other way round).
[[nodiscard]] std::optional<ChildProcess> launchFfmpeg(std::wstring const& arguments, bool reportOnly) {
    auto const& executable = exporting::FfmpegVideoWriter::executable();
    if (executable.empty()) return std::nullopt;

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE              readRaw{};
    HANDLE              writeRaw{};
    if (!CreatePipe(&readRaw, &writeRaw, &inheritable, 1u << 20)) return std::nullopt;
    UniqueHandle read(readRaw);
    UniqueHandle write(writeRaw);
    if (!SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0)) return std::nullopt;
    UniqueHandle nul(CreateFileW(
        L"NUL",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &inheritable,
        OPEN_EXISTING,
        0,
        nullptr
    ));
    if (!nul) return std::nullopt;

    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<std::byte> attributeStorage(attributeSize);
    auto* const attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)) return std::nullopt;
    HANDLE inherited[] = {write.get(), nul.get()};
    bool const listed  = UpdateProcThreadAttribute(
        attributes,
        0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited,
        sizeof(inherited),
        nullptr,
        nullptr
    );

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb         = sizeof(startup);
    startup.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput  = nul.get();
    startup.StartupInfo.hStdOutput = reportOnly ? nul.get() : write.get();
    startup.StartupInfo.hStdError  = reportOnly ? write.get() : nul.get();
    startup.lpAttributeList        = attributes;

    std::wstring         command = quoteArgument(executable.wstring()) + L" " + arguments;
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');
    PROCESS_INFORMATION info{};
    BOOL const          created = listed
                         && CreateProcessW(
                             executable.c_str(),
                             commandLine.data(),
                             nullptr,
                             nullptr,
                             TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                             nullptr,
                             nullptr,
                             &startup.StartupInfo,
                             &info
                         );
    DeleteProcThreadAttributeList(attributes);
    if (!created) return std::nullopt;
    if (HANDLE const job = decoderJob()) AssignProcessToJobObject(job, info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    return ChildProcess{UniqueHandle(info.hProcess), std::move(read)};
}

[[nodiscard]] bool readExact(HANDLE pipe, uint8_t* data, size_t size) {
    while (size > 0) {
        DWORD      read{};
        auto const chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 24));
        if (!ReadFile(pipe, data, chunk, &read, nullptr) || read == 0) return false;
        data += read;
        size -= read;
    }
    return true;
}

// Scales and fits the picture into width x height, then makes it RGBA bytes.
[[nodiscard]] std::wstring fitFilter(uint32_t width, uint32_t height, ScreenFit fit) {
    switch (fit) {
    case ScreenFit::Fill:
        return std::format(
            L"scale={0}:{1}:force_original_aspect_ratio=increase:flags=bicubic,crop={0}:{1}",
            width,
            height
        );
    case ScreenFit::Fit:
        return std::format(
            L"scale={0}:{1}:force_original_aspect_ratio=decrease:flags=bicubic,pad={0}:{1}:(ow-iw)/2:(oh-ih)/2:color=black",
            width,
            height
        );
    case ScreenFit::Stretch:
        break;
    }
    return std::format(L"scale={}:{}:flags=bicubic", width, height);
}

// Lays transparent pixels over black.
void flattenOverBlack(std::vector<uint8_t>& rgba) {
    for (size_t index = 0; index + 3 < rgba.size(); index += 4) {
        uint32_t const alpha = rgba[index + 3];
        if (alpha == 255) continue;
        for (size_t channel = 0; channel < 3; ++channel) {
            rgba[index + channel] = static_cast<uint8_t>((rgba[index + channel] * alpha + 127) / 255);
        }
        rgba[index + 3] = 255;
    }
}

[[nodiscard]] ComPtr<IWICImagingFactory> imagingFactory() {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }
    return factory;
}

[[nodiscard]] std::shared_ptr<MediaFrame const>
loadStillWithWic(std::filesystem::path const& file, uint32_t width, uint32_t height, ScreenFit fit) {
    ComScope                      com;
    auto const                    factory = imagingFactory();
    ComPtr<IWICBitmapDecoder>     decoder;
    ComPtr<IWICBitmapFrameDecode> source;
    ComPtr<IWICFormatConverter>   converter;
    ComPtr<IWICBitmapScaler>      scaler;
    // Premultiplied, so scaling does not bleed the colour of transparent pixels; it is the picture over black.
    if (!factory
        || FAILED(factory->CreateDecoderFromFilename(
            file.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            &decoder
        ))
        || FAILED(decoder->GetFrame(0, &source)) || FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(
            source.Get(),
            GUID_WICPixelFormat32bppPRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        ))) {
        return nullptr;
    }
    UINT imageWidth{};
    UINT imageHeight{};
    if (FAILED(converter->GetSize(&imageWidth, &imageHeight)) || imageWidth == 0 || imageHeight == 0
        || imageWidth > MaxImageDimension || imageHeight > MaxImageDimension) {
        return nullptr;
    }

    double const widthScale  = static_cast<double>(width) / imageWidth;
    double const heightScale = static_cast<double>(height) / imageHeight;
    uint32_t     scaledWidth = width;
    uint32_t     scaledHeight = height;
    if (fit == ScreenFit::Fill) {
        double const scale = std::max(widthScale, heightScale);
        scaledWidth        = std::max(width, static_cast<uint32_t>(std::ceil(imageWidth * scale - 1e-6)));
        scaledHeight       = std::max(height, static_cast<uint32_t>(std::ceil(imageHeight * scale - 1e-6)));
    } else if (fit == ScreenFit::Fit) {
        double const scale = std::min(widthScale, heightScale);
        scaledWidth        = std::clamp(static_cast<uint32_t>(std::lround(imageWidth * scale)), 1u, width);
        scaledHeight       = std::clamp(static_cast<uint32_t>(std::lround(imageHeight * scale)), 1u, height);
    }
    if (FAILED(factory->CreateBitmapScaler(&scaler))
        || FAILED(scaler->Initialize(converter.Get(), scaledWidth, scaledHeight, WICBitmapInterpolationModeFant))) {
        return nullptr;
    }

    auto frame    = std::make_shared<MediaFrame>();
    frame->width  = width;
    frame->height = height;
    frame->rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    UINT const stride = width * 4;
    HRESULT    copied{};
    if (fit == ScreenFit::Fit) {
        // Centred on black.
        size_t const offset = (static_cast<size_t>((height - scaledHeight) / 2) * width + (width - scaledWidth) / 2) * 4;
        copied              = scaler->CopyPixels(
            nullptr,
            stride,
            static_cast<UINT>(frame->rgba.size() - offset),
            frame->rgba.data() + offset
        );
    } else {
        // Fill crops the overhang evenly; Stretch has none.
        WICRect const crop{
            static_cast<INT>((scaledWidth - width) / 2),
            static_cast<INT>((scaledHeight - height) / 2),
            static_cast<INT>(width),
            static_cast<INT>(height),
        };
        copied = scaler->CopyPixels(&crop, stride, static_cast<UINT>(frame->rgba.size()), frame->rgba.data());
    }
    if (FAILED(copied)) return nullptr;
    for (size_t index = 3; index < frame->rgba.size(); index += 4) frame->rgba[index] = 255;
    return frame;
}

[[nodiscard]] std::shared_ptr<MediaFrame const>
loadStillWithFfmpeg(std::filesystem::path const& file, uint32_t width, uint32_t height, ScreenFit fit) {
    auto const arguments = std::format(
        L"-hide_banner -loglevel error -nostdin -i {} -an -sn -dn -frames:v 1 -vf {},format=rgba -f rawvideo "
        L"-pix_fmt rgba pipe:1",
        quoteArgument(file.wstring()),
        fitFilter(width, height, fit)
    );
    auto child = launchFfmpeg(arguments, false);
    if (!child) return nullptr;
    auto frame    = std::make_shared<MediaFrame>();
    frame->width  = width;
    frame->height = height;
    frame->rgba.resize(static_cast<size_t>(width) * height * 4);
    bool const read = readExact(child->output.get(), frame->rgba.data(), frame->rgba.size());
    TerminateProcess(child->process.get(), 0);
    if (!read) return nullptr;
    flattenOverBlack(frame->rgba);
    return frame;
}

// ffmpeg -i reports the file on stderr and stops for want of an output.
[[nodiscard]] std::optional<std::string> ffmpegReport(std::filesystem::path const& file) {
    auto child = launchFfmpeg(L"-hide_banner -nostdin -i " + quoteArgument(file.wstring()), true);
    if (!child) return std::nullopt;
    std::string report;
    auto const  deadline = std::chrono::steady_clock::now() + ProbeTimeout;
    char        buffer[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available{};
        if (!PeekNamedPipe(child->output.get(), nullptr, 0, nullptr, &available, nullptr)) break;
        if (available == 0) {
            if (WaitForSingleObject(child->process.get(), 5) == WAIT_OBJECT_0) {
                // Exited: what is left in the pipe is all there is.
                if (!PeekNamedPipe(child->output.get(), nullptr, 0, nullptr, &available, nullptr) || available == 0) {
                    return report;
                }
            }
            continue;
        }
        DWORD read{};
        if (!ReadFile(child->output.get(), buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr)) break;
        report.append(buffer, read);
    }
    TerminateProcess(child->process.get(), 0);
    return report.empty() ? std::nullopt : std::optional{report};
}

[[nodiscard]] std::optional<MediaInfo> parseReport(std::string const& report) {
    auto const videoLine = [&]() -> std::string {
        for (size_t start = 0; start < report.size();) {
            size_t const end  = std::min(report.find('\n', start), report.size());
            auto const   line = report.substr(start, end - start);
            if (line.find("Stream #") != std::string::npos && line.find("Video:") != std::string::npos) return line;
            start = end + 1;
        }
        return {};
    }();
    if (videoLine.empty()) return std::nullopt;

    MediaInfo   info;
    std::smatch match;
    static std::regex const durationPattern{R"(Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?))"};
    if (std::regex_search(report, match, durationPattern)) {
        info.duration = std::stod(match[1].str()) * 3600.0 + std::stod(match[2].str()) * 60.0 + std::stod(match[3].str());
    }
    static std::regex const sizePattern{R"(\b(\d{1,5})x(\d{1,5})\b)"};
    if (std::regex_search(videoLine, match, sizePattern)) {
        info.width  = static_cast<uint32_t>(std::stoul(match[1].str()));
        info.height = static_cast<uint32_t>(std::stoul(match[2].str()));
    }
    static std::regex const fpsPattern{R"(([\d.]+)\s*fps)"};
    static std::regex const tbrPattern{R"(([\d.]+)(k?)\s*tbr)"};
    if (std::regex_search(videoLine, match, fpsPattern)) {
        info.fps = std::stod(match[1].str());
    } else if (std::regex_search(videoLine, match, tbrPattern)) {
        info.fps = std::stod(match[1].str()) * (match[2].matched && match[2].length() > 0 ? 1000.0 : 1.0);
    }
    if (!std::isfinite(info.fps) || info.fps <= 0.0) info.fps = 30.0;
    info.fps   = std::min(info.fps, MaxFrameRate);
    info.still = !(info.duration > 0.0) || !std::isfinite(info.duration);
    return info;
}

} // namespace

bool isStillImageFile(std::filesystem::path const& file) {
    auto extension = file.extension().wstring();
    std::ranges::transform(extension, extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return extension == L".png" || extension == L".jpg" || extension == L".jpeg" || extension == L".bmp"
        || extension == L".tif" || extension == L".tiff" || extension == L".webp" || extension == L".jxr"
        || extension == L".heic" || extension == L".ico";
}

std::filesystem::path mediaPath(std::string const& text) {
    auto const* begin = reinterpret_cast<char8_t const*>(text.data());
    return std::filesystem::path(std::u8string(begin, begin + text.size()));
}

std::optional<MediaInfo> probeMedia(std::filesystem::path const& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return std::nullopt;
    auto const report = ffmpegReport(file);
    if (!report) return std::nullopt;
    try {
        return parseReport(*report);
    } catch (std::exception const&) {
        return std::nullopt;
    }
}

std::shared_ptr<MediaFrame const>
loadStillFrame(std::filesystem::path const& file, uint32_t width, uint32_t height, ScreenFit fit) {
    if (width == 0 || height == 0) return nullptr;
    if (auto frame = loadStillWithWic(file, width, height, fit)) return frame;
    return loadStillWithFfmpeg(file, width, height, fit);
}

bool decodeImageBytes(std::span<uint8_t const> bytes, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba) {
    if (bytes.empty() || bytes.size() > std::numeric_limits<DWORD>::max()) return false;
    ComScope                      com;
    auto const                    factory = imagingFactory();
    ComPtr<IWICStream>            stream;
    ComPtr<IWICBitmapDecoder>     decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter>   converter;
    if (!factory || FAILED(factory->CreateStream(&stream))
        || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())))
        || FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))
        || FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom
        ))) {
        return false;
    }
    UINT w{};
    UINT h{};
    if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0 || w > MaxImageDimension || h > MaxImageDimension) {
        return false;
    }
    rgba.resize(static_cast<size_t>(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()), rgba.data()))) return false;
    width  = w;
    height = h;
    return true;
}

struct VideoStream::Impl {
    std::filesystem::path file;
    uint32_t              width{};
    uint32_t              height{};
    ScreenFit             fit{};
    double                fps{};
    int64_t               lastIndex{};

    std::mutex              mutex;
    std::condition_variable changed;
    bool                    stopping{};
    // A new decoder is wanted from restartIndex; generation tells the worker its pipe is stale.
    bool                    restart{};
    int64_t                 restartIndex{};
    uint64_t                generation{};
    // This generation's first frame and the newest decoded; ended once its decoder ran out.
    int64_t                 startIndex{std::numeric_limits<int64_t>::max()};
    int64_t                 frontier{-1};
    bool                    ended{};
    HANDLE                  process{};
    std::deque<std::shared_ptr<MediaFrame const>> queue;
    // The newest frame handed out from this generation; the last one before a restart stands in until it has one.
    std::shared_ptr<MediaFrame const>             current;
    std::shared_ptr<MediaFrame const>             stale;
    bool                                          failureLogged{};
    std::thread                                   worker;

    [[nodiscard]] std::optional<ChildProcess> launch(int64_t index) const {
        double const seconds   = static_cast<double>(index) / fps;
        auto const   arguments = std::format(
            L"-hide_banner -loglevel error -nostdin -threads 2 -ss {:.6f} -i {} -an -sn -dn -vf {},fps={:.6f},format=rgba "
              L"-f rawvideo -pix_fmt rgba pipe:1",
            seconds,
            quoteArgument(file.wstring()),
            fitFilter(width, height, fit),
            fps
        );
        return launchFfmpeg(arguments, false);
    }

    // Under the lock: the worker drops what it is decoding and starts again at `index`.
    void requestRestart(int64_t index) {
        ++generation;
        restart      = true;
        restartIndex = index;
        startIndex   = index;
        frontier     = index - 1;
        ended        = false;
        queue.clear();
        if (current) stale = std::move(current);
        current.reset();
        if (process) TerminateProcess(process, 1);
        changed.notify_all();
    }

    void run() {
        ChildProcess child;
        uint64_t     childGeneration{};
        int64_t      next{};
        size_t const frameBytes = static_cast<size_t>(width) * height * 4;
        auto const   closeChild = [&] {
            if (child.process) TerminateProcess(child.process.get(), 1);
            child   = {};
            process = nullptr;
        };

        std::unique_lock lock(mutex);
        while (true) {
            changed.wait(lock, [&] {
                return stopping || restart || (child.output && !ended && queue.size() < FramesAhead);
            });
            if (stopping) break;
            if (restart) {
                restart = false;
                closeChild();
                childGeneration     = generation;
                next                = restartIndex;
                int64_t const index = restartIndex;
                lock.unlock();
                auto launched = launch(index);
                lock.lock();
                if (!launched) {
                    ended = true;
                    if (!std::exchange(failureLogged, true)) {
                        getLogger().warn("Unable to start FFmpeg to decode the Screen video {}", utf8(file));
                    }
                    changed.notify_all();
                    continue;
                }
                child   = std::move(*launched);
                process = child.process.get();
                continue;
            }

            auto frame    = std::make_shared<MediaFrame>();
            frame->width  = width;
            frame->height = height;
            frame->rgba.resize(frameBytes);
            HANDLE const pipe = child.output.get();
            lock.unlock();
            bool const read = readExact(pipe, frame->rgba.data(), frameBytes);
            if (read) flattenOverBlack(frame->rgba);
            lock.lock();
            // Restarted meanwhile: the requester already killed this decoder.
            if (childGeneration != generation) continue;
            if (!read) {
                ended = true;
                closeChild();
                changed.notify_all();
                continue;
            }
            frame->index = next++;
            frontier     = frame->index;
            queue.push_back(std::move(frame));
            changed.notify_all();
        }
        closeChild();
    }
};

VideoStream::VideoStream(
    std::filesystem::path file,
    MediaInfo const&      info,
    uint32_t              width,
    uint32_t              height,
    ScreenFit             fit
)
: mImpl(std::make_unique<Impl>()) {
    mImpl->file      = std::move(file);
    mImpl->width     = width;
    mImpl->height    = height;
    mImpl->fit       = fit;
    mImpl->fps       = info.fps > 0.0 ? info.fps : 30.0;
    mImpl->lastIndex = std::max<int64_t>(0, static_cast<int64_t>(std::ceil(info.duration * mImpl->fps)) - 1);
    mImpl->worker    = std::thread([impl = mImpl.get()] { impl->run(); });
}

VideoStream::~VideoStream() {
    {
        std::lock_guard lock(mImpl->mutex);
        mImpl->stopping = true;
        if (mImpl->process) TerminateProcess(mImpl->process, 1);
        mImpl->changed.notify_all();
    }
    if (mImpl->worker.joinable()) mImpl->worker.join();
}

std::shared_ptr<MediaFrame const> VideoStream::frameAt(double seconds, bool exact) {
    auto& impl = *mImpl;
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    int64_t const wanted = std::min(impl.lastIndex, static_cast<int64_t>(std::floor(seconds * impl.fps + 1e-6)));

    std::unique_lock lock(impl.mutex);
    auto const take = [&] {
        while (!impl.queue.empty() && impl.queue.front()->index <= wanted) {
            impl.current = std::move(impl.queue.front());
            impl.stale.reset();
            impl.queue.pop_front();
        }
    };
    take();
    auto const skipAhead = std::max<int64_t>(30, static_cast<int64_t>(impl.fps * SkipAheadSeconds));
    bool const before    = wanted < impl.startIndex
                     || (impl.current && impl.current->index > wanted && impl.current->index >= impl.startIndex);
    bool const farAhead = !impl.ended && wanted > impl.frontier + skipAhead;
    if (before || farAhead) {
        impl.requestRestart(wanted);
    } else {
        impl.changed.notify_all();
    }
    if (exact) {
        // Past the decoder's end the last frame it gave stands for the rest.
        impl.changed.wait_for(lock, ExactFrameTimeout, [&] {
            take();
            impl.changed.notify_all();
            return impl.stopping || (impl.current && impl.current->index == wanted)
                || (impl.ended && impl.queue.empty());
        });
        if (!(impl.current && impl.current->index == wanted) && !impl.ended) {
            getLogger().debug("Screen video frame {} of {} was not decoded in time", wanted, utf8(impl.file));
        }
    }
    return impl.current ? impl.current : impl.stale;
}

MediaProbeCache::Result MediaProbeCache::find(std::string const& file) {
    auto const future = probe(file);
    if (future.wait_for(std::chrono::seconds{0}) != std::future_status::ready) return {};
    return {true, future.get()};
}

std::optional<MediaInfo> MediaProbeCache::get(std::string const& file) { return probe(file).get(); }

void MediaProbeCache::clear() {
    std::unordered_map<std::string, std::shared_future<std::optional<MediaInfo>>> probes;
    {
        std::lock_guard lock(mMutex);
        probes.swap(mProbes);
    }
    // Leaving the scope waits for the probes still running.
}

std::shared_future<std::optional<MediaInfo>> MediaProbeCache::probe(std::string const& file) {
    std::lock_guard lock(mMutex);
    auto            found = mProbes.find(file);
    if (found == mProbes.end()) {
        found = mProbes.emplace(file, std::async(std::launch::async, [file] { return probeMedia(mediaPath(file)); }).share())
                    .first;
    }
    return found->second;
}

} // namespace playback::replay
