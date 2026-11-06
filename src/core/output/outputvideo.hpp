#pragma once

#include "outputvideoprocess.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

struct OutputVideoFrame {
    std::uint64_t capture = 0;
    std::int64_t monotonic_nanoseconds = 0;
    std::uint64_t time_revision = 0;
    bool playing = true;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool bgra = false;

    struct Data;
    std::shared_ptr<Data> data;
};

struct OutputVideoStatus {
    std::string state = "running";
    std::string stop_reason = "none";
    std::uint64_t stop_value = 0;
    std::string last_error;
    std::uint64_t frames_written = 0;
    std::uint64_t frames_repeated = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t frames_discarded = 0;
    std::uint64_t queue_depth = 0;
    std::uint64_t encoder_pid = 0;
    std::string encoder_state = "not_started";
    std::uint64_t final_bytes = 0;
};

class OutputVideoWriter {
  public:
    OutputVideoWriter(std::string name, std::filesystem::path directory,
                      std::optional<std::filesystem::path> encoder,
                      std::uint64_t fps_num, std::uint64_t fps_den,
                      bool live, bool offline_rate_valid,
                      std::uint32_t max_frames, std::uint64_t max_bytes);
    ~OutputVideoWriter();

    // The render side owns an entry until the GPU worker fills or cancels it.
    // In offline mode this is the only per-sink back-pressure wait.
    std::shared_ptr<OutputVideoFrame> reserve(OutputVideoFrame metadata,
                                               bool wait);
    void complete(const std::shared_ptr<OutputVideoFrame> &frame,
                  std::vector<std::uint8_t> pixels);
    void cancel(const std::shared_ptr<OutputVideoFrame> &frame);
    void requestStop(std::int64_t monotonic_nanoseconds = -1);
    void abort(std::string error);
    bool finished() const;
    OutputVideoStatus status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Pelican
