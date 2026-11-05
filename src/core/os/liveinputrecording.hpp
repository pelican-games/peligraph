#pragma once

#include "../container.hpp"
#include "../appflow/enginetime.hpp"
#include "liveinputs.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

enum class LiveRecordingClockMode : std::uint8_t { fixed, realtime };

struct LiveRecordingClock {
    LiveRecordingClockMode mode = LiveRecordingClockMode::fixed;
    double fixed_step = 1.0 / 60.0;
    std::uint64_t start_frame = 1;
    double start_time = 0.0;
    std::uint64_t time_set_revision = 0;
};

LiveRecordingClock liveRecordingClock(const EngineTime &time,
                                      EngineTime::Mode mode);
std::string liveRecordingProjectDigest(const std::filesystem::path &project_root);

struct LiveRecordingFrame {
    std::uint64_t frame = 0;
    double time = 0.0;
    std::map<std::string, double, std::less<>> values;
};

struct LiveRecordingEnd {
    std::uint64_t frame = 0;
    double time = 0.0;
    std::uint64_t evaluations = 0;
};

// Only scalar value snapshots are recorded. Triggers are not part of version 1.
// Fixed-step lookup reproduces relative evaluation indices, including held
// paused frames. Any other clock pairing resamples by time and is not
// frame-identical; the last value at an exactly equal time wins.
class LiveInputRecording {
  public:
    LiveRecordingClock clock;
    std::string project_digest;
    std::string graph_digest;
    std::vector<LiveInputSpec> inputs;
    std::vector<LiveRecordingFrame> frames;
    LiveRecordingEnd end;

    static LiveInputRecording fromJsonLines(std::string_view text);
    static LiveInputRecording loadFile(const std::filesystem::path &path);
    std::string toJsonLines() const;

    const LiveInputSpec *declaration(std::string_view name) const;
    std::map<std::string, double, std::less<>> valuesAtTime(double time) const;
    std::map<std::string, double, std::less<>> valuesAtFrame(
        std::uint64_t relative_frame) const;
};

struct LiveRecordStatus {
    std::filesystem::path path;
    std::uint64_t lines = 0;
    std::uint64_t evaluations = 0;
    double total_write_ms = 0.0;
    double max_write_ms = 0.0;
};

DECLARE_MODULE(LiveInputRecordingRuntime) {
  public:
    LiveInputRecordingRuntime();
    ~LiveInputRecordingRuntime();

    void startRecording(const std::filesystem::path &path,
                        std::vector<LiveInputSpec> inputs,
                        const LiveRecordingClock &clock,
                        std::string project_digest = {},
                        std::string graph_digest = {});
    void stopRecording();
    void startReplay(std::shared_ptr<const LiveInputRecording> recording,
                     const LiveRecordingClock &clock);
    void setReplayClock(const LiveRecordingClock &clock);
    void stopReplay();

    void prepareFrame(const LiveRecordingClock &clock);
    std::map<std::string, double, std::less<>> replayValues(
        std::uint64_t frame, double time) const;
    void recordFrame(std::uint64_t frame, double time,
                     const std::vector<LiveInputSnapshot> &snapshot);

    bool isRecording() const noexcept { return recording_.has_value(); }
    bool isReplaying() const noexcept { return replay_.has_value(); }
    const LiveInputRecording *recordingData() const noexcept {
        return replay_ ? replay_->recording.get() : launch_recording_.get();
    }
    std::optional<LiveRecordStatus> recordStatus() const;
    std::uint64_t replayLines() const noexcept;

    // Deterministic fault injection for the frame-boundary failure test.
    void failRecordingAfterLinesForTesting(std::uint64_t lines);

  private:
    struct RecordingState {
        std::filesystem::path path;
        std::ofstream output;
        LiveRecordingClock clock;
        std::vector<LiveInputSpec> inputs;
        std::map<std::string, double, std::less<>> previous;
        LiveRecordStatus status;
        std::uint64_t last_frame = 0;
        double last_time = 0.0;
        std::uint64_t fail_after = 0;
        bool first = true;
    };
    struct ReplayState {
        std::shared_ptr<const LiveInputRecording> recording;
        LiveRecordingClock clock;
        bool frame_lookup = false;
    };

    void failRecording(std::string_view detail);
    void reportProblem(std::string_view code, std::string_view severity,
                       std::string_view message) const;
    void writeLine(const std::string &line);

    std::optional<RecordingState> recording_;
    std::optional<ReplayState> replay_;
    std::shared_ptr<const LiveInputRecording> launch_recording_;
};

} // namespace Pelican
