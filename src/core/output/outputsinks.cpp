#include "outputsinks.hpp"

#include "../launchconfig.hpp"
#include "../vkcore/renderer.hpp"
#include "../watch/problemlist.hpp"

#include <algorithm>
#include <stdexcept>

namespace Pelican {
namespace {

constexpr std::string_view outputProblemDomain = "output";

std::string sinkError(const OutputSinkSpec &sink) {
    if (sink.error_code.empty()) return {};
    if (sink.error_message.empty()) return sink.error_code;
    return sink.error_code + ": " + sink.error_message;
}

bool sameIdentity(const OutputSinks::State &state, const OutputSinkSpec &sink) {
    return state.name == sink.name && state.kind == sink.kind &&
           state.graph == sink.graph && state.target == sink.target;
}

std::string problemCode(std::string_view error, std::string_view fallback) {
    const auto colon = error.find(':');
    auto code = error.substr(0, colon);
    while (!code.empty() && (code.back() == ' ' || code.back() == '\t'))
        code.remove_suffix(1);
    if (code.empty() || !code.starts_with("output."))
        return std::string{fallback};
    return std::string{code};
}

bool replaceOutputProblems(const std::vector<nlohmann::json> &problems) {
    auto *list = FastModuleContainer::tryGet<ProblemList>();
    if (list == nullptr) return false;
    list->replace(outputProblemDomain, problems);
    return true;
}

std::vector<OutputSinks::State> statesToValues(
    const std::vector<std::shared_ptr<OutputSinks::State>> &states) {
    std::vector<OutputSinks::State> values;
    values.reserve(states.size());
    for (const auto &state : states) values.push_back(*state);
    return values;
}

} // namespace

// The pure part of the sink status, shared with the CPU tests. A problem is
// emitted only for a refused sink or a sink stopped by a limit. Running,
// inactive and finished-without-a-limit sinks produce none, so a table reload
// that drops a problem replaces the empty list and withdraws it.
std::vector<nlohmann::json> outputSinkProblems(
    const std::vector<OutputSinks::State> &states) {
    std::vector<nlohmann::json> problems;
    for (const auto &state : states) {
        std::string code;
        if (state.state == "refused") {
            code = problemCode(state.last_error, "output.write_failed@1");
        } else if (state.state == "finished" && state.stop_reason != "none") {
            code = "output.limit_reached@1";
        } else {
            continue;
        }
        nlohmann::json fields{
            {"graph", state.graph},
            {"sink", state.name},
        };
        if (state.state == "finished" && state.stop_reason != "none") {
            fields["limit"] = state.stop_reason;
            fields["value"] = state.stop_value;
        }
        auto message = state.last_error;
        if (message.empty()) {
            message = code;
            if (state.state == "finished") {
                message += ": " + state.stop_reason + " " +
                           std::to_string(state.stop_value) + " reached";
            }
        }
        problems.push_back(nlohmann::json{
            {"severity", "error"},
            {"source", "runtime"},
            {"code", code},
            {"message", message},
            {"scope", state.graph},
            {"node", state.name},
            {"fields", std::move(fields)},
        });
    }
    return problems;
}

OutputSinks::OutputSinks() {
    if (const auto *config = FastModuleContainer::tryGet<EngineLaunchConfig>();
        config != nullptr) {
        max_frames = config->output_max_frames;
        max_bytes = config->output_max_bytes;
        if (config->output_dir) {
            active = true;
            directory = *config->output_dir;
        }
    }
}

void OutputSinks::install(std::shared_ptr<const OutputSinkTable> next) {
    std::vector<nlohmann::json> problems;
    {
        std::lock_guard lock{mutex};
        std::vector<std::shared_ptr<State>> old = std::move(states);
        states.clear();
        table = std::move(next);
        if (table) {
            states.reserve(table->sinks.size());
            for (const auto &sink : table->sinks) {
                auto state = std::make_shared<State>();
                state->name = sink.name;
                state->kind = sink.kind;
                state->graph = sink.graph;
                state->target = sink.target;
                state->format = sink.format;
                for (const auto &previous : old) {
                    if (sameIdentity(*previous, sink)) {
                        // A reload keeps the worked counters of the same sink
                        // and starts its state afresh: a refusal or a finished
                        // run is a property of the generation that produced it.
                        state->frames_written = previous->frames_written;
                        break;
                    }
                }
                // No --output-dir means no sink is active. It also means it
                // cannot refuse: an error_code carried by the document is not
                // a runtime problem when no file can be written at all.
                if (!active) {
                    state->state = "inactive";
                } else if (!sink.error_code.empty()) {
                    state->state = "refused";
                    state->last_error = sinkError(sink);
                } else if (max_frames != 0 &&
                           state->frames_written >= max_frames) {
                    state->state = "finished";
                    state->stop_reason = "max_frames";
                    state->stop_value = max_frames;
                } else if (max_bytes != 0 && bytes_written >= max_bytes) {
                    // The run's byte budget was exhausted before this
                    // generation was installed. The sink starts stopped by
                    // that limit rather than pretending it can write.
                    state->state = "finished";
                    state->stop_reason = "max_bytes";
                    state->stop_value = max_bytes;
                    state->last_error = "output.limit_reached@1: max_bytes " +
                                        std::to_string(max_bytes) + " reached";
                } else {
                    state->state = "running";
                }
                states.push_back(std::move(state));
            }
        }
        problems = outputSinkProblems(statesToValues(states));
    }
    // Never hold the sink mutex while another module takes its own.
    const bool published = replaceOutputProblems(problems);
    std::lock_guard lock{mutex};
    problems_need_sync = !published;
}

void OutputSinks::captureFrame(Renderer &renderer) {
    if (renderer.publishedLogicalFrameCount() == 0) return;
    // The file number is the just-published frame's zero-based position:
    // publishedLogicalFrameCount() - 1. In the ordinary headless fixture the
    // shader's frame_index starts at one, hence file n holds frame_index n+1.
    // Do not extend that relation across reloads or unpublished frames.
    const auto frame = renderer.publishedLogicalFrameCount() - 1;
    std::vector<std::pair<OutputSinkSpec, std::shared_ptr<State>>> work;
    bool is_active = false;
    bool sync_problems = false;
    std::filesystem::path output_directory;
    std::uint32_t frame_limit = 1000;
    std::uint64_t byte_limit = 0;
    std::vector<nlohmann::json> problems;
    {
        std::lock_guard lock{mutex};
        is_active = active;
        output_directory = directory;
        frame_limit = max_frames;
        byte_limit = max_bytes;
        sync_problems = problems_need_sync;
        if (!table || !is_active) {
            if (sync_problems)
                problems = outputSinkProblems(statesToValues(states));
        } else {
            work.reserve(states.size());
            for (std::size_t index = 0;
                 index < table->sinks.size() && index < states.size(); ++index) {
                work.emplace_back(table->sinks[index], states[index]);
            }
        }
    }
    if (!is_active) {
        if (sync_problems && replaceOutputProblems(problems)) {
            std::lock_guard lock{mutex};
            problems_need_sync = false;
        }
        return;
    }

    bool changed = false;
    for (auto &[sink, state] : work) {
        if (sink.every_n == 0 || frame % sink.every_n != 0) continue;
        {
            std::lock_guard lock{mutex};
            if (state->state == "finished" || state->state == "refused" ||
                state->state == "inactive")
                continue;
            if (frame_limit != 0 && state->frames_written >= frame_limit) {
                state->state = "finished";
                state->stop_reason = "max_frames";
                state->stop_value = frame_limit;
                changed = true;
                continue;
            }
            if (byte_limit != 0 && bytes_written >= byte_limit) {
                state->state = "finished";
                state->stop_reason = "max_bytes";
                state->stop_value = byte_limit;
                state->last_error = "output.limit_reached@1: max_bytes " +
                                    std::to_string(byte_limit) + " reached";
                changed = true;
                continue;
            }
        }
        try {
            const auto readback =
                renderer.readOutputRenderTargetForSink(sink.target, sink.graph);
            if (readback.status == OutputReadStatus::inactive) {
                std::lock_guard lock{mutex};
                ++frames_skipped_inactive;
                continue;
            }
            if (readback.status == OutputReadStatus::no_contents) {
                std::lock_guard lock{mutex};
                ++frames_skipped_no_contents;
                continue;
            }
            if (readback.status == OutputReadStatus::refused) {
                std::lock_guard lock{mutex};
                state->state = "refused";
                state->last_error = readback.error;
                changed = true;
                continue;
            }
            const auto format = outputFileFormat(readback.format);
            if (!format) {
                std::lock_guard lock{mutex};
                state->state = "refused";
                state->last_error = "output.format_unsupported@1: " +
                                    vk::to_string(readback.format);
                changed = true;
                continue;
            }
            std::optional<std::uint64_t> budget;
            {
                std::lock_guard lock{mutex};
                // The lock protects the run-wide counter while another sink
                // (or a reload) could change it. The writer still checks the
                // real encoded size, so this is only a fast path.
                if (byte_limit != 0)
                    budget = bytes_written < byte_limit
                                 ? byte_limit - bytes_written
                                 : 0;
            }
            const auto written = writeOutputFrameFile(
                output_directory, sink.name, frame,
                OutputFileImage{
                    .width = readback.extent.width,
                    .height = readback.extent.height,
                    .format = *format,
                    .pixels = readback.bytes,
                },
                sink.format == "png" ? OutputFileEncoding::png
                                     : OutputFileEncoding::raw,
                budget);
            std::lock_guard lock{mutex};
            if (written.status == OutputFileWriteStatus::written) {
                // The real encoded size, not the uncompressed readback size,
                // is what max_bytes bounds.
                bytes_written += written.bytes_written;
                ++state->frames_written;
                if (frame_limit != 0 && state->frames_written >= frame_limit) {
                    state->state = "finished";
                    state->stop_reason = "max_frames";
                    state->stop_value = frame_limit;
                    changed = true;
                }
            } else if (written.status == OutputFileWriteStatus::limit_reached) {
                state->state = "finished";
                state->stop_reason = "max_bytes";
                state->stop_value = byte_limit;
                state->last_error = written.message;
                changed = true;
            } else {
                state->state = "refused";
                state->last_error =
                    written.message.empty()
                        ? (written.status == OutputFileWriteStatus::exists
                               ? "output.exists@1"
                               : "output.write_failed@1")
                        : written.message;
                changed = true;
            }
        } catch (const vk::DeviceLostError &) {
            throw;
        } catch (const std::exception &error) {
            std::lock_guard lock{mutex};
            state->state = "refused";
            state->last_error = std::string{"output.read_failed@1: "} + error.what();
            changed = true;
        }
    }
    {
        std::lock_guard lock{mutex};
        // A sink that did not run this frame still cannot write once the
        // run's budget is exhausted. Stop it at the same limit so its state
        // and problem agree with the others.
        if (byte_limit != 0 && bytes_written >= byte_limit) {
            for (auto &state : states) {
                if (state->state != "running") continue;
                state->state = "finished";
                state->stop_reason = "max_bytes";
                state->stop_value = byte_limit;
                state->last_error = "output.limit_reached@1: max_bytes " +
                                    std::to_string(byte_limit) + " reached";
                changed = true;
            }
        }
        if (sync_problems || changed)
            problems = outputSinkProblems(statesToValues(states));
    }
    if (sync_problems || changed) {
        if (replaceOutputProblems(problems)) {
            std::lock_guard lock{mutex};
            problems_need_sync = false;
        }
    }
}

nlohmann::json OutputSinks::status() const {
    std::lock_guard lock{mutex};
    nlohmann::json sinks = nlohmann::json::array();
    for (const auto &state : states) {
        sinks.push_back({{"name", state->name},
                         {"kind", state->kind},
                         {"format", state->format},
                         {"state", state->state},
                         {"stop_reason", state->stop_reason},
                         {"stop_value", state->stop_value},
                         {"frames_written", state->frames_written},
                         {"last_error", state->last_error}});
    }
    return {{"active", active},
            {"sinks", std::move(sinks)},
            {"frames_skipped_inactive", frames_skipped_inactive},
            {"frames_skipped_no_contents", frames_skipped_no_contents}};
}

} // namespace Pelican
