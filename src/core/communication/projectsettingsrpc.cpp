#include "projectsettingsrpc.hpp"

#include "rpcserver.hpp"

#include "../loader/assetsverification.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../watch/reloadservice.hpp"

#include "../../project/projectdocumenttransaction.hpp"
#include "../../project/projectformat.hpp"
#include "../../project/projectpathresolver.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view editMethod = "edit_project_settings";
constexpr std::string_view readMethod = "get_project_settings";

[[noreturn]] void refuse(const std::string &message, std::string_view code, Json fields) {
    throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError, message,
                              Json{{"error_code", std::string{code}}, {"error_fields", std::move(fields)}});
}

[[noreturn]] void invalidParams(const std::string &message) {
    throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams, message);
}

std::string readProjectJson(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("cannot read " + path.generic_string());
    }
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

Json settingValue(const OrderedJson &project, std::string_view key) {
    const auto found = project.find(std::string{key});
    if (found == project.end()) return nullptr;
    return Json::parse(found->dump());
}

Json settingsJson(const OrderedJson &project) {
    return {{"hot_reload", settingValue(project, "hot_reload")},
            {"asset_stores", settingValue(project, "asset_stores")}};
}

// project.json as the engine writes it: the keys in the order the file had
// them (a new key last), two spaces, and the file's own line ending.
std::string renderProjectJson(const OrderedJson &project, std::string_view previous) {
    auto text = project.dump(2);
    if (previous.ends_with('\n')) text += '\n';
    if (previous.find("\r\n") == std::string_view::npos) return text;
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 16);
    for (const char ch : text) {
        if (ch == '\n') crlf += '\r';
        crlf += ch;
    }
    return crlf;
}

Json namesJson(const std::vector<std::string> &names) { return Json(names); }

void requireOnly(const Json &params, std::span<const std::string_view> keys) {
    for (const auto &[key, value] : params.items()) {
        (void)value;
        bool known = false;
        for (const auto allowed : keys) known = known || key == allowed;
        if (!known) invalidParams(std::string{editMethod} + " does not take '" + key + "'");
    }
}

Json readSettings(const PathResolver &path_resolver) {
    const auto bytes = readProjectJson(path_resolver.projectRoot() / "project.json");
    auto answer = settingsJson(OrderedJson::parse(bytes));
    answer["project_digest"] = projectDocumentDigest(bytes);
    return answer;
}

