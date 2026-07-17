#pragma once

#include "../container.hpp"
#include "inputstate.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct InputSequenceFrame {
    std::uint64_t frame = 0;
    std::vector<InputEvent> events;
};

class InputSequence {
    double sequence_fps = 60.0;
    std::vector<InputSequenceFrame> frame_samples;

  public:
    static InputSequence fromJsonLines(std::string_view json_lines);
    static InputSequence loadFile(const std::filesystem::path &path);

    explicit InputSequence(double fps = 60.0);

    double fps() const noexcept { return sequence_fps; }
    std::span<const InputSequenceFrame> frames() const noexcept { return frame_samples; }
    void appendFrame(std::span<const InputEvent> events);
    std::string toJsonLines() const;
    void writeFile(const std::filesystem::path &path) const;
};

struct InputRecordResult {
    std::filesystem::path path;
    std::size_t frames = 0;
    std::size_t events = 0;
};

DECLARE_MODULE(InputSequenceRuntime) {
    enum class Mode : std::uint8_t {
        idle,
        recording,
        replaying,
    };

    Mode mode = Mode::idle;
    InputSequence sequence;
    std::filesystem::path output_path;
    std::size_t replay_frame = 0;
    std::optional<std::uint64_t> replay_sequence_offset;

    std::size_t eventCount() const noexcept;

  public:
    ~InputSequenceRuntime();

    void startRecording(const std::filesystem::path &path, double fps,
                        std::span<const std::string> pose_action_names = {});
    InputRecordResult stopRecording();
    void startReplay(const std::filesystem::path &path,
                     std::span<const std::string> pose_action_names = {});
    void stopReplay();

    void prepareFrame(InputState &input);
    void recordFrame(const FrameInput &frame_input);

    bool isRecording() const noexcept { return mode == Mode::recording; }
    bool isReplaying() const noexcept { return mode == Mode::replaying; }
    bool replayComplete() const noexcept;
    std::size_t replayFrameCount() const noexcept;
    std::size_t replayFrameIndex() const noexcept { return replay_frame; }
    double replayFps() const noexcept { return sequence.fps(); }
    const std::filesystem::path &recordPath() const noexcept { return output_path; }
};

} // namespace Pelican
