#include "outputvideo.hpp"

#include "../log.hpp"
#include "outputsinktable.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Pelican {
struct OutputVideoFrame::Data {
    std::vector<std::uint8_t> pixels;
    bool ready = false;
    bool cancelled = false;
};
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t video_queue_limit = 4;

std::chrono::milliseconds encoderTimeout() {
    if (const char *hook = std::getenv("PELICAN_VIDEO_ENCODER_TIMEOUT_MS")) {
        const auto value = std::strtoull(hook, nullptr, 10);
        if (value > 0) return std::chrono::milliseconds(value);
    }
    return std::chrono::seconds(30);
}
std::uint64_t runIdentifier() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp = Clock::now().time_since_epoch().count();
    return static_cast<std::uint64_t>(stamp) ^ (++sequence * 0x9e3779b97f4a7c15ull);
}
std::string encoderFailure(const OutputVideoEncoderProcess &process,
                           const std::optional<int> &exit,
                           const std::string &reason) {
    auto message = "output.video_encoder_failed@1: " + reason;
    message += "; exit_code=" + (exit ? std::to_string(*exit) : std::string{"unknown"});
    const auto tail = process.stderrTail();
    if (!tail.empty()) message += "; stderr=" + tail;
    return message;
}
} // namespace

struct OutputVideoWriter::Impl {
    const std::string name;
    const std::filesystem::path directory;
    const std::optional<std::filesystem::path> encoder;
    const std::uint64_t fps_num;
    const std::uint64_t fps_den;
    const bool live;
    const bool offline_rate_valid;
    const std::uint32_t max_frames;
    const std::uint64_t max_bytes;
    const std::filesystem::path final_path;
    const std::filesystem::path partial_path;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<OutputVideoFrame>> queue;
    OutputVideoStatus current;
    std::unique_ptr<OutputVideoEncoderProcess> process =
        std::make_unique<OutputVideoEncoderProcess>();
    std::thread writer;
    std::thread supervisor;
    bool stopping = false;
    bool aborted = false;
    bool done = false;
    bool started = false;
    bool all_frames_sent = false;
    bool time_jump = false;
    std::int64_t stop_nanoseconds = 0;
    std::optional<std::int64_t> first_nanoseconds;
    std::optional<std::uint64_t> initial_time_revision;
    std::uint64_t next_index = 0;
    std::vector<std::uint8_t> previous;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> extent;
    bool first_bgra = false;
    Clock::time_point last_size_check{};
    std::uint64_t observed_size = 0;
    std::optional<std::string> deferred_refusal;

    Impl(std::string sink_name, std::filesystem::path output_directory,
         std::optional<std::filesystem::path> path, std::uint64_t num,
         std::uint64_t den, bool is_live, bool rate_valid,
         std::uint32_t frame_limit, std::uint64_t byte_limit)
        : name(std::move(sink_name)), directory(std::move(output_directory)),
          encoder(std::move(path)), fps_num(num), fps_den(den), live(is_live),
          offline_rate_valid(rate_valid), max_frames(frame_limit),
          max_bytes(byte_limit), final_path(directory / (name + ".mp4")),
          partial_path(directory / (name + "." + std::to_string(runIdentifier()) +
                                    ".mp4.partial")) {
        writer = std::thread([this] { run(); });
        supervisor = std::thread([this] { supervise(); });
    }

