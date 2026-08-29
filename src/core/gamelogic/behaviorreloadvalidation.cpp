#include "behaviorreloadvalidation.hpp"

#include "../userpublic/details/behavior/registerer.hpp"

#include <stdexcept>
#include <string>

namespace Pelican::internal {

void validateBehaviorReloadSources(
    RegistrationOwner active_owner, RegistrationOwner candidate_owner,
    std::optional<std::span<const BehaviorReloadSourceParams>> sources) {
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
