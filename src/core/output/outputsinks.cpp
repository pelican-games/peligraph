#include "outputsinks.hpp"

#include "../launchconfig.hpp"
#include "../vkcore/renderer.hpp"

#include <stdexcept>

namespace Pelican {
namespace {

std::string sinkError(const OutputSinkSpec &sink) {
    if (sink.error_code.empty()) return {};
    if (sink.error_message.empty()) return sink.error_code;
    return sink.error_code + ": " + sink.error_message;
}

bool sameIdentity(const OutputSinks::State &state, const OutputSinkSpec &sink) {
    return state.name == sink.name && state.kind == sink.kind &&
           state.graph == sink.graph && state.target == sink.target;
}

} // namespace

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
    std::lock_guard lock{mutex};
    std::vector<std::shared_ptr<State>> old = std::move(states);
    states.clear();
    table = std::move(next);
    if (!table) return;
    states.reserve(table->sinks.size());
    for (const auto &sink : table->sinks) {
        auto state = std::make_shared<State>();
        state->name = sink.name;
        state->kind = sink.kind;
        state->graph = sink.graph;
        state->target = sink.target;
        for (const auto &previous : old) {
            if (sameIdentity(*previous, sink)) {
                // A reload keeps the worked counters of the same sink and
                // starts its state afresh: a refusal or a finished run is a
                // property of the generation that produced it.
                state->frames_written = previous->frames_written;
                break;
            }
        }
        if (!sink.error_code.empty()) {
            state->state = "refused";
            state->last_error = sinkError(sink);
        } else if (!active) {
            state->state = "inactive";
        } else if (max_frames != 0 && state->frames_written >= max_frames) {
            state->state = "finished";
        } else {
            state->state = "running";
        }
        states.push_back(std::move(state));
    }
}

void OutputSinks::captureFrame(Renderer &renderer) {
    std::vector<std::pair<OutputSinkSpec, std::shared_ptr<State>>> work;
    // The published frame is the engine's zero-based logical frame. Its own
    // count, not a sink-local counter, keeps numbering on the engine's frame
    // when a frame never reaches publication or the table changes.
    if (renderer.publishedLogicalFrameCount() == 0) return;
    const auto frame = renderer.publishedLogicalFrameCount() - 1;
    bool is_active = false;
    std::filesystem::path output_directory;
    std::uint32_t frame_limit = 1000;
    std::uint64_t byte_limit = 0;
    {
        std::lock_guard lock{mutex};
        is_active = active;
        output_directory = directory;
        frame_limit = max_frames;
        byte_limit = max_bytes;
        if (!table || !is_active) return;
        work.reserve(states.size());
        for (std::size_t index = 0;
             index < table->sinks.size() && index < states.size(); ++index) {
            work.emplace_back(table->sinks[index], states[index]);
        }
    }
    if (!is_active) return;

    for (auto &[sink, state] : work) {
        if (sink.every_n == 0 || frame % sink.every_n != 0) continue;
        {
            std::lock_guard lock{mutex};
            if (state->state == "finished" || state->state == "refused" ||
                state->state == "inactive")
                continue;
            if (frame_limit != 0 && state->frames_written >= frame_limit) {
                state->state = "finished";
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
                continue;
            }
            const auto format = outputFileFormat(readback.format);
            if (!format) {
                std::lock_guard lock{mutex};
                state->state = "refused";
                state->last_error = "output.format_unsupported@1: " +
                                    vk::to_string(readback.format);
                continue;
            }
            const auto byte_count =
                static_cast<std::uint64_t>(output_file_header_bytes) +
                static_cast<std::uint64_t>(readback.bytes.size());
            {
                std::lock_guard lock{mutex};
                if (byte_limit != 0 &&
                    (bytes_written >= byte_limit ||
                     byte_count > byte_limit - bytes_written)) {
                    state->state = "finished";
                    state->last_error = "output.byte_limit@1";
                    continue;
                }
            }
            const auto written = writeOutputFrameFile(
                output_directory, sink.name, frame,
                OutputFileImage{
                    .width = readback.extent.width,
                    .height = readback.extent.height,
                    .format = *format,
                    .pixels = readback.bytes,
                });
            std::lock_guard lock{mutex};
            if (written.status == OutputFileWriteStatus::written) {
                bytes_written += byte_count;
                ++state->frames_written;
                if (frame_limit != 0 && state->frames_written >= frame_limit)
                    state->state = "finished";
            } else {
                state->state = "refused";
                state->last_error =
                    written.message.empty()
                        ? (written.status == OutputFileWriteStatus::exists
                               ? "output.exists@1"
                               : "output.write_failed@1")
                        : written.message;
            }
        } catch (const vk::DeviceLostError &) {
            throw;
        } catch (const std::exception &error) {
            std::lock_guard lock{mutex};
            state->state = "refused";
            state->last_error = std::string{"output.read_failed@1: "} + error.what();
        }
    }
    {
        std::lock_guard lock{mutex};
        if (byte_limit != 0 && bytes_written >= byte_limit) {
            for (auto &state : states) {
                if (state->state != "running") continue;
                state->state = "finished";
                state->last_error = "output.byte_limit@1";
            }
        }
    }
}

nlohmann::json OutputSinks::status() const {
    std::lock_guard lock{mutex};
    nlohmann::json sinks = nlohmann::json::array();
    for (const auto &state : states) {
        sinks.push_back({{"name", state->name},
                         {"kind", state->kind},
                         {"state", state->state},
                         {"frames_written", state->frames_written},
                         {"last_error", state->last_error}});
    }
    return {{"active", active},
            {"sinks", std::move(sinks)},
            {"frames_skipped_inactive", frames_skipped_inactive},
            {"frames_skipped_no_contents", frames_skipped_no_contents}};
}

} // namespace Pelican