    void supervise() {
        std::unique_lock lock{mutex};
        while (!done) {
            if (aborted) {
                lock.unlock();
                process->terminate();
                lock.lock();
                cv.wait(lock, [&] { return done; });
                break;
            }
            if (!stopping) {
                if (started && current.encoder_state == "running") {
                    lock.unlock();
                    const auto exit = process->wait(std::chrono::milliseconds{0});
                    lock.lock();
                    if (exit) {
                        aborted = true;
                        current.state = "refused";
                        current.encoder_state = "failed";
                        current.last_error = encoderFailure(*process, exit,
                            "encoder exited before recording stopped");
                        cv.notify_all();
                        continue;
                    }
                }
                const auto now = Clock::now();
                if (max_bytes != 0 && current.frames_written != 0 &&
                    (last_size_check == Clock::time_point{} ||
                     now - last_size_check >= std::chrono::seconds(1))) {
                    lock.unlock();
                    std::error_code error;
                    const auto size = std::filesystem::file_size(partial_path, error);
                    lock.lock();
                    last_size_check = now;
                    if (!error) observed_size = size;
                    if (!stopping && !aborted && !error && size > max_bytes) {
                        stopping = true;
                        stop_nanoseconds = std::chrono::duration_cast<
                            std::chrono::nanoseconds>(now.time_since_epoch()).count();
                        current.stop_reason = "max_bytes";
                        current.stop_value = max_bytes;
                        current.last_error = "output.limit_reached@1: max_bytes " +
                                             std::to_string(max_bytes) + " reached";
                        cv.notify_all();
                    }
                }
                cv.wait_for(lock, started || max_bytes != 0
                                        ? std::chrono::milliseconds{50}
                                        : std::chrono::hours{24 * 365},
                            [&] { return stopping || aborted || done; });
                continue;
            }
            const auto remaining = std::chrono::milliseconds{encoderTimeout()};
            const auto deadline = Clock::now() + remaining;
            if (!cv.wait_until(lock, deadline, [&] { return done || aborted; })) {
                current.state = "refused";
                current.last_error = "output.video_encoder_failed@1: encoder exit timeout";
                current.encoder_state = "failed";
                lock.unlock();
                process->terminate();
                lock.lock();
                cv.wait(lock, [&] { return done; });
            }
        }
    }

    std::shared_ptr<OutputVideoFrame> reserve(OutputVideoFrame frame, bool wait) {
        std::unique_lock lock{mutex};
        if (live && !first_nanoseconds) first_nanoseconds = frame.monotonic_nanoseconds;
        if (!initial_time_revision) initial_time_revision = frame.time_revision;
        if (!frame.playing || frame.time_revision != *initial_time_revision) {
            if (!live) {
                aborted = true;
                time_jump = true;
                current.state = "refused";
                current.last_error = "output.video_time_jump@1";
                cv.notify_all();
                lock.unlock();
                process->terminate();
                return nullptr;
            }
        }
        cv.wait(lock, [&] {
            return queue.size() < video_queue_limit || stopping || aborted || done ||
                   !wait;
        });
        if (stopping || aborted || done || current.state != "running") return nullptr;
        if (queue.size() == video_queue_limit) {
            ++current.frames_dropped;
            return nullptr;
        }
        auto result = std::make_shared<OutputVideoFrame>(std::move(frame));
        result->data = std::make_shared<OutputVideoFrame::Data>();
        queue.push_back(result);
        current.queue_depth = queue.size();
        cv.notify_all();
        return result;
    }

    void cancel(const std::shared_ptr<OutputVideoFrame> &frame) {
        if (!frame || !frame->data) return;
        {
            std::lock_guard lock{mutex};
            if (!frame->data->cancelled) {
                frame->data->cancelled = true;
                ++current.frames_discarded;
            }
            frame->data->ready = true;
        }
        cv.notify_all();
    }

    void complete(const std::shared_ptr<OutputVideoFrame> &result,
                  std::vector<std::uint8_t> pixels) {
        if (!result || !result->data) return;
        {
            std::lock_guard lock{mutex};
            result->data->pixels = std::move(pixels);
            result->data->ready = true;
        }
        cv.notify_all();
    }

