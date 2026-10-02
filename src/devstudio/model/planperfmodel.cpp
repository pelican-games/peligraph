#include "planperfmodel.hpp"

#include "renderedithistory.hpp"

#include <algorithm>
#include <vector>
#include <cmath>
#include <cstdio>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

constexpr const char *Dot = " \xC2\xB7 ";
constexpr const char *Arrow = "\xE2\x86\x92";

std::string text(const Json &value, const char *key) {
    if (!value.is_object()) return {};
    const auto found = value.find(key);
    if (found == value.end() || !found->is_string()) return {};
    return found->get<std::string>();
}

std::size_t length(const Json &value, const char *key) {
    if (!value.is_object()) return 0;
    const auto found = value.find(key);
    return found != value.end() && found->is_array() ? found->size() : 0;
}

std::optional<std::uint64_t> unsignedAt(const Json &value, const char *key) {
    if (!value.is_object()) return std::nullopt;
    const auto found = value.find(key);
    if (found == value.end() || !found->is_number_unsigned()) return std::nullopt;
    return found->get<std::uint64_t>();
}

double numberAt(const Json &value, const char *key) {
    if (!value.is_object()) return 0.0;
    const auto found = value.find(key);
    return found != value.end() && found->is_number() ? found->get<double>() : 0.0;
}

bool flagAt(const Json &value, const char *key) {
    if (!value.is_object()) return false;
    const auto found = value.find(key);
    return found != value.end() && found->is_boolean() && found->get<bool>();
}

// "0.13", or three places under a hundredth so a small pass is not "0.00".
std::string number(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, std::abs(value) < 0.01 && value != 0.0 ? "%.3f" : "%.2f",
                  value);
    return buffer;
}

// Rows of node averages -- get_gpu_timing's `nodes`, and (ST-1) get_status
// gpu_timing's `node_averages`, which have the same shape: {view_index,
// node_kind, node_name, barriers_ms, body_ms, body_supported, sample_count}.
// A node's time is its barriers and its body, every view; the sum is the
// frame's.
double addNodeAverages(const Json &rows, std::map<std::string, double, std::less<>> &node_ms) {
    double total = 0.0;
    if (!rows.is_array()) return total;
    for (const auto &row : rows) {
        const auto name = text(row, "node_name");
        if (name.empty()) continue;
        double ms = numberAt(row, "barriers_ms");
        if (flagAt(row, "body_supported")) ms += numberAt(row, "body_ms");
        node_ms[name] += ms;
        total += ms;
    }
    return total;
}

std::string plural(std::size_t count, const char *one, const char *many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

} // namespace

std::string PlanPerfModel::milliseconds(double value) { return number(value) + " ms"; }

