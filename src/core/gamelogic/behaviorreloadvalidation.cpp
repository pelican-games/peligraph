#include "behaviorreloadvalidation.hpp"

#include "../userpublic/details/behavior/registerer.hpp"
#include "../userpublic/details/event/registerer.hpp"
#include "../ecs/componentinfo.hpp"

#include <stdexcept>
#include <algorithm>
#include <string>
#include <vector>

namespace Pelican::internal {

namespace {

struct SchemaSnapshot {
    RegistrationKind kind;
    std::string key;
    std::uint32_t version;
    std::string fingerprint;
};

std::vector<SchemaSnapshot> schemaSnapshots(RegistrationOwner owner) {
    std::vector<SchemaSnapshot> result;
    for (const auto &registration :
         getBehaviorRegisterer().registeredBehaviors()) {
        if (registration.owner != owner) continue;
        result.push_back({RegistrationKind::behavior,
                          registration.stable_name,
                          registration.schema_version,
                          registration.params_schema_fingerprint});
    }

    const auto &events = getEventRegisterer();
    for (const auto &registration : events.registeredEventTypes()) {
        if (registration.owner != owner) continue;
        const auto metadata = std::ranges::find_if(
            events.registeredV3Schemas(), [&](const auto &entry) {
                return entry.token == registration.token;
            });
        if (metadata == events.registeredV3Schemas().end()) continue;
        result.push_back({RegistrationKind::event, registration.name,
                          metadata->schema_version,
                          metadata->schema_fingerprint});
    }

    if (const auto *components =
            FastModuleContainer::tryGet<ComponentInfoManager>()) {
        for (const auto &registration : components->registeredComponents()) {
            if (!registration.token || registration.owner != owner ||
                !registration.schema_v3)
                continue;
            result.push_back({RegistrationKind::component,
                              registration.name,
                              registration.schema_version,
                              registration.schema_fingerprint});
        }
    }
    return result;
}

void validateSchemaSnapshots(RegistrationOwner active_owner,
                             RegistrationOwner candidate_owner) {
    const auto active = schemaSnapshots(active_owner);
    const auto candidate = schemaSnapshots(candidate_owner);
    for (const auto &expected : active) {
        const auto found = std::ranges::find_if(
            candidate, [&](const auto &entry) {
                return entry.kind == expected.kind && entry.key == expected.key;
            });
        if (found == candidate.end()) continue; // removal diagnostics stay registry-specific
        if (found->version == expected.version &&
            found->fingerprint != expected.fingerprint) {
            throw std::runtime_error(
                "schema_changed_without_version_bump: kind='" +
                std::to_string(static_cast<unsigned>(expected.kind)) +
                "' stable_name='" + expected.key + "' version=" +
                std::to_string(expected.version));
        }
    }
}

} // namespace

void validateBehaviorReloadSources(
    RegistrationOwner active_owner, RegistrationOwner candidate_owner,
    std::optional<std::span<const BehaviorReloadSourceParams>> sources) {
    validateSchemaSnapshots(active_owner, candidate_owner);
    const auto &registry = getBehaviorRegisterer();
    for (const auto &active : registry.registeredBehaviors()) {
        if (active.owner != active_owner) continue;

        const auto *candidate = registry.findByNameAndOwner(
            active.stable_name, candidate_owner);
        if (candidate == nullptr) {
            throw std::runtime_error(
                "behavior_type_removed: stable_name='" + active.stable_name +
                "' version=" + std::to_string(active.schema_version));
        }
        if (candidate->schema_version == active.schema_version &&
            candidate->params_schema_fingerprint !=
                active.params_schema_fingerprint) {
            throw std::runtime_error(
                "schema_changed_without_version_bump: stable_name='" +
                active.stable_name + "' version=" +
                std::to_string(active.schema_version));
        }
        if (candidate->schema_version == active.schema_version) continue;

        if (!sources) {
            throw std::runtime_error(
                "schema_incompatible: stable_name='" + active.stable_name +
                "' version=" + std::to_string(active.schema_version) +
                "->" + std::to_string(candidate->schema_version) +
                " field='params' authoring document is unavailable");
        }

        for (const auto &source : *sources) {
            if (source.stable_name != active.stable_name) continue;
            try {
                // Do not pass an active-canonicalized value. Candidate defaults
                // are applied once, here, to the authored source params.
                (void)candidate->canonicalize_params(source.source_params);
            } catch (const StructFieldValidationError &error) {
                throw std::runtime_error(
                    "schema_incompatible: stable_name='" +
                    active.stable_name + "' version=" +
                    std::to_string(active.schema_version) + "->" +
                    std::to_string(candidate->schema_version) + " scene='" +
                    source.provenance.scene_id + "' object_index=" +
                    std::to_string(source.provenance.object_index) +
                    " component_index=" +
                    std::to_string(source.provenance.component_index) +
                    " field='" + std::string{error.path()} + "': " +
                    error.what());
            } catch (const std::exception &error) {
                throw std::runtime_error(
                    "schema_incompatible: stable_name='" +
                    active.stable_name + "' version=" +
                    std::to_string(active.schema_version) + "->" +
                    std::to_string(candidate->schema_version) + " scene='" +
                    source.provenance.scene_id + "' object_index=" +
                    std::to_string(source.provenance.object_index) +
                    " component_index=" +
                    std::to_string(source.provenance.component_index) +
                    " field='params': " + error.what());
            }
        }
    }
}

} // namespace Pelican::internal