    bool startEncoder(const OutputVideoFrame &frame) {
        if (started) return true;
        started = true;
        const auto fail = [this](std::string error) {
            std::lock_guard lock{mutex};
            current.state = "refused";
            current.last_error = std::move(error);
            current.encoder_state = "failed";
            return false;
        };
        if (!live && !offline_rate_valid)
            return fail("output.video_rate_mismatch@1");
        const auto resolved_encoder = findOutputVideoEncoder(encoder);
        if (!resolved_encoder) return fail("output.ffmpeg_missing@1");
        if (logger) LOG_INFO(logger, "Output video {} found ffmpeg {}", name,
                             resolved_encoder->string());
        const auto version = outputVideoEncoderVersion(*resolved_encoder,
                                                       std::chrono::seconds(5));
        if (!version) return fail("output.video_encoder_failed@1: version probe failed");
        if (logger) LOG_INFO(logger, "Output video {} encoder version: {}", name, *version);
        const auto pixels = static_cast<std::uint64_t>(frame.width) * frame.height;
        if (frame.width == 0 || frame.height == 0 || frame.width > 16384 ||
            frame.height > 16384 || pixels > 268435456 ||
            pixels > (std::numeric_limits<std::size_t>::max)() / 4 ||
            (frame.width % 2) != 0 || (frame.height % 2) != 0)
            return fail("output.video_dimensions_invalid@1");
        OutputVideoEncoderOptions options;
        options.executable = std::filesystem::absolute(*resolved_encoder);
        options.partial_path = std::filesystem::absolute(partial_path);
        options.width = frame.width;
        options.height = frame.height;
        options.fps_num = fps_num;
        options.fps_den = fps_den;
        // Input is tightly packed, top-down display-encoded SDR. Linear data
        // is not transformed to display encoding before the BT.709 conversion.
        options.bgra = frame.bgra;
        std::string error;
        if (!process->start(options, error))
            return fail("output.video_encoder_failed@1: " + error);
        const auto pid = process->pid();
        {
            std::lock_guard lock{mutex};
            current.encoder_pid = pid;
            current.encoder_state = "running";
        }
        if (logger) LOG_INFO(logger, "Output video {} started ffmpeg {} (pid {})", name,
                             options.executable.string(), pid);
        return true;
    }

