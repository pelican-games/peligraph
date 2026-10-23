#pragma once

#include "renderconfigeditor.hpp"
#include <map>

namespace Pelican {

struct ProceduralAssetEditorDependencies {
    std::string reference;
    std::string normalized_reference;
    std::filesystem::path path;
    std::filesystem::path project_root;
    std::string source_bytes;
    std::function<RenderConfigEditorGateObservation()> gate;
    // Prepare privately, call source_commit as the last fallible action,
    // then publish the prepared runtime state without throwing.
    std::function<void(std::string_view, const std::function<void()> &)> apply;
    // MI-1: whether asset_data bytes are what the engine has published now
    // (another writer -- register_model, create_material, bind_material --
    // wrote and published them). Such bytes become this editor's source in
    // place of the ones it holds. Empty: never.
    std::function<bool(std::string_view)> source_published;
};

class ProceduralAssetEditorService {
    using Json = nlohmann::json;
    using Result = nlohmann::ordered_json;
    struct Pending {
        std::string ticket, base_digest, bytes;
        std::uint64_t accepted_gate_epoch;
    };
    ProceduralAssetEditorDependencies dependencies_;
    EditorGateEpoch<RenderConfigEditorGateObservation> gate_tracker_;
    std::vector<Pending> pending_;
    std::map<std::string, Result> results_;
    std::vector<Result> completed_;
    std::uint64_t next_ticket_ = 1;
    void adoptPublishedSource();
  public:
    explicit ProceduralAssetEditorService(ProceduralAssetEditorDependencies dependencies);
    Result get(const Json &params);
    Result edit(const Json &params);
    Result result(const Json &params) const;
    void commitPending() noexcept;
    std::vector<Result> takeCompletedResults();
};

} // namespace Pelican
