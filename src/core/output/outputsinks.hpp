#pragma once

#include "../container.hpp"
#include "./outputfile.hpp"
#include "./outputsinktable.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {

class Renderer;

// The loaded document's file sinks. The render thread only records copies;
// one worker waits for the GPU, encodes and writes. File sinks default to
// "wait" to preserve every frame; only "drop" gives a non-waiting full queue.
// Each kind is armed by its own flag: file and video by --output-dir, Spout by
// --output-spout. A Spout sink always drops, and only hands the worker's copy to
// its sender's thread (outputspout.hpp), so it can't hold up either thread.
DECLARE_MODULE(OutputSinks) {
  public:
    struct State {
        std::string name;
        std::string kind;
        std::string node;
        std::string graph;
        std::string source_graph;
        std::string target;
        std::string format = "raw";
        std::string when_full = "wait";
        std::string state = "inactive";
        std::string stop_reason = "none";
        std::uint64_t stop_value = 0;
        std::uint64_t frames_written = 0;
        std::string last_error;
        std::uint64_t frames_dropped = 0;
        std::uint64_t frames_discarded = 0;
        std::string mode = "offline";
        std::uint64_t frames_repeated = 0;
        std::uint64_t encoder_pid = 0;
        std::string encoder_state = "not_started";
        std::uint64_t final_bytes = 0;
        std::uint64_t queue_depth = 0;
        double render_wait_ms = 0.0;
        // VO-4: output.spout only. No receiver count: it can't be read reliably.
        bool registered = false;
        std::string sender_name;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        // VO-5b: output.spout only: transport, its reason and its counters.
        nlohmann::json spout_transport;
    };

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;

  public:
    OutputSinks();
    ~OutputSinks();

    // Called only when a compiled generation becomes current. Counters survive
    // a reload for a sink whose name, kind, graph and target are unchanged.
    void install(std::shared_ptr<const OutputSinkTable> next);

    // Runs after publishFrame(). It never does file I/O. A full "wait" pool is
    // its only condition-variable wait; device loss escapes as before.
    void captureFrame(Renderer &renderer);

    nlohmann::json status() const;

#if PELICAN_OUTPUT_TEST_SEAMS
    // The true call returns after the worker acknowledges its paused loop.
    void pauseWorkerForTesting(bool paused);
    void resumeWorkerAfterForTesting(std::chrono::milliseconds delay);
    void gateWorkerForTesting(bool hold, bool wait,
                              std::chrono::milliseconds release_after);
    nlohmann::json takeCaptureTimingsForTesting();
#endif
};

std::vector<nlohmann::json> outputSinkProblems(
    const std::vector<OutputSinks::State> &states);

} // namespace Pelican
