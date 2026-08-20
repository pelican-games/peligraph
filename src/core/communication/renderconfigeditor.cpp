#include "renderconfigeditor.hpp"

#include "../loader/engineresources.hpp"
#include "../vkcore/renderer_config.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <ranges>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

std::atomic<std::uint64_t> next_temporary_file{1};

void requireObject(const Json &params, std::string_view method) {
    if (!params.is_object()) {
        throw std::invalid_argument(std::string{method} +
                                    " params must be an object");
    }
}

void requireOnly(const Json &params,
                 std::initializer_list<std::string_view> allowed,
                 std::string_view method) {
    requireObject(params, method);
    for (const auto &[key, value] : params.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) ==
            allowed.end()) {
            throw std::invalid_argument(std::string{method} +
                                        " contains unsupported field '" +
                                        key + "'");
        }
    }
}

std::string requireString(const Json &object, std::string_view field,
                          std::string_view method) {
    const auto found = object.find(std::string{field});
    if (found == object.end() || !found->is_string()) {
        throw std::invalid_argument(std::string{method} +
                                    " requires string field '" +
                                    std::string{field} + "'");
    }
    return found->get<std::string>();
}

bool isLowerHexDigest(std::string_view value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= '0' && ch <= '9') ||
                      (ch >= 'a' && ch <= 'f');
           });
}

std::string readFileBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error(
            "could not open render config source: " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

void replaceFile(const std::filesystem::path &temporary,
                 const std::filesystem::path &destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::error_code error{static_cast<int>(GetLastError()),
                                    std::system_category()};
        throw std::runtime_error("atomic render config replace failed: " +
                                 error.message());
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        throw std::runtime_error("atomic render config replace failed: " +
                                 error.message());
    }
#endif
}

class ExternalModification final : public std::runtime_error {
  public:
    explicit ExternalModification(const std::string &message)
        : std::runtime_error{message} {}
};