    enum class WriteResult { written, limit, failed };
    WriteResult writeOne(const std::vector<std::uint8_t> &pixels, bool repeat) {
        {
            std::lock_guard lock{mutex};
            if (current.stop_reason == "max_bytes") return WriteResult::limit;
            if (max_frames != 0 && current.frames_written >= max_frames) {
                current.stop_reason = "max_frames";
                current.stop_value = max_frames;
                current.last_error = "output.limit_reached@1: max_frames " +
                                     std::to_string(max_frames) + " reached";
                return WriteResult::limit;
            }
        }
#ifndef _WIN32
        sigset_t blocked;
        sigset_t previous_mask;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPIPE);
        (void)pthread_sigmask(SIG_BLOCK, &blocked, &previous_mask);
#endif
        std::string reason;
        const auto ok = process->write(pixels.data(), pixels.size(), reason);
#ifndef _WIN32
        if (!ok) {
            timespec zero{};
            (void)sigtimedwait(&blocked, nullptr, &zero);
        }
        (void)pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
#endif
        if (!ok) {
            const auto exit = process->wait(std::chrono::milliseconds(50));
            {
                std::lock_guard lock{mutex};
                current.last_error = encoderFailure(*process, exit, reason);
                current.state = "refused";
                current.encoder_state = "failed";
                aborted = true;
            }
            process->terminate();
            return WriteResult::failed;
        }
        std::lock_guard lock{mutex};
        ++current.frames_written;
        if (repeat) ++current.frames_repeated;
        if (max_frames != 0 && current.frames_written >= max_frames) {
            current.stop_reason = "max_frames";
            current.stop_value = max_frames;
            current.last_error = "output.limit_reached@1: max_frames " +
                                 std::to_string(max_frames) + " reached";
            return WriteResult::limit;
        }
        const auto now = Clock::now();
        if (max_bytes != 0 && (last_size_check == Clock::time_point{} ||
                               now - last_size_check >= std::chrono::seconds(1))) {
            std::error_code error;
            observed_size = std::filesystem::file_size(partial_path, error);
            if (!error) last_size_check = now;
            if (!error && observed_size > max_bytes) {
                current.stop_reason = "max_bytes";
                current.stop_value = max_bytes;
                current.last_error = "output.limit_reached@1: max_bytes " +
                                     std::to_string(max_bytes) + " reached";
                return WriteResult::limit;
            }
        }
        return WriteResult::written;
    }

    void discardQueue() {
        for (auto &entry : queue) {
            if (!entry->data->cancelled) {
                entry->data->cancelled = true;
                ++current.frames_discarded;
            }
            entry->data->ready = true;
        }
        current.queue_depth = queue.size();
    }

    bool publish() {
        process->closeInput();
        auto exit = process->wait(encoderTimeout());
        if (!exit) {
            process->terminate();
            exit = process->wait(std::chrono::seconds(5));
        }
        std::lock_guard lock{mutex};
        current.encoder_pid = 0;
        current.encoder_state = "exited";
        if (!exit || *exit != 0 || !all_frames_sent || aborted) {
            current.state = "refused";
            if (current.last_error.empty())
                current.last_error = encoderFailure(*process, exit, "encoder did not finish");
            current.encoder_state = "failed";
            return false;
        }
        std::error_code error;
        current.final_bytes = std::filesystem::file_size(partial_path, error);
        if (error || current.final_bytes == 0) {
            current.state = "refused";
            current.last_error = "output.video_encoder_failed@1: empty or missing output";
            current.encoder_state = "failed";
            return false;
        }
#ifdef _WIN32
        if (!MoveFileExW(partial_path.c_str(), final_path.c_str(), 0)) {
            current.state = "refused";
            current.last_error = GetLastError() == ERROR_ALREADY_EXISTS ||
                                         GetLastError() == ERROR_FILE_EXISTS
                                     ? "output.exists@1: " + final_path.string()
                                     : "output.write_failed@1: cannot publish video";
            return false;
        }
#else
        if (::link(partial_path.c_str(), final_path.c_str()) != 0) {
            const auto reason = errno;
            current.state = "refused";
            current.last_error = reason == EEXIST
                ? "output.exists@1: " + final_path.string()
                : "output.write_failed@1: cannot publish video";
            return false;
        }
        (void)::unlink(partial_path.c_str());
#endif
        current.final_bytes = std::filesystem::file_size(final_path, error);
        return true;
    }

    void finish(bool normal) {
        {
            std::lock_guard lock{mutex};
            if (normal && started && !aborted) all_frames_sent = true;
        }
        if (all_frames_sent) (void)publish();
        std::lock_guard lock{mutex};
        if (deferred_refusal) {
            current.state = "refused";
            current.last_error = *deferred_refusal;
        } else if (normal && current.state == "running") {
            current.state = "finished";
            current.stop_reason = current.stop_reason == "none" ? "stop" : current.stop_reason;
        }
        if (!normal && current.state == "running") {
            current.state = "refused";
            if (current.last_error.empty())
                current.last_error = time_jump ? "output.video_time_jump@1"
                                               : "output.video_encoder_failed@1";
        }
        current.queue_depth = 0;
        current.encoder_pid = 0;
        if (current.encoder_state == "running") current.encoder_state = "exited";
        done = true;
        cv.notify_all();
    }

    void run() {
        bool normal = false;
        while (true) {
            std::shared_ptr<OutputVideoFrame> frame;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, [&] {
                    return aborted || (!queue.empty() && queue.front()->data->ready) ||
                           (stopping && queue.empty());
                });
                if (aborted) {
                    discardQueue();
                    queue.clear();
                    break;
                }
                if (queue.empty()) {
                    if (live && !previous.empty() && first_nanoseconds) {
                        stop_nanoseconds = std::max(stop_nanoseconds, *first_nanoseconds);
                    }
                    normal = true;
                    break;
                }
                frame = queue.front();
                queue.pop_front();
                current.queue_depth = queue.size();
            }
            if (frame->data->cancelled) {
                if (!live) {
                    std::lock_guard lock{mutex};
                    aborted = true;
                    current.state = "refused";
                    current.last_error =
                        "output.video_encoder_failed@1: captured frame was discarded";
                    current.encoder_state = "failed";
                    discardQueue();
                    queue.clear();
                    process->terminate();
                    break;
                }
                continue;
            }
            if (!startEncoder(*frame)) {
                std::lock_guard lock{mutex};
                aborted = true;
                discardQueue();
                queue.clear();
                break;
            }
            if (extent && (*extent != std::pair{frame->width, frame->height} ||
                           first_bgra != frame->bgra)) {
                std::lock_guard lock{mutex};
                deferred_refusal = "output.video_size_changed@1";
                ++current.frames_discarded;
                discardQueue();
                queue.clear();
                normal = true;
                break;
            }
            if (!extent) {
                extent = std::pair{frame->width, frame->height};
                first_bgra = frame->bgra;
            }
            const auto &pixels = frame->data->pixels;
            if (!live) {
                const auto result = writeOne(pixels, false);
                if (result == WriteResult::failed) break;
                if (result == WriteResult::limit) {
                    std::lock_guard lock{mutex};
                    discardQueue();
                    queue.clear();
                    normal = true;
                    break;
                }
                continue;
            }
            const auto elapsed = std::max<std::int64_t>(0,
                frame->monotonic_nanoseconds - first_nanoseconds.value_or(
                    frame->monotonic_nanoseconds));
            const auto position = outputVideoPosition(elapsed, fps_num, fps_den);
            if (position < next_index) {
                std::lock_guard lock{mutex};
                ++current.frames_dropped;
                continue;
            }
            const auto repeats = position - next_index;
            const auto catch_up_limit = (2 * fps_num + fps_den - 1) / fps_den;
            if (repeats > catch_up_limit || (repeats != 0 && previous.empty())) {
                std::lock_guard lock{mutex};
                deferred_refusal = "output.video_encoder_too_slow@1";
                ++current.frames_discarded;
                discardQueue();
                queue.clear();
                normal = true;
                break;
            }
            bool failed = false;
            bool limited = false;
            for (std::uint64_t index = 0; index < repeats; ++index) {
                const auto result = writeOne(previous, true);
                if (result == WriteResult::failed) { failed = true; break; }
                if (result == WriteResult::limit) { limited = true; break; }
            }
            if (failed) break;
            if (!limited) {
                const auto result = writeOne(pixels, false);
                failed = result == WriteResult::failed;
                limited = result == WriteResult::limit;
            }
            if (failed) break;
            previous = pixels;
            next_index = position + 1;
            if (limited) {
                std::lock_guard lock{mutex};
                discardQueue();
                queue.clear();
                normal = true;
                break;
            }
        }
        if (normal && live && !previous.empty() && stop_nanoseconds != 0) {
            const auto stop_position = outputVideoPosition(
                std::max<std::int64_t>(0, stop_nanoseconds - *first_nanoseconds),
                fps_num, fps_den);
            while (next_index < stop_position) {
                const auto result = writeOne(previous, true);
                if (result == WriteResult::failed) { normal = false; break; }
                ++next_index;
                if (result == WriteResult::limit) break;
            }
        }
        finish(normal);
    }

    void requestStop(std::int64_t monotonic_nanoseconds) {
        {
            std::lock_guard lock{mutex};
            if (stopping || done) return;
            stopping = true;
            stop_nanoseconds = monotonic_nanoseconds >= 0 ? monotonic_nanoseconds :
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now().time_since_epoch()).count();
        }
        cv.notify_all();
    }

    void abort(std::string error) {
        {
            std::lock_guard lock{mutex};
            if (done) return;
            aborted = true;
            current.state = "refused";
            current.last_error = std::move(error);
            current.encoder_state = "failed";
        }
        cv.notify_all();
        process->terminate();
    }

    ~Impl() {
        requestStop(-1);
        cv.notify_all();
        if (supervisor.joinable()) supervisor.join();
        if (writer.joinable()) writer.join();
    }
};

