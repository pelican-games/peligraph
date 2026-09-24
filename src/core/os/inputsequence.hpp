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
    // How this frame's cursor events became framebuffer pixels when it was
    // recorded (GP-4). A v1 recording carries none: scale 1, framebuffer
    // unknown, which is what its window coordinates meant then.
    PointerTransform pointer_transform;
};

// pelican.input_seq, JSON Lines. It is written wherever the recorder was told
// (--record-input, start_input_record), not as a project document, so the
// project migration (MG-1) never sees it; the parser reads both versions.
//
// v2 (written): the header carries the first frame's pointer transform as
// "content_scale":[x, y] and "framebuffer":[width, height]; a frame whose
// transform differs from the frame before carries both fields on its marker.
// v1 (read): no transform; every frame reads as scale 1 with the framebuffer
// unknown, the meaning its window coordinates had when it was written.
class InputSequence {
    double sequence_fps = 60.0;
    std::vector<InputSequenceFrame> frame_samples;
    int source_version = 2;

  public:
    static InputSequence fromJsonLines(std::string_view json_lines);
    static InputSequence loadFile(const std::filesystem::path &path);

    explicit InputSequence(double fps = 60.0);

    double fps() const noexcept { return sequence_fps; }
    // The version it was read from; a sequence built in memory is v2.
    int sourceVersion() const noexcept { return source_version; }
    std::span<const InputSequenceFrame> frames() const noexcept { return frame_samples; }
    void appendFrame(std::span<const InputEvent> events, const PointerTransform &pointer_transform = {});
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

    // Gives the next frame its events and its pointer transform: the recorded
    // one while replaying (a v1 recording's unknown framebuffer is taken from
    // live), live otherwise. live is this machine's transform.
    void prepareFrame(InputState &input, const PointerTransform &live = {});
    // Records the frame's events and its pointer transform.
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