Json editSettings(const Json &params, PathResolver &path_resolver, watch::ReloadService *reload_service,
                  AssetsVerification *assets_verification) {
    if (!params.is_object()) invalidParams(std::string{editMethod} + " params must be an object");
    static constexpr std::array<std::string_view, 3> keys{"base_project_digest", "hot_reload", "asset_stores"};
    requireOnly(params, keys);
    const auto base = params.find("base_project_digest");
    if (base == params.end() || !base->is_string()) {
        invalidParams(std::string{editMethod} +
                      " requires string 'base_project_digest' (get_project_settings.project_digest)");
    }
    const auto hot_reload = params.find("hot_reload");
    if (hot_reload != params.end() && !hot_reload->is_null() && !hot_reload->is_array()) {
        invalidParams(std::string{editMethod} + " 'hot_reload' must be an array of paths or null");
    }
    const auto asset_stores = params.find("asset_stores");
    if (asset_stores != params.end() && !asset_stores->is_null() && !asset_stores->is_object()) {
        invalidParams(std::string{editMethod} + " 'asset_stores' must be an object or null");
    }

    const auto project_root = path_resolver.projectRoot();
    const auto project_path = project_root / "project.json";
    const auto previous_bytes = readProjectJson(project_path);
    const auto previous_digest = projectDocumentDigest(previous_bytes);
    const auto &expected = base->get_ref<const std::string &>();
    if (expected != previous_digest) {
        refuse(std::string{editMethod} + ": project.json changed since it was read (expected sha256 " +
                   expected + ", found " + previous_digest + ")",
               projectSettingsExternalModificationCode, {{"expected", expected}, {"actual", previous_digest}});
    }

    const auto previous_project = OrderedJson::parse(previous_bytes);
    auto project = previous_project;
    if (hot_reload != params.end()) {
        if (hot_reload->is_null()) {
            project.erase("hot_reload");
        } else {
            std::optional<std::vector<std::string>> normalized;
            try {
                normalized = parseProjectHotReload(Json{{"hot_reload", *hot_reload}});
            } catch (const std::exception &error) {
                refuse(std::string{editMethod} + ": " + error.what(), projectHotReloadInvalidCode,
                       {{"key", "hot_reload"}, {"reason", error.what()}});
            }
            project["hot_reload"] = *normalized;
        }
    }
    if (asset_stores != params.end()) {
        if (asset_stores->is_null()) {
            project.erase("asset_stores");
        } else {
            project["asset_stores"] = OrderedJson::parse(asset_stores->dump());
        }
    }

    // The whole document as the engine would open it next time (the engine
    // version is not this edit's business).
    ProjectEnvelope envelope;
    try {
        envelope = parseProjectEnvelopeJson(Json::parse(project.dump()), {.ignore_engine_version = true}).envelope;
    } catch (const std::exception &error) {
        refuse(std::string{editMethod} + ": " + error.what(), projectSettingsInvalidCode,
               {{"key", "project"}, {"reason", error.what()}});
    }
    AssetStoresChange change;
    try {
        change = path_resolver.planDeclarations(envelope.asset_stores);
    } catch (const AssetStoresChangeRefused &error) {
        Json fields{{"key", "asset_stores"}, {"reason", error.reason()}};
        if (!error.store().empty()) fields["store"] = error.store();
        refuse(std::string{editMethod} + ": " + error.what(), error.code(), std::move(fields));
    }

    Json answer{{"committed", true},
                {"previous_project_digest", previous_digest},
                {"previous", settingsJson(previous_project)},
                {"applied", settingsJson(project)},
                {"asset_stores_change",
                 {{"added", namesJson(change.added_stores)},
                  {"removed", namesJson(change.removed_stores)},
                  {"manifests_declared", namesJson(change.declared_manifests)},
                  {"manifests_removed", namesJson(change.removed_manifests)}}},
                {"assets_verification", nullptr},
                {"restart_required", nullptr}};
    const bool hot_reload_changed = envelope.hot_reload != path_resolver.hotReloadPaths();
    if (project == previous_project && change.empty() && !hot_reload_changed) {
        answer["changed"] = false;
        answer["project_digest"] = previous_digest;
        return answer;
    }

    const auto bytes = renderProjectJson(project, previous_bytes);
    const std::array documents{ProjectCandidateDocument{
        .reference = "project.json",
        .normalized_reference = "project.json",
        .path = project_path,
        .operation = ProjectDocumentOperation::replace,
        .expected = {ProjectDocumentExistence::present, previous_digest},
        .bytes = bytes,
    }};
    try {
        (void)commitProjectDocuments(project_root, documents);
    } catch (const ProjectDocumentExternalModification &error) {
        refuse(std::string{editMethod} + ": " + error.what(), projectSettingsExternalModificationCode,
               {{"expected", expected}, {"actual", nullptr}});
    } catch (const ProjectDocumentTransactionBusy &error) {
        refuse(std::string{editMethod} + ": " + error.what(), projectSettingsTransactionBusyCode,
               {{"path", error.path().generic_string()}});
    }
    answer["changed"] = true;
    answer["project_digest"] = projectDocumentDigest(bytes);
    LOG_INFO(logger, "project.json settings edited in place (hot_reload, asset_stores)");

    // project.json is written; from here on a part that cannot be applied in
    // this process is answered as restart_required, never as a refusal.
    std::vector<std::string> restart_reasons;
    try {
        change = path_resolver.replaceDeclarations(envelope.asset_stores, envelope.hot_reload);
    } catch (const std::exception &error) {
        restart_reasons.push_back(std::string{"the engine could not apply the new declarations: "} +
                                  error.what());
    }
    if (restart_reasons.empty()) {
        if (reload_service != nullptr) {
            try {
                reload_service->setup(path_resolver);
            } catch (const std::exception &error) {
                restart_reasons.push_back(std::string{"the file watch could not be replaced: "} + error.what());
            }
        } else if (hot_reload_changed) {
            restart_reasons.push_back("this host has no file watch; hot_reload applies when the engine starts");
        }
        if (assets_verification != nullptr) {
            try {
                answer["assets_verification"] =
                    assets_verification->redeclareStores(path_resolver.stores(), change.declared_manifests);
            } catch (const AssetsVerificationError &error) {
                if (!change.declared_manifests.empty()) {
                    restart_reasons.push_back(std::string{"the asset verification could not start: "} +
                                              error.what());
                }
            }
        } else if (!change.declared_manifests.empty()) {
            restart_reasons.push_back("this host does not verify assets; the manifest is verified when the engine "
                                      "starts");
        }
    }
    if (!restart_reasons.empty()) {
        std::string reason;
        for (const auto &part : restart_reasons) {
            if (!reason.empty()) reason += "; ";
            reason += part;
        }
        LOG_WARNING(logger, "project.json settings edited; restart required: {}", reason);
        answer["restart_required"] = {{"reason", reason}};
    }
    return answer;
}

} // namespace

void configureProjectSettingsRpcHandlers(RpcServer &server, PathResolver &path_resolver,
                                         watch::ReloadService *reload_service,
                                         AssetsVerification *assets_verification) {
    server.setHandler(std::string{readMethod}, [&path_resolver](const Json &params) {
        if (!params.is_null() && !(params.is_object() && params.empty())) {
            invalidParams(std::string{readMethod} + " takes no parameters");
        }
        return readSettings(path_resolver);
    });
    server.setHandler(std::string{editMethod},
                      [&path_resolver, reload_service, assets_verification](const Json &params) {
                          return editSettings(params, path_resolver, reload_service, assets_verification);
                      });
}

} // namespace Pelican