std::optional<PlanPerfStatusReading> PlanPerfModel::statusReading(const Json &status) {
    if (!status.is_object()) return std::nullopt;
    const auto timing = status.find("gpu_timing");
    if (timing == status.end() || !timing->is_object()) return std::nullopt;
    PlanPerfStatusReading reading;
    reading.off = !flagAt(*timing, "enabled") || !flagAt(*timing, "supported");
    reading.generation = unsignedAt(*timing, "generation");
    const auto samples = static_cast<std::size_t>(unsignedAt(*timing, "samples").value_or(0));
    // The frame's time: the graph variant the window holds most frames of
    // (one, while nothing switches variants).
    std::uint64_t most = 0;
    std::string variant;
    if (const auto averages = timing->find("logical_frame_averages");
        averages != timing->end() && averages->is_array())
        for (const auto &entry : *averages) {
            const auto frames = unsignedAt(entry, "frame_count").value_or(0);
            if (frames == 0 || frames <= most) continue;
            most = frames;
            variant = text(entry, "graph_variant");
            reading.gpu = PlanPerfGpu{samples, numberAt(entry, "average_total_ms"),
                                      numberAt(entry, "min_total_ms"),
                                      numberAt(entry, "max_total_ms")};
        }
    // The frames themselves, where the reply lists them: the time without a
    // new generation's first frames, and the spread between the 10th and the
    // 90th percentile rather than the window's single worst frame.
    std::vector<double> frames;
    if (const auto history = timing->find("logical_frame_history");
        reading.gpu && history != timing->end() && history->is_array())
        for (const auto &frame : *history)
            if (text(frame, "graph_variant") == variant) frames.push_back(numberAt(frame, "total_ms"));
    if (!frames.empty()) {
        const auto capacity = unsignedAt(*timing, "history_capacity").value_or(0);
        // Not full yet: the window still begins where the generation began.
        const bool from_start = capacity == 0 || frames.size() < capacity;
        const std::size_t skip =
            from_start ? std::min(SettlingFrames, frames.size() / 3) : std::size_t{0};
        frames.erase(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(skip));
        double sum = 0.0;
        for (const double frame : frames) sum += frame;
        std::ranges::sort(frames);
        const auto at = [&](double fraction) {
            const auto index = static_cast<std::size_t>(fraction * static_cast<double>(frames.size() - 1) + 0.5);
            return frames[std::min(index, frames.size() - 1)];
        };
        reading.gpu->average_ms = sum / static_cast<double>(frames.size());
        reading.gpu->low_ms = at(0.1);
        reading.gpu->high_ms = at(0.9);
    }
    // Decision 2: each node's time over the window. An engine with ST-1 says
    // it (`node_averages`, `nodes` being the last frame's samples only); one
    // before it lists every sample of the window in `nodes`, summed here.
    if (const auto averages = timing->find("node_averages");
        averages != timing->end() && averages->is_array()) {
        (void)addNodeAverages(*averages, reading.node_ms);
        return reading;
    }
    std::map<std::string, double, std::less<>> totals;
    if (const auto nodes = timing->find("nodes"); nodes != timing->end() && nodes->is_array())
        for (const auto &sample : *nodes) {
            if (!flagAt(sample, "supported")) continue;
            const auto name = text(sample, "node_name");
            if (!name.empty()) totals[name] += numberAt(sample, "ms");
        }
    if (samples > 0)
        for (const auto &[name, total] : totals)
            reading.node_ms.emplace(name, total / static_cast<double>(samples));
    return reading;
}

void PlanPerfModel::read(const PlanPerfStatusReading &reading) {
    status_read_ = true;
    gpu_off_ = reading.off;
    status_generation_ = reading.generation;
    gpu_ = reading.gpu;
    if (!timing_) node_ms_ = reading.node_ms;
    if (status_generation_ && gpu_) generations_[*status_generation_].gpu = gpu_;
    ++revision_;
    resolve();
}

std::optional<PlanPerfTimingReading> PlanPerfModel::timingReading(const Json &timing) {
    if (!timing.is_object() || text(timing, "schema") != "pelican.gpu_timing_node_averages")
        return std::nullopt;
    PlanPerfTimingReading reading;
    reading.off = !flagAt(timing, "enabled") || !flagAt(timing, "supported");
    reading.frames = static_cast<std::size_t>(unsignedAt(timing, "frame_count").value_or(0));
    if (const auto nodes = timing.find("nodes"); nodes != timing.end())
        reading.total_ms = addNodeAverages(*nodes, reading.node_ms);
    return reading;
}

void PlanPerfModel::read(const PlanPerfTimingReading &reading) {
    timing_ = reading;
    node_ms_ = reading.node_ms;
    ++revision_;
}

bool PlanPerfModel::readStatus(const Json &status) {
    const auto reading = statusReading(status);
    if (reading) read(*reading);
    return reading.has_value();
}

std::optional<PlanPerfPlanReading> PlanPerfModel::planReading(const Json &plan) {
    if (!plan.is_object() || !plan.contains("nodes") || !plan.at("nodes").is_array())
        return std::nullopt;
    PlanPerfPlanReading reading;
    reading.counts.passes = plan.at("nodes").size();
    reading.counts.barriers = length(plan, "barriers");
    if (const auto physical = plan.find("physical_target_plan");
        physical != plan.end() && physical->is_object())
        reading.counts.alias_groups = length(*physical, "alias_groups");
    reading.generation = unsignedAt(plan, "runtime_generation");
    return reading;
}

