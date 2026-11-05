#include "renderconfigeditor.hpp"

#include "../os/liveinputrecordingbake.hpp"

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view liveBakeBusyCode = "live_input.bake_busy@1";

std::string requiredLiveString(const Json &params, const char *field) {
    const auto found = params.find(field);
    if (found == params.end() || !found->is_string() || found->get_ref<const std::string &>().empty())
        throw std::invalid_argument(std::string{"bake_live_recording requires a non-empty string '"} + field + "'");
    return found->get<std::string>();
}

bool pathEscapes(const std::filesystem::path &path) {
    return std::any_of(path.begin(), path.end(), [](const auto &part) { return part == ".."; });
}

void checkLiveOutputPath(const std::filesystem::path &root, const std::string &out,
                         const std::filesystem::path &out_path,
                         const std::filesystem::path &recording) {
    const std::filesystem::path spelled{out};
    if (spelled.is_absolute() || pathEscapes(spelled))
        throw std::invalid_argument("bake_live_recording out must stay inside the project: " + out);
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    const auto canonical_parent = std::filesystem::weakly_canonical(out_path.parent_path(), error);
    if (error) throw std::invalid_argument("bake_live_recording cannot resolve out: " + out);
    const auto relative = std::filesystem::relative(canonical_parent, canonical_root, error);
    if (error || pathEscapes(relative) || std::filesystem::is_symlink(out_path, error))
        throw std::invalid_argument("bake_live_recording out must not escape the project through a symlink: " + out);
    const auto manifest = derivedAssetManifestPath(out_path);
    if (out_path.lexically_normal() == recording.lexically_normal() ||
        manifest.lexically_normal() == recording.lexically_normal() ||
        out_path.lexically_normal() == derivedAssetManifestPath(recording).lexically_normal())
        throw std::invalid_argument("bake_live_recording out collides with the recording or its manifest: " + out);
}

} // namespace

OrderedJson RenderConfigEditorService::bakeLiveRecording(const Json &params) {
    if (!params.is_object()) throw std::invalid_argument("bake_live_recording params must be an object");
    for (const auto &[key, value] : params.items()) {
        (void)value;
        if (key != "recording" && key != "out" && key != "rate" && key != "inputs")
            throw std::invalid_argument("bake_live_recording contains unsupported field '" + key + "'");
    }
    const auto recording = requiredLiveString(params, "recording");
    const auto out = requiredLiveString(params, "out");
    if (!out.ends_with(".ktx2")) throw std::invalid_argument("bake_live_recording out must name a .ktx2 file: " + out);
    double rate = 0.0;
    if (const auto found = params.find("rate"); found != params.end()) {
        if (!found->is_number()) throw std::invalid_argument("bake_live_recording rate must be a number");
        rate = found->get<double>();
        if (!std::isfinite(rate) || rate < 1.0 || rate > 1000.0)
            throw std::invalid_argument("bake_live_recording rate must be finite and between 1 and 1000");
    }
    std::optional<std::vector<std::string>> inputs;
    if (const auto found = params.find("inputs"); found != params.end()) {
        if (!found->is_array()) throw std::invalid_argument("bake_live_recording inputs must be an array");
        inputs.emplace();
        for (const auto &name : *found) {
            if (!name.is_string()) throw std::invalid_argument("bake_live_recording inputs must contain strings");
            inputs->push_back(name.get<std::string>());
        }
    }

    const auto resolve = [this](const std::string &reference) {
        if (dependencies_.resolve_document_path) return dependencies_.resolve_document_path(reference);
        return (dependencies_.project_root / reference).lexically_normal();
    };
    const auto source_path = resolve(recording);
    const auto out_path = resolve(out);
    checkLiveOutputPath(dependencies_.project_root, out, out_path, source_path);
    for (const auto &task : audio_bakes_)
        if (task.out == out_path)
            return rejection(std::string{liveBakeBusyCode}, "an audio analysis bake writing " + out + " is running",
                             {{"out", out}, {"ticket", task.ticket}});
    for (const auto &task : live_bakes_)
        if (task.out == out_path)
            return rejection(std::string{liveBakeBusyCode}, "a bake_live_recording writing " + out + " is running",
                             {{"out", out}, {"ticket", task.ticket}});

    const auto ticket_id = "render-config-edit-" + std::to_string(next_ticket_++);
    live_bakes_.push_back(AudioBakeTask{
        .ticket = ticket_id,
        .out = out_path,
        .outcome = std::async(std::launch::async,
            [recording, out, source_path, out_path, rate, inputs]() -> AudioAnalysisBakeOutcome {
                try {
                    const auto baked = Pelican::bakeLiveRecording(source_path, out_path, rate, inputs);
                    return {.committed = true,
                            .fields = {{"recording", recording},
                                       {"out", out},
                                       {"manifest", out + ".import.json"},
                                       {"layout", sampleTableLayoutJson(baked.layout)},
                                       {"features", baked.layout.features},
                                       {"rows", baked.rows},
                                       {"sample_count", baked.layout.sample_count},
                                       {"hop_seconds", baked.layout.hop_seconds},
                                       {"t0_seconds", baked.t0_seconds},
                                       {"interpolation", "hold"},
                                       {"approximation", "fixed grid; a change between grid points arrives up to one hop late"}}};
                } catch (const GraphRegistryError &error) {
                    auto fields = OrderedJson::parse(error.detail().dump());
                    fields["recording"] = recording;
                    fields["out"] = out;
                    return {.committed = false, .fields = std::move(fields), .code = error.code(), .message = error.what()};
                } catch (const std::exception &error) {
                    return {.committed = false,
                            .fields = {{"recording", recording}, {"out", out}, {"message", error.what()}},
                            .code = std::string{liveRecordingBakeFailedCode}, .message = error.what()};
                }
            })});
    results_[ticket_id] = {{"ticket", ticket_id}, {"status", "running"}, {"operation", "bake_live_recording"},
                           {"recording", recording}, {"out", out}};
    return {{"ticket", ticket_id}, {"status", "accepted"}, {"recording", recording}, {"out", out}};
}

} // namespace Pelican

namespace Pelican {

void RenderConfigEditorService::harvestLiveBakes() noexcept {
    for (auto task = live_bakes_.begin(); task != live_bakes_.end();) {
        if (task->outcome.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            ++task;
            continue;
        }
        auto done = std::move(*task);
        task = live_bakes_.erase(task);
        OrderedJson result;
        try {
            auto outcome = done.outcome.get();
            if (outcome.committed) {
                result = {{"ticket", done.ticket},
                          {"status", "committed"},
                          {"committed", true},
                          {"operation", "bake_live_recording"}};
                for (auto &[key, value] : outcome.fields.items()) result[key] = std::move(value);
            } else {
                outcome.fields["ticket"] = done.ticket;
                result = rejection(outcome.code, outcome.message, std::move(outcome.fields));
            }
        } catch (const std::exception &error) {
            result = rejection(std::string{liveRecordingBakeFailedCode}, error.what(), {{"ticket", done.ticket}});
        } catch (...) {
            result = rejection(std::string{liveRecordingBakeFailedCode}, "unknown bake_live_recording failure",
                               {{"ticket", done.ticket}});
        }
        if (!result.contains("ticket")) result["ticket"] = done.ticket;
        results_[done.ticket] = result;
        completed_.push_back(std::move(result));
    }
}

} // namespace Pelican
