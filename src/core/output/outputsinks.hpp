#pragma once

#include "../container.hpp"
#include "./outputfile.hpp"
#include "./outputsinktable.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {

class Renderer;

// VO-1a: the loaded document's file sinks, the run's only writer of raw
// render-target frames while the engine runs. The table is installed by the
// exact publication of a compiled generation, never by a candidate that was
// rolled back. A launch without --output-dir has no active sinks and never
// reads a render target.
DECLARE_MODULE(OutputSinks) {
  public:
    struct State {
        std::string name;
        std::string kind;
        std::string graph;
        std::string target;
        // VO-1b: "raw" or "png". The writer refuses neither at runtime;
        // lowering already refused every other spelling.
        std::string format = "raw";
        std::string state = "inactive";
        // Why a finished sink stopped. "none" means it did not stop at a
        // limit (for example, every_n only thinned its frames).
        std::string stop_reason = "none";
        std::uint64_t stop_value = 0;
        std::uint64_t frames_written = 0;
        std::string last_error;
    };

  private:
    mutable std::mutex mutex;
    std::shared_ptr<const OutputSinkTable> table;
    std::vector<std::shared_ptr<State>> states;
    bool active = false;
    std::filesystem::path directory;
    std::uint32_t max_frames = 1000;
    std::uint64_t max_bytes = 4ull * 1024ull * 1024ull * 1024ull;
    std::uint64_t bytes_written = 0;
    std::uint64_t frames_skipped_inactive = 0;
    std::uint64_t frames_skipped_no_contents = 0;
    // The problem list module may not exist when a generation is first
    // published. Keep the need to publish until a frame can reach it.
    bool problems_need_sync = true;

  public:
    OutputSinks();

    // Called by the registration publication callback with the table of the
    // generation that became current. Counters survive a reload only for a
    // sink whose name, kind, graph and target are unchanged.
    void install(std::shared_ptr<const OutputSinkTable> next);

    // Once per rendered logical frame, between publishFrame() and
    // advanceFrameHistories(). A sink failure is recorded and never ends the
    // frame; only a device loss escapes.
    void captureFrame(Renderer &renderer);

    nlohmann::json status() const;
};

// VO-1b: the problems a sink table produces, without reading the live module.
// A refused sink and a sink stopped by a limit each contribute one problem;
// running, inactive and finished-without-a-limit sinks contribute none. The
// caller passes the current generation's states so a result from an old
// shared_ptr<State> can never add a problem to a new generation.
std::vector<nlohmann::json> outputSinkProblems(
    const std::vector<OutputSinks::State> &states);

} // namespace Pelican
