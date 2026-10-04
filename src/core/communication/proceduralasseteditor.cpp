#include "proceduralasseteditor.hpp"
#include "../../project/assetdataformat.hpp"
#include <algorithm>
#include <fstream>
#include <iterator>

namespace Pelican {
namespace {
std::string readAssetSource(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read asset document: " + path.string());
    return {std::istreambuf_iterator<char>{input}, {}};
}
nlohmann::ordered_json rejectAsset(std::string code, std::string message) {
    return {{"status", "rejected"}, {"error", {{"code", std::move(code)}, {"message", std::move(message)}}}};
}
}

ProceduralAssetEditorService::ProceduralAssetEditorService(ProceduralAssetEditorDependencies dependencies)
    : dependencies_(std::move(dependencies)) {
    if (!dependencies_.gate || !dependencies_.apply)
        throw std::invalid_argument("Procedural asset editing requires gate and runtime publication");
}

void ProceduralAssetEditorService::adoptPublishedSource() {
    if (!dependencies_.source_published) return;
    std::string bytes;
    try {
        bytes = readAssetSource(dependencies_.path);
    } catch (const std::exception &) {
        return;
    }
    if (bytes != dependencies_.source_bytes && dependencies_.source_published(bytes))
        dependencies_.source_bytes = std::move(bytes);
}

ProceduralAssetEditorService::Result ProceduralAssetEditorService::get(const Json &params) {
    if (!params.is_object() || !params.empty()) throw std::invalid_argument("get_procedural_assets takes no parameters");
    adoptPublishedSource();
    const auto source = Json::parse(dependencies_.source_bytes);
    const auto gate = dependencies_.gate();
    const auto digest = renderConfigSourceDigest(dependencies_.source_bytes);
    const bool current = renderConfigSourceDigest(readAssetSource(dependencies_.path)) == digest;
    const auto assets = source.value("procedural_geometry", Json::array());
    return {{"source_reference", dependencies_.reference}, {"source_digest", {{"algorithm", "sha256"}, {"hex", digest}}},
            {"editable", gate.can_edit && current}, {"external_modification", !current},
            {"assets", assets}, {"assets_json", assets.dump(2)}};
}

ProceduralAssetEditorService::Result ProceduralAssetEditorService::edit(const Json &params) {
    if (!params.is_object()) throw std::invalid_argument("edit_procedural_asset requires an object");
    for (const auto &[key, value] : params.items()) {
        if (key != "base_source_digest" && key != "asset_name" && key != "aabb" && key != "record" && key != "operation")
            throw std::invalid_argument("Unknown procedural asset edit field: " + key);
    }
    const auto digest = params.at("base_source_digest").get<std::string>();
    const auto name = params.at("asset_name").get<std::string>();
    const auto operation = params.value("operation", std::string{"update"});
    if (operation != "update" && operation != "create" && operation != "delete")
        throw std::invalid_argument("Procedural asset operation must be update, create or delete");
    if (operation == "delete" && (params.contains("aabb") || params.contains("record")))
        throw std::invalid_argument("Deleting a procedural asset does not accept aabb or record");
    if (operation == "create" && (!params.contains("aabb") || !params.contains("record")))
        throw std::invalid_argument("Creating a procedural asset requires aabb and record");
    if (operation != "delete" && !params.contains("aabb") && !params.contains("record"))
        throw std::invalid_argument("Provide aabb or record to edit");
    const auto gate = dependencies_.gate();
    const auto gate_epoch = gate_tracker_.observe(gate);
    if (!gate.can_edit) return rejectAsset("gate_closed", "Asset editing is currently disabled");
    adoptPublishedSource();
    if (digest != renderConfigSourceDigest(dependencies_.source_bytes) ||
        digest != renderConfigSourceDigest(readAssetSource(dependencies_.path)))
        return rejectAsset("external_modification", "Asset source changed; reload before editing");
    auto candidate = nlohmann::ordered_json::parse(dependencies_.source_bytes);
    if (operation == "create" && !candidate.contains("procedural_geometry"))
        candidate["procedural_geometry"] = nlohmann::ordered_json::array();
    if (!candidate.contains("procedural_geometry"))
        return rejectAsset("asset_not_found", "Unknown procedural asset: " + name);
    auto &assets = candidate.at("procedural_geometry");
    auto found = std::find_if(assets.begin(), assets.end(), [&](const auto &asset) {
        return asset.at("name").template get<std::string>() == name;
    });
    if (operation == "create") {
        if (found != assets.end()) return rejectAsset("asset_already_exists", "Procedural asset already exists: " + name);
        assets.push_back({{"name", name}, {"kind", "aabb_v1"},
                          {"aabb", params.at("aabb")}, {"record", params.at("record")}});
    } else {
        if (found == assets.end()) return rejectAsset("asset_not_found", "Unknown procedural asset: " + name);
        if (operation == "delete") assets.erase(found);
        else for (const auto *field : {"aabb", "record"})
            if (params.contains(field)) (*found)[field] = params.at(field);
    }
    (void)parseAssetDataFormatJson(Json(candidate));
    const auto ticket = "procedural-asset-edit-" + std::to_string(next_ticket_++);
    pending_.push_back({ticket, digest, candidate.dump(2) + "\n", gate_epoch});
    results_[ticket] = {{"ticket", ticket}, {"status", "pending"}, {"committed", false}};
    return {{"status", "accepted"}, {"ticket", ticket}};
}

void ProceduralAssetEditorService::commitPending() noexcept {
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto &edit : pending) {
        Result outcome = {{"ticket", edit.ticket}, {"status", "rejected"}, {"committed", false}};
        try {
            const auto gate = dependencies_.gate();
            if (!gate.can_edit || gate_tracker_.observe(gate) != edit.accepted_gate_epoch)
                throw std::runtime_error("Asset editing gate changed before publication");
            if (renderConfigSourceDigest(dependencies_.source_bytes) != edit.base_digest)
                throw RenderConfigExternalModification("An earlier asset edit changed this document");
            const RenderConfigCandidateDocumentSet documents{dependencies_.normalized_reference, {{
                .reference = dependencies_.reference,
                .normalized_reference = dependencies_.normalized_reference,
                .path = dependencies_.path,
                .operation = RenderConfigDocumentOperation::replace,
                .expected = {RenderConfigDocumentExistence::present, edit.base_digest},
                .bytes = edit.bytes}}};
            dependencies_.apply(edit.bytes, [&] {
                (void)commitRenderConfigCandidateDocuments(dependencies_.project_root, documents);
            });
            dependencies_.source_bytes.swap(edit.bytes);
            outcome["committed"] = true;
            outcome["status"] = "committed";
            outcome["source_digest"] = {{"algorithm", "sha256"}, {"hex", renderConfigSourceDigest(dependencies_.source_bytes)}};
        } catch (const RenderConfigExternalModification &error) {
            outcome["error"] = {{"code", "external_modification"}, {"message", error.what()}};
        } catch (const std::exception &error) {
            outcome["error"] = {{"code", "asset_edit_failed"}, {"message", error.what()}};
        }
        results_[edit.ticket] = outcome;
        completed_.push_back(std::move(outcome));
    }
}

ProceduralAssetEditorService::Result ProceduralAssetEditorService::result(const Json &params) const {
    const auto ticket = params.at("ticket").get<std::string>();
    const auto found = results_.find(ticket);
    if (found == results_.end()) throw std::invalid_argument("Unknown asset edit ticket: " + ticket);
    return found->second;
}

std::vector<ProceduralAssetEditorService::Result> ProceduralAssetEditorService::takeCompletedResults() {
    auto result = std::move(completed_);
    completed_.clear();
    return result;
}
} // namespace Pelican