bool atomicReplaceWithDigestCas(const std::filesystem::path &destination,
                                std::string_view expected_digest,
                                std::string_view next_bytes) {
    auto current = readFileBytes(destination);
    if (renderConfigSourceDigest(current) != expected_digest) {
        throw ExternalModification{
            "render config source changed outside the editor"};
    }
    if (current == next_bytes) return false;

    auto temporary = destination;
    temporary += ".pelican-render-edit-" +
                 std::to_string(next_temporary_file.fetch_add(1)) + ".tmp";
    try {
        std::ofstream output{temporary,
                             std::ios::binary | std::ios::trunc};
        if (!output.is_open()) {
            throw std::runtime_error(
                "could not open temporary render config file");
        }
        output.write(next_bytes.data(),
                     static_cast<std::streamsize>(next_bytes.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error(
                "could not flush temporary render config file");
        }
        output.close();
        if (!output) {
            throw std::runtime_error(
                "could not close temporary render config file");
        }

        // Recheck after the temporary write so an external edit racing the
        // compiler/preflight cannot be overwritten by the atomic replace.
        current = readFileBytes(destination);
        if (renderConfigSourceDigest(current) != expected_digest) {
            throw ExternalModification{
                "render config source changed while the candidate was prepared"};
        }
        replaceFile(temporary, destination);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    return true;
}

OrderedJson digestJson(std::string_view digest) {
    return {{"algorithm", "sha256"}, {"hex", digest}};
}

OrderedJson runtimeJson(const RenderConfigRuntimeSnapshot &runtime) {
    return {{"published_generation", runtime.published_generation},
            {"enabled_feature_names", runtime.enabled_feature_names}};
}

std::string primaryReason(const std::vector<std::string> &reasons) {
    return reasons.empty() ? std::string{"editor_gate_closed"}
                           : reasons.front();
}

} // namespace

std::vector<RenderFeatureCatalogEntry>
enumerateEngineRenderFeatureDocuments(
    const std::function<RenderFeatureRuntimeAvailability(
        std::string_view, const Json &)> &availability) {
    if (!availability) {
        throw std::invalid_argument(
            "render feature enumeration requires an engine availability evaluator");
    }
    std::vector<RenderFeatureCatalogEntry> result;
    std::set<std::string, std::less<>> names;
    for (const auto id : registeredEngineResourceIds()) {
        if (!id.starts_with("features/") || !id.ends_with(".json")) {
            continue;
        }
        const auto resource = engineResource(id);
        if (!resource) continue;
        const auto document = Json::parse(*resource);
        if (!document.is_object() ||
            document.value("schema", std::string{}) !=
                "pelican.render_feature") {
            continue;
        }
        if (!document.contains("name") ||
            !document.at("name").is_string()) {
            throw std::runtime_error(
                "registered render feature has no string name: " +
                std::string{id});
        }
        const auto name = document.at("name").get<std::string>();
        if (!names.insert(name).second) {
            throw std::runtime_error(
                "registered render feature name is duplicated: " + name);
        }
        auto evaluated = availability(name, document);
        if (!evaluated.available &&
            evaluated.unavailable_reason.empty()) {
            throw std::runtime_error(
                "engine marked render feature unavailable without a reason: " +
                name);
        }
        if (evaluated.available) {
            evaluated.unavailable_reason.clear();
        }
        result.push_back(RenderFeatureCatalogEntry{
            .name = name,
            .reference = "engine://" + std::string{id},
            .requires_runtime_module =
                renderFeatureRequiresRuntimeModule(name),
            .available = evaluated.available,
            .unavailable_reason =
                std::move(evaluated.unavailable_reason),
        });
    }
    std::ranges::sort(result, {}, &RenderFeatureCatalogEntry::name);
    return result;
}

RenderConfigEditorService::RenderConfigEditorService(
    RenderConfigEditorDependencies dependencies)
    : dependencies_{std::move(dependencies)},
      document_{AuthoredRenderConfigDocument::parse(
          dependencies_.source_bytes)} {
    if (dependencies_.source_reference.empty() ||
        dependencies_.source_path.empty() || !dependencies_.gate ||
        !dependencies_.runtime_snapshot || !dependencies_.apply_candidate) {
        throw std::invalid_argument(
            "RenderConfigEditorService requires source, gate, runtime and apply dependencies");
    }
    if (!dependencies_.feature_catalog) {
        throw std::invalid_argument(
            "RenderConfigEditorService requires an engine-evaluated feature catalog");
    }
    feature_catalog_ = dependencies_.feature_catalog();
}

RenderConfigEditorService::GateSnapshot
RenderConfigEditorService::gateSnapshot() {
    const auto observation = dependencies_.gate();
    if (!gate_initialized_) {
        gate_initialized_ = true;
        observed_can_edit_ = observation.can_edit;
        observed_transition_epoch_ = observation.transition_epoch;
        observed_gate_reasons_ = observation.reasons;
    } else if (observed_can_edit_ != observation.can_edit ||
               observed_transition_epoch_ != observation.transition_epoch ||
               observed_gate_reasons_ != observation.reasons) {
        observed_can_edit_ = observation.can_edit;
        observed_transition_epoch_ = observation.transition_epoch;
        observed_gate_reasons_ = observation.reasons;
        ++gate_epoch_;
    }
    return {.can_edit = observed_can_edit_,
            .epoch = gate_epoch_,
            .transition_epoch = observed_transition_epoch_,
            .reasons = observed_gate_reasons_};
}

OrderedJson RenderConfigEditorService::rejection(
    std::string code, std::string message, OrderedJson details) const {
    details["code"] = std::move(code);
    details["message"] = std::move(message);
    return {{"status", "rejected"},
            {"committed", false},
            {"error", std::move(details)}};
}

const RenderFeatureCatalogEntry *
RenderConfigEditorService::findCatalogReference(
    std::string_view reference) const noexcept {
    const auto found = std::find_if(
        feature_catalog_.begin(), feature_catalog_.end(),
        [&](const auto &entry) { return entry.reference == reference; });
    return found == feature_catalog_.end() ? nullptr : &*found;
}

OrderedJson RenderConfigEditorService::getRenderFeatures(
    const Json &params) const {
    requireOnly(params, {}, "get_render_features");
    return {{"source_reference", dependencies_.source_reference},
            {"source_digest", digestJson(document_.sourceDigest())},
            {"features", document_.featureReferences()},
            {"runtime", runtimeJson(dependencies_.runtime_snapshot())}};
}

OrderedJson RenderConfigEditorService::listRenderFeatures(
    const Json &params) const {
    requireOnly(params, {}, "list_render_features");
    auto features = OrderedJson::array();
    for (const auto &entry : feature_catalog_) {
        auto encoded = OrderedJson{
            {"name", entry.name},
            {"reference", entry.reference},
            {"requires_runtime_module", entry.requires_runtime_module},
            {"hot_add_supported", entry.available},
            {"available", entry.available},
        };
        if (!entry.available) {
            encoded["unavailable_reason"] =
                entry.unavailable_reason;
        }
        features.push_back(std::move(encoded));
    }
    return {{"features", std::move(features)}};
}

OrderedJson RenderConfigEditorService::editRenderFeatures(
    const Json &params) {
    requireOnly(params, {"base_source_digest", "operations"},
                "edit_render_features");
    const auto base_digest = requireString(
        params, "base_source_digest", "edit_render_features");
    if (!isLowerHexDigest(base_digest)) {
        throw std::invalid_argument(
            "edit_render_features base_source_digest must be 64 lowercase hex characters");
    }
    const auto operations = params.find("operations");
    if (operations == params.end() || !operations->is_array() ||
        operations->size() > 1) {
        throw std::invalid_argument(
            "edit_render_features operations must be an array containing at most one element in v1");
    }

    const auto gate = gateSnapshot();
    if (!gate.can_edit) {
        return rejection(
            "gate_closed", "render feature editing is currently disabled",
            {{"method", "edit_render_features"},
             {"reason", primaryReason(gate.reasons)},
             {"gate_epoch", gate.epoch}});
    }
    if (base_digest != document_.sourceDigest()) {
        return rejection(
            "external_modification",
            "render config base digest no longer matches the engine-owned document",
            {{"expected_source_digest", document_.sourceDigest()},
             {"provided_source_digest", base_digest}});
    }
    try {
        const auto disk_digest =
            renderConfigSourceDigest(readFileBytes(dependencies_.source_path));
        if (disk_digest != document_.sourceDigest()) {
            return rejection(
                "external_modification",
                "render config source changed outside the editor",
                {{"expected_source_digest", document_.sourceDigest()},
                 {"actual_source_digest", disk_digest}});
        }
    } catch (const std::exception &error) {
        return rejection("external_modification", error.what());
    }

    auto candidate = document_;
    if (!operations->empty()) {
        const auto &operation = operations->front();
        requireOnly(operation, {"op", "feature"},
                    "edit_render_features operation");
        const auto op = requireString(operation, "op",
                                      "edit_render_features operation");
        const auto feature = requireString(
            operation, "feature", "edit_render_features operation");
        if (op == "add") {
            const auto *catalog = findCatalogReference(feature);
            if (catalog == nullptr) {
                return rejection(
                    "unknown_render_feature",
                    "render feature is not present in the engine catalog: " +
                        feature,
                    {{"feature", feature}});
            }
            if (!candidate.containsFeature(feature) &&
                !catalog->available) {
                return rejection(
                    catalog->requires_runtime_module
                        ? "restart_required_feature"
                        : "render_feature_unavailable",
                    catalog->unavailable_reason,
                    {{"feature", catalog->name},
                     {"reference", catalog->reference}});
            }
            candidate = candidate.withFeatureAdded(feature);
        } else if (op == "remove") {
            try {
                candidate = candidate.withFeatureRemoved(feature);
            } catch (const std::invalid_argument &error) {
                return rejection("render_feature_not_enabled", error.what(),
                                 {{"feature", feature}});
            }
        } else {
            throw std::invalid_argument(
                "edit_render_features operation op must be 'add' or 'remove'");
        }
    }

    const auto ticket_id =
        "render-feature-edit-" + std::to_string(next_ticket_++);
    pending_.push_back(Ticket{
        .id = ticket_id,
        .accepted_gate_epoch = gate.epoch,
        .accepted_transition_epoch = gate.transition_epoch,
        .base_source_digest = base_digest,
        .candidate = std::move(candidate),
    });
    auto accepted = OrderedJson{
        {"ticket", ticket_id},
        {"status", "accepted"},
        {"base_source_digest", digestJson(base_digest)},
    };
    results_[ticket_id] = {{"ticket", ticket_id}, {"status", "pending"}};
    return accepted;
}

OrderedJson RenderConfigEditorService::getResult(
    const Json &params) const {
    requireOnly(params, {"ticket"}, "get_edit_result");
    const auto ticket = requireString(params, "ticket", "get_edit_result");
    const auto found = results_.find(ticket);
    if (found == results_.end()) {
        throw std::invalid_argument(
            "get_edit_result ticket was not issued by this session");
    }
    return found->second;
}

bool RenderConfigEditorService::ownsTicket(
    std::string_view ticket) const noexcept {
    return results_.contains(std::string{ticket});
}

void RenderConfigEditorService::commitPending() noexcept {
    auto queue = std::move(pending_);
    pending_.clear();
    for (auto &ticket : queue) {
        auto finish = [&](OrderedJson result) {
            if (!result.contains("ticket")) result["ticket"] = ticket.id;
            results_[ticket.id] = result;
            completed_.push_back(std::move(result));
        };
        bool source_replaced = false;
        try {
            const auto gate = gateSnapshot();
            if (!gate.can_edit || gate.epoch != ticket.accepted_gate_epoch ||
                gate.transition_epoch != ticket.accepted_transition_epoch) {
                finish(rejection(
                    "gate_closed",
                    "editor gate changed after render feature edit acceptance",
                    {{"ticket", ticket.id},
                     {"method", "edit_render_features"},
                     {"reason", primaryReason(gate.reasons)},
                     {"gate_epoch", gate.epoch}}));
                continue;
            }
            if (ticket.base_source_digest != document_.sourceDigest()) {
                finish(rejection(
                    "stale_source_digest",
                    "another render feature edit committed first",
                    {{"ticket", ticket.id},
                     {"current_source_digest", document_.sourceDigest()}}));
                continue;
            }
            const auto disk_digest = renderConfigSourceDigest(
                readFileBytes(dependencies_.source_path));
            if (disk_digest != ticket.base_source_digest) {
                finish(rejection(
                    "external_modification",
                    "render config source changed outside the editor",
                    {{"ticket", ticket.id},
                     {"expected_source_digest", ticket.base_source_digest},
                     {"actual_source_digest", disk_digest}}));
                continue;
            }

            if (ticket.candidate.sourceDigest() ==
                document_.sourceDigest()) {
                const auto runtime =
                    dependencies_.runtime_snapshot();
                finish(OrderedJson{
                    {"ticket", ticket.id},
                    {"status", "committed"},
                    {"committed", true},
                    {"no_change", true},
                    {"published_generation",
                     runtime.published_generation},
                    {"source_digest",
                     digestJson(document_.sourceDigest())},
                    {"source_commit_called", false},
                });
                continue;
            }

            bool source_commit_called = false;
            std::string source_external_modification;
            const auto source_commit = [&] {
                source_commit_called = true;
                try {
                    source_replaced = atomicReplaceWithDigestCas(
                        dependencies_.source_path,
                        ticket.base_source_digest,
                        ticket.candidate.bytes());
                } catch (const ExternalModification &error) {
                    // Production's reload participant converts callback
                    // exceptions to its result type. Preserve the CAS
                    // classification across that boundary.
                    source_external_modification = error.what();
                    throw;
                }
            };
            const auto applied = dependencies_.apply_candidate(
                ticket.candidate.bytes(), source_commit);
            if (!source_external_modification.empty()) {
                finish(rejection(
                    "external_modification",
                    source_external_modification,
                    {{"ticket", ticket.id},
                     {"expected_source_digest",
                      ticket.base_source_digest}}));
                continue;
            }
            if (!applied.committed) {
                std::string rollback_error;
                if (source_replaced) {
                    try {
                        (void)atomicReplaceWithDigestCas(
                            dependencies_.source_path,
                            ticket.candidate.sourceDigest(),
                            document_.bytes());
                    } catch (const std::exception &error) {
                        rollback_error = error.what();
                    }
                }
                auto details = OrderedJson{
                    {"ticket", ticket.id},
                    {"participant", "pelican.render_pipeline"},
                };
                if (!rollback_error.empty()) {
                    details["rollback_error"] = rollback_error;
                }
                finish(rejection(
                    rollback_error.empty() ? "render_pipeline_preflight_failed"
                                           : "render_config_rollback_failed",
                    applied.error.empty()
                        ? "render pipeline candidate was rejected"
                        : applied.error,
                    std::move(details)));
                continue;
            }
            if (!source_commit_called) {
                finish(rejection(
                    "render_pipeline_commit_protocol_error",
                    "pelican.render_pipeline reported a commit without invoking the source commit callback",
                    {{"ticket", ticket.id},
                     {"participant", "pelican.render_pipeline"}}));
                continue;
            }

            document_ = std::move(ticket.candidate);
            auto result = OrderedJson{
                {"ticket", ticket.id},
                {"status", "committed"},
                {"committed", true},
                {"published_generation", applied.published_generation},
                {"source_digest", digestJson(document_.sourceDigest())},
                {"source_commit_called", source_commit_called},
            };
            if (!applied.post_commit_error.empty()) {
                result["post_commit_error"] = applied.post_commit_error;
            }
            finish(std::move(result));
        } catch (const ExternalModification &error) {
            finish(rejection(
                "external_modification", error.what(),
                {{"ticket", ticket.id},
                 {"expected_source_digest", ticket.base_source_digest}}));
        } catch (const std::exception &error) {
            if (source_replaced) {
                try {
                    (void)atomicReplaceWithDigestCas(
                        dependencies_.source_path,
                        ticket.candidate.sourceDigest(), document_.bytes());
                } catch (...) {
                    finish(rejection(
                        "render_config_rollback_failed", error.what(),
                        {{"ticket", ticket.id}}));
                    continue;
                }
            }
            finish(rejection(
                "render_pipeline_preflight_failed", error.what(),
                {{"ticket", ticket.id},
                 {"participant", "pelican.render_pipeline"}}));
        } catch (...) {
            finish(rejection(
                "render_pipeline_preflight_failed",
                "unknown render pipeline candidate failure",
                {{"ticket", ticket.id},
                 {"participant", "pelican.render_pipeline"}}));
        }
    }
}

std::vector<OrderedJson>
RenderConfigEditorService::takeCompletedResults() {
    auto result = std::move(completed_);
    completed_.clear();
    return result;
}

} // namespace Pelican
