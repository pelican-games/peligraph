#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// FG-2: verify_fold (docs/design_graph_layers.md 5.3). A fold group of kind
// "decisions" is the same as its open form when, drawn by the engine's own
// executable in a headless child process, the form with the group's decisions
// gives the same pictures as the form without them and no validation error the
// open form does not give. Three runs of one copy of the project: the open
// form twice (the control: two runs that must agree with each other, or nothing
// can be judged) and the decided form once. Each run draws `warmup_seconds` of
// fixed-step frames and throws them away (the NVIDIA driver swaps the code it
// compiled first for an optimized one seconds later), then writes `frames`
// frames.
namespace Pelican {

// The control runs differ from each other: {group, detail: {frame,
// differing_pixels}}. Not the decisions' fault; nothing could be compared.
inline constexpr std::string_view graphFoldVerifyUnstableCode = "graph.fold_verify_unstable@1";
// The control runs agree and the decided run does not: {group, what: "pixels"
// | "validation" | "compile", detail: {frame?, differing_pixels?,
// validation_errors?, control_validation_errors?, exit_code?, message?}}.
inline constexpr std::string_view graphFoldMismatchCode = "graph.fold_mismatch@1";
// A verify of the same group is running: {group, ticket}.
inline constexpr std::string_view foldVerifyBusyCode = "fold_verify_busy";
// This launch cannot start the children (headless, replay or golden runs,
// or no way to start the engine's own executable): {reason}.
inline constexpr std::string_view foldVerifyUnavailableCode = "fold_verify_unavailable";
// The open form itself could not be drawn (a control run failed, or the copy
// of the project could not be made): {group, run: "open" | "setup",
// detail: {exit_code?, message}}. Nothing was compared.
inline constexpr std::string_view foldVerifyFailedCode = "fold_verify_failed";

struct FoldVerifyDocument {
    // Absolute, inside the job's project_root.
    std::filesystem::path path;
    std::string bytes;
};

// What a worker needs to draw the two forms. Everything is captured on the
// engine's thread when the request is accepted, so the worker reads nothing
// the editor may change meanwhile.
struct FoldVerifyJob {
    std::string group;
    std::filesystem::path project_root;
    // Absolute path of the rendering root document (inside project_root).
    std::filesystem::path root_path;
    // The root as of base_source_digest without the group's verified mark
    // (the open form) and with verified.digest the group's digest now (the
    // decided form).
    std::string open_root_bytes;
    std::string decided_root_bytes;
    // The other documents the composition reads from the project as the
    // editor holds them (managed fragments), written over the copy.
    std::vector<FoldVerifyDocument> documents;
    std::uint32_t frames = 3;
    double warmup_seconds = 30.0;
    // How many pixels of one frame may differ; 0 is byte equality.
    std::uint64_t tolerance = 0;
};

// One decoded frame, RGBA8.
struct FoldVerifyFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

// One child run.
struct FoldVerifyRun {
    // The process exited 0 and every frame was read back.
    bool finished = false;
    int exit_code = 0;
    std::vector<FoldVerifyFrame> frames;
    // Lines of the child's stdout and stderr that carry "Validation Error".
    std::uint64_t validation_errors = 0;
    // Why the run did not finish (the tail of its stderr, or the runner's
    // own error).
    std::string message;
};

struct FoldVerifyRuns {
    // The project copy could not be made: the runs are empty.
    std::optional<std::string> setup_failure;
    // The engine is shutting down: nothing is judged.
    bool cancelled = false;
    FoldVerifyRun open_first;
    FoldVerifyRun open_second;
    FoldVerifyRun decided;
};

// Draws the three runs. Called on a worker thread; must not throw (a failure
// is a run that did not finish). `cancelled` turns true when the engine
// shuts down.
using FoldVerifyRunner = std::function<FoldVerifyRuns(const FoldVerifyJob &, const std::atomic<bool> &cancelled)>;

struct FoldVerifyVerdict {
    bool passed = false;
    // When not passed: the refusal (one of the codes above), its message and
    // its fields (group first).
    std::string code;
    std::string message;
    nlohmann::ordered_json fields = nlohmann::ordered_json::object();
};

// Pixels whose four bytes differ. Frames of different sizes differ in every
// pixel of the larger one.
std::uint64_t foldVerifyDifferingPixels(const FoldVerifyFrame &left, const FoldVerifyFrame &right);

// The judgement: a control run that did not finish is fold_verify_failed;
// controls that differ by more than `tolerance` pixels in a frame are
// graph.fold_verify_unstable@1; a decided run that did not finish is
// graph.fold_mismatch@1 what "compile", one whose frame differs from the first
// control by more than `tolerance` pixels what "pixels", one with more
// validation errors than either control what "validation". Frames are
// numbered from 1, as --render-out numbers them.
FoldVerifyVerdict judgeFoldVerifyRuns(const std::string &group, const FoldVerifyRuns &runs, std::uint64_t tolerance);

// The production runner: copies the project to a directory of its own under
// the system temp directory (everything but a top-level .git), writes the
// documents, and starts `executable` three times, one after the other:
//   --headless --project <copy> --frames N --fps F [--size WxH] [--start-time S]
//   --render-out <run>/frame_%d.png --vulkan-validation on
//   --log-file <run>/pelican.log  <extra_arguments...>
// with stdout and stderr going to files in the run's directory. A child that
// outlives `run_timeout`, or that is still running when `cancelled` turns
// true and `stop_grace` has passed, is ended by closing its job object (the
// one handle that owns the child and whatever it started; never by name or
// pid), and the runner waits for it before removing the copy.
struct FoldVerifyProcessOptions {
    std::filesystem::path executable;
    std::vector<std::filesystem::path> extra_arguments;
    double fps = 60.0;
    // --size WxH of every run (0: the player's default).
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::chrono::seconds run_timeout{900};
    std::chrono::milliseconds stop_grace{3000};
};
FoldVerifyRuns runFoldVerifyProcesses(const FoldVerifyJob &job, const FoldVerifyProcessOptions &options,
                                      const std::atomic<bool> &cancelled);

// The executable this process runs (nullopt where the platform cannot say,
// or where the runner cannot start children: anything but Windows).
std::optional<std::filesystem::path> foldVerifyExecutablePath();

// "2026-10-02T12:00:00Z", now.
std::string foldVerifyUtcNow();

} // namespace Pelican