void PlanPerfModel::read(const PlanPerfPlanReading &reading) {
    counts_ = reading.counts;
    plan_generation_ = reading.generation;
    if (plan_generation_) generations_[*plan_generation_].counts = reading.counts;
    ++revision_;
    resolve();
}

bool PlanPerfModel::readPlan(const Json &plan) {
    const auto reading = planReading(plan);
    if (reading) read(*reading);
    return reading.has_value();
}

std::optional<std::uint64_t> PlanPerfModel::currentGeneration() const {
    if (status_generation_ && plan_generation_)
        return std::max(*status_generation_, *plan_generation_);
    return status_generation_ ? status_generation_ : plan_generation_;
}

void PlanPerfModel::noteEdit(const std::string &label) {
    if (label.empty()) return;
    const auto generation = currentGeneration();
    // Two edits under one generation: the after is the later one's.
    if (pending_ && pending_->epoch == epoch_ && pending_->generation == generation) {
        pending_->label = label;
        return;
    }
    Pending next;
    next.label = label;
    next.epoch = epoch_;
    next.generation = generation;
    if (generation)
        if (const auto found = generations_.find(*generation); found != generations_.end()) {
            next.counts = found->second.counts;
            next.gpu = found->second.gpu;
        }
    // get_status is read only while an edit waits, so the generation running
    // now may have no reading of its own: the node averages (get_gpu_timing,
    // whose window also starts with the generation) stand in, with no spread.
    if ((!next.gpu || next.gpu->samples < MeasuredSamples) && timing_ && !timing_->off &&
        timing_->frames >= MeasuredSamples)
        next.gpu = PlanPerfGpu{timing_->frames, timing_->total_ms, timing_->total_ms,
                               timing_->total_ms};
    pending_ = std::move(next);
}

void PlanPerfModel::engineStopped() {
    ++epoch_;
    generations_.clear();
    status_generation_.reset();
    plan_generation_.reset();
    status_read_ = false;
    gpu_off_ = false;
    gpu_.reset();
    timing_.reset();
    counts_.reset();
    node_ms_.clear();
    ++revision_;
}

std::optional<std::uint64_t> PlanPerfModel::planWanted() const {
    if (!pending_) return std::nullopt;
    const auto generation = status_generation_ ? status_generation_ : plan_generation_;
    if (!generation) return std::nullopt;
    if (pending_->epoch == epoch_ && pending_->generation &&
        *generation <= *pending_->generation)
        return std::nullopt;
    const auto found = generations_.find(*generation);
    if (found != generations_.end() && found->second.counts) return std::nullopt;
    return generation;
}

void PlanPerfModel::resolve() {
    if (!pending_) return;
    // Timing off: the plan's generation is the only one there is.
    const auto generation = gpu_off_ || !status_generation_ ? plan_generation_ : status_generation_;
    if (!generation) return;
    // Still the before, or a reading older than it.
    if (pending_->epoch == epoch_ && pending_->generation && *generation <= *pending_->generation)
        return;
    const auto found = generations_.find(*generation);
    if (found == generations_.end() || !found->second.counts) return;
    const auto &after = found->second;
    if (!gpu_off_ && (!after.gpu || after.gpu->samples < ComparedSamples)) return;

    const auto &before = pending_->counts;
    const auto pair = [&](std::size_t PlanPerfCounts::*field) {
        return (before ? std::to_string((*before).*field) : std::string{"?"}) + Arrow +
               std::to_string((*after.counts).*field);
    };
    std::string line = "after " + pending_->label + ": passes " + pair(&PlanPerfCounts::passes) +
                       ", barriers " + pair(&PlanPerfCounts::barriers) + ", alias " +
                       pair(&PlanPerfCounts::alias_groups);
    const bool before_gpu = pending_->gpu && pending_->gpu->samples >= MeasuredSamples;
    if (gpu_off_) {
        line += ", GPU timing off";
    } else {
        line += ", GPU " + (before_gpu ? number(pending_->gpu->average_ms) : std::string{"?"}) +
                Arrow + milliseconds(after.gpu->average_ms);
    }
    if (before && *before == *after.counts) line = "no change in the plan" + std::string{Dot} + line;
    if (!gpu_off_ && before_gpu) {
        // How far the time moves on its own is the before's spread: that
        // window had settled. The after's is read only when the before has
        // none (a reading of node averages stood in for it). After a restart
        // the after's own spread is the driver still swapping in optimised
        // shader code for seconds (gui_build_optimize's 4 samples: 0.15 to
        // 0.40 ms read as "within noise" when it counted).
        const double before_spread = pending_->gpu->high_ms - pending_->gpu->low_ms;
        const double noise =
            before_spread > 0.0 ? before_spread : after.gpu->high_ms - after.gpu->low_ms;
        if (std::abs(after.gpu->average_ms - pending_->gpu->average_ms) < noise)
            line += std::string{Dot} + "GPU difference is within noise";
    }
    // The generations compared: two edits that moved the same numbers the
    // same way are still told apart.
    line += std::string{Dot} + "generation " +
            (pending_->generation ? std::to_string(*pending_->generation) : std::string{"?"}) +
            Arrow + std::to_string(*generation);
    delta_line_ = std::move(line);
    pending_.reset();
    ++revision_;
}

