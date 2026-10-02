#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace PelicanStudio {

// UX-6b. What the frame costs and how big its plan is, read where Studio
// already reads them -- get_status's gpu_timing and get_frame_plan -- and kept
// per runtime generation so an edit's before and after can be put side by
// side. Qt-free: the panel that owns the transport hands every reply in.
//
// One generation is one window of GPU timing: the engine empties the window
// when a generation begins (PO-1), so the generation an edit was sent under
// and the one that follows it are the before and the after.

// The plan's size, as the status line counts it.
struct PlanPerfCounts {
    std::size_t passes = 0;
    std::size_t barriers = 0;
    std::size_t alias_groups = 0;

    bool operator==(const PlanPerfCounts &) const = default;
};

// One reading of a generation's GPU window (get_status gpu_timing).
struct PlanPerfGpu {
    // Logical frames in the window.
    std::size_t samples = 0;
    double average_ms = 0;
    // How far a frame's time moves on its own: the 10th and 90th percentile
    // of the window's frames (get_status logical_frame_history), or its
    // min / max where the reply lists no frames. A window that still begins
    // where its generation began leaves out its first frames -- the
    // pipelines' first compiles (a restart's are many times the frame).
    double low_ms = 0;
    double high_ms = 0;
};

// What one get_status says of the GPU window, and what one plan says of its
// size: taken out of the reply where it is parsed (the owner may do that off
// the main thread -- a full window is a megabyte), and handed in.
struct PlanPerfStatusReading {
    bool off = false;
    std::optional<std::uint64_t> generation;
    std::optional<PlanPerfGpu> gpu;
    std::map<std::string, double, std::less<>> node_ms;
};

struct PlanPerfPlanReading {
    PlanPerfCounts counts;
    std::optional<std::uint64_t> generation;
};

// What one get_gpu_timing says: each node's average over the engine's
// 30-frame window (a few kilobytes, where get_status carries every sample of
// 120 frames). The frame's time is the sum of its nodes'.
struct PlanPerfTimingReading {
    bool off = false;
    std::size_t frames = 0;
    double total_ms = 0;
    std::map<std::string, double, std::less<>> node_ms;
};

class PlanPerfModel {
  public:
    using Json = nlohmann::json;

    // nullopt: the reply carried no gpu_timing / is not a whole plan.
    [[nodiscard]] static std::optional<PlanPerfStatusReading> statusReading(const Json &status);
    [[nodiscard]] static std::optional<PlanPerfPlanReading> planReading(const Json &plan);
    [[nodiscard]] static std::optional<PlanPerfTimingReading> timingReading(const Json &timing);
    void read(const PlanPerfStatusReading &reading);
    void read(const PlanPerfPlanReading &reading);
    // The status line's time and the nodes' times follow get_gpu_timing once
    // one was read; get_status keeps the generations an edit is compared by.
    void read(const PlanPerfTimingReading &reading);

    // Decision 1: the status line shows a time once the window holds this
    // many frames, "measuring…" before.
    static constexpr std::size_t MeasuredSamples = 30;
    // Decision 3: the after of an edit is read once its window holds this
    // many frames.
    static constexpr std::size_t ComparedSamples = 60;
    // The first frames of a generation the time and its spread leave out.
    static constexpr std::size_t SettlingFrames = 20;

    // A get_status result. False when it carried no gpu_timing.
    bool readStatus(const Json &status);
    // A get_frame_plan result (the whole plan, not a page of loop elements).
    bool readPlan(const Json &plan);
    // An edit that can move the plan is going out (`label` names it, see
    // editLabel). Its before is the generation the engine runs now.
    void noteEdit(const std::string &label);
    // The engine went away: a new one numbers its generations afresh. An edit
    // waiting for its after keeps its before (a restart is such an edit).
    void engineStopped();

    // Decision 1: "GPU 0.13 ms · 24 passes · 20 barriers · 2 alias groups";
    // empty before anything was read.
    [[nodiscard]] std::string summaryLine() const;
    // Decision 3: the last edit's before and after, once its after is read;
    // empty before that.
    [[nodiscard]] const std::string &deltaLine() const noexcept { return delta_line_; }
    // Decision 2: each plan node's GPU time, the average over the window
    // (barriers and body together, every view), by the plan node's name --
    // get_gpu_timing's once one was read, else get_status's.
    [[nodiscard]] const std::map<std::string, double, std::less<>> &nodeMs() const noexcept {
        return node_ms_;
    }
    // An edit is waiting for its after.
    [[nodiscard]] bool waiting() const noexcept { return pending_.has_value(); }
    // The generation whose plan the waiting edit needs and nobody has read:
    // the owner reads the plan once for it.
    [[nodiscard]] std::optional<std::uint64_t> planWanted() const;
    // Moves whenever a line or a node time changes.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    // The name an edit request goes by in the delta line, or empty for a
    // request that is not an edit that can move the plan (a read, a
    // placement): edit_rendering_keys by its key ("draw_sort"), a graph edit
    // by what its first op touches ("loop.count"), the render edit history's
    // edits by the history's label.
    [[nodiscard]] static std::string editLabel(const std::string &method, const Json &params);
    // "0.13 ms"
    [[nodiscard]] static std::string milliseconds(double value);

  private:
    struct Generation {
        std::optional<PlanPerfCounts> counts;
        std::optional<PlanPerfGpu> gpu;
    };
    struct Pending {
        std::string label;
        std::uint64_t epoch = 0;
        std::optional<std::uint64_t> generation;
        std::optional<PlanPerfCounts> counts;
        std::optional<PlanPerfGpu> gpu;
    };

    [[nodiscard]] std::optional<std::uint64_t> currentGeneration() const;
    void resolve();

    std::map<std::uint64_t, Generation> generations_;
    std::uint64_t epoch_ = 0;
    // The generation the last status reading's window began at.
    std::optional<std::uint64_t> status_generation_;
    std::optional<std::uint64_t> plan_generation_;
    // get_status said: timing off or not supported (and why).
    bool gpu_off_ = false;
    bool status_read_ = false;
    std::optional<PlanPerfGpu> gpu_;
    std::optional<PlanPerfTimingReading> timing_;
    std::optional<PlanPerfCounts> counts_;
    std::map<std::string, double, std::less<>> node_ms_;
    std::optional<Pending> pending_;
    std::string delta_line_;
    std::uint64_t revision_ = 0;
};

} // namespace PelicanStudio