OutputVideoWriter::OutputVideoWriter(
    std::string name, std::filesystem::path directory,
    std::optional<std::filesystem::path> encoder, std::uint64_t fps_num,
    std::uint64_t fps_den, bool live, bool offline_rate_valid,
    std::uint32_t max_frames, std::uint64_t max_bytes)
    : impl_(std::make_unique<Impl>(std::move(name), std::move(directory),
          std::move(encoder), fps_num, fps_den, live, offline_rate_valid,
          max_frames, max_bytes)) {}
OutputVideoWriter::~OutputVideoWriter() = default;

std::shared_ptr<OutputVideoFrame> OutputVideoWriter::reserve(
    OutputVideoFrame metadata, bool wait) {
    return impl_->reserve(std::move(metadata), wait);
}
void OutputVideoWriter::complete(const std::shared_ptr<OutputVideoFrame> &frame,
                                 std::vector<std::uint8_t> pixels) {
    impl_->complete(frame, std::move(pixels));
}
void OutputVideoWriter::cancel(const std::shared_ptr<OutputVideoFrame> &frame) {
    impl_->cancel(frame);
}
void OutputVideoWriter::requestStop(std::int64_t monotonic_nanoseconds) {
    impl_->requestStop(monotonic_nanoseconds);
}
void OutputVideoWriter::abort(std::string error) { impl_->abort(std::move(error)); }
bool OutputVideoWriter::finished() const {
    std::lock_guard lock{impl_->mutex};
    return impl_->done;
}
OutputVideoStatus OutputVideoWriter::status() const {
    std::lock_guard lock{impl_->mutex};
    return impl_->current;
}

} // namespace Pelican