std::string PlanPerfModel::summaryLine() const {
    std::string line;
    const auto add = [&](const std::string &part) {
        line += (line.empty() ? std::string{} : std::string{Dot}) + part;
    };
    if (timing_) {
        if (timing_->off)
            add("GPU timing off");
        else if (timing_->frames < MeasuredSamples)
            add("GPU measuring\xE2\x80\xA6");
        else
            add("GPU " + milliseconds(timing_->total_ms));
    } else if (status_read_ || counts_) {
        if (status_read_ && gpu_off_)
            add("GPU timing off");
        else if (!gpu_ || gpu_->samples < MeasuredSamples)
            add("GPU measuring\xE2\x80\xA6");
        else
            add("GPU " + milliseconds(gpu_->average_ms));
    }
    if (counts_) {
        add(plural(counts_->passes, "pass", "passes"));
        add(plural(counts_->barriers, "barrier", "barriers"));
        add(plural(counts_->alias_groups, "alias group", "alias groups"));
    }
    return line;
}

std::string PlanPerfModel::editLabel(const std::string &method, const Json &params) {
    if (method == "edit_rendering_keys") {
        const auto set = params.is_object() ? params.value("set", Json::object()) : Json::object();
        const auto remove = params.is_object() ? params.value("remove", Json::array()) : Json::array();
        const auto set_count = set.is_object() ? set.size() : 0;
        const auto remove_count = remove.is_array() ? remove.size() : 0;
        if (set_count == 1 && remove_count == 0) return set.begin().key();
        if (set_count == 0 && remove_count == 1 && remove.front().is_string())
            return remove.front().get<std::string>();
        return "rendering settings";
    }
    if (method == "edit_graph_document") {
        if (!params.is_object()) return {};
        const auto ops = params.find("ops");
        if (ops == params.end() || !ops->is_array()) return {};
        for (const auto &op : *ops) {
            const auto kind = text(op, "op");
            // A placement moves nothing the engine draws.
            if (kind.empty() || kind == "set_layout") continue;
            const auto scope = text(op, "scope");
            const auto prefix = scope.empty() || scope == "root" ? std::string{} : scope + "/";
            auto node = text(op, "node");
            if (node.empty() && op.is_object() && op.contains("node")) node = text(op.at("node"), "id");
            if (kind == "set_node_parameters") {
                const auto parameters = op.value("parameters", Json::object());
                if (parameters.is_object() && parameters.size() == 1)
                    return prefix + node + "." + parameters.begin().key();
                return prefix + node;
            }
            if (kind == "set_input") return prefix + node + "." + text(op, "port");
            if (kind == "add_node") return "add " + prefix + node;
            if (kind == "remove_node") return "remove " + prefix + node;
            return node.empty() ? kind : kind + " " + prefix + node;
        }
        return {};
    }
    // The render edit history's Undo and Redo.
    if (method == "restore_render_documents") return "Undo / Redo";
    if (RenderEditHistory::recordsMethod(method)) return RenderEditHistory::labelFor(method, params);
    return {};
}

} // namespace PelicanStudio
