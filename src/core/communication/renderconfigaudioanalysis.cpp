// AB-1: bake_audio_analysis and list_derived_assets on the render config
// editor's tickets (the same tickets and the same get_edit_result as
// verify_fold's: running on a worker, committed or rejected at a frame
// boundary).
#include "renderconfigeditor.hpp"

#include "../../project/audioanalysisformat.hpp"
#include "../../project/graphcomposition.hpp"
#if PELICAN_WITH_AUDIO
#include "../audio/audioanalysis.hpp"
#endif

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view audioAnalysisBusyCode = "audio.analysis_busy@1";
constexpr std::string_view audioAnalysisUnavailableCode = "audio.analysis_unavailable@1";

std::string requiredString(const Json &params, const char *field) {
    const auto found = params.find(field);
    if (found == params.end() || !found->is_string() || found->get_ref<const std::string &>().empty())
        throw std::invalid_argument(std::string{"bake_audio_analysis requires a non-empty string '"} + field + "'");
    return found->get<std::string>();
}

} // namespace

OrderedJson RenderConfigEditorService::bakeAudioAnalysis(const Json &params) {
    constexpr auto method = "bake_audio_analysis";
    if (!params.is_object()) throw std::invalid_argument("bake_audio_analysis params must be an object");
    for (const auto &[key, value] : params.items()) {
        (void)value;
        if (key != "asset" && key != "out" && key != "settings")
            throw std::invalid_argument("bake_audio_analysis contains unsupported field '" + key + "'");
    }
    const auto asset = requiredString(params, "asset");
    const auto out = requiredString(params, "out");
    if (!out.ends_with(".ktx2")) throw std::invalid_argument("bake_audio_analysis out must name a .ktx2 file: " + out);
    AudioAnalysisSettings settings;
    try {
        settings = parseAudioAnalysisSettings(params.value("settings", Json{}));
    } catch (const GraphRegistryError &error) {
        auto fields = OrderedJson::parse(error.detail().dump());
        return rejection(error.code(), error.what(), std::move(fields));
    }
#if !PELICAN_WITH_AUDIO
    (void)settings;
    (void)method;
    return rejection(std::string{audioAnalysisUnavailableCode}, "this engine was built without audio",
                     {{"reason", "built_without_audio"}});
#else
    const auto resolve = [this](const std::string &reference) {
        if (dependencies_.resolve_document_path) return dependencies_.resolve_document_path(reference);
        return (dependencies_.project_root / reference).lexically_normal();
    };
    const auto source_path = resolve(asset);
    const auto out_path = resolve(out);
    for (const auto &task : audio_bakes_)
        if (task.out == out_path)
            return rejection(std::string{audioAnalysisBusyCode}, "a bake_audio_analysis writing " + out + " is running",
                             {{"out", out}, {"ticket", task.ticket}});

    const auto ticket_id = "render-config-edit-" + std::to_string(next_ticket_++);
    audio_bakes_.push_back(AudioBakeTask{
        .ticket = ticket_id,
        .out = out_path,
        // Its own thread: the frame and the RPCs go on while it decodes and
        // writes. It reads nothing of the engine's.
        .outcome = std::async(std::launch::async,
                              [asset, out, source_path, out_path, settings]() -> AudioAnalysisBakeOutcome {
                                  try {
                                      const auto baked = Pelican::bakeAudioAnalysis(source_path, out_path, settings);
                                      return {.committed = true,
                                              .fields = {{"asset", asset},
                                                         {"out", out},
                                                         {"manifest", out + ".import.json"},
                                                         {"layout", baked.layout}}};
                                  } catch (const GraphRegistryError &error) {
                                      auto fields = OrderedJson::parse(error.detail().dump());
                                      fields["asset"] = asset;
                                      fields["out"] = out;
                                      // AB-2: output_locked names the locked file as the
                                      // request named it.
                                      if (const auto path = fields.find("path"); path != fields.end()) {
                                          if (*path == out_path.generic_string()) *path = out;
                                          else if (*path == derivedAssetManifestPath(out_path).generic_string())
                                              *path = out + ".import.json";
                                      }
                                      return {.committed = false, .fields = std::move(fields), .code = error.code(),
                                              .message = error.what()};
                                  } catch (const std::exception &error) {
                                      return {.committed = false,
                                              .fields = {{"asset", asset}, {"out", out}, {"message", error.what()}},
                                              .code = std::string{audioAnalysisFailedCode},
                                              .message = error.what()};
                                  }
                              }),
    });
    results_[ticket_id] = {{"ticket", ticket_id}, {"status", "running"}, {"operation", method},
                           {"asset", asset}, {"out", out}};
    return {{"ticket", ticket_id}, {"status", "accepted"}, {"asset", asset}, {"out", out}};
#endif
}

void RenderConfigEditorService::harvestAudioBakes() noexcept {
    for (auto task = audio_bakes_.begin(); task != audio_bakes_.end();) {
        if (task->outcome.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            ++task;
            continue;
        }
        auto done = std::move(*task);
        task = audio_bakes_.erase(task);
        OrderedJson result;
        try {
            auto outcome = done.outcome.get();
            if (outcome.committed) {
                result = {{"ticket", done.ticket},
                          {"status", "committed"},
                          {"committed", true},
                          {"operation", "bake_audio_analysis"}};
                for (auto &[key, value] : outcome.fields.items()) result[key] = std::move(value);
            } else {
                outcome.fields["ticket"] = done.ticket;
                result = rejection(outcome.code, outcome.message, std::move(outcome.fields));
            }
        } catch (const std::exception &error) {
            result = rejection(std::string{audioAnalysisFailedCode}, error.what(), {{"ticket", done.ticket}});
        } catch (...) {
            result = rejection(std::string{audioAnalysisFailedCode}, "unknown bake_audio_analysis failure",
                               {{"ticket", done.ticket}});
        }
        if (!result.contains("ticket")) result["ticket"] = done.ticket;
        results_[done.ticket] = result;
        completed_.push_back(std::move(result));
    }
}

OrderedJson RenderConfigEditorService::listDerivedAssets(const Json &params) const {
    if (!params.is_null() && (!params.is_object() || !params.empty()))
        throw std::invalid_argument("list_derived_assets takes no parameters");
    std::vector<std::filesystem::path> roots;
    if (dependencies_.asset_stores) {
        for (const auto &store : dependencies_.asset_stores()) roots.push_back(store.root);
    }
    if (roots.empty()) roots.push_back(dependencies_.project_root);
    auto result = OrderedJson::array();
    for (const auto &status : findDerivedAssets(roots))
        result.push_back(derivedAssetJson(status, dependencies_.project_root));
    return result;
}

} // namespace Pelican
