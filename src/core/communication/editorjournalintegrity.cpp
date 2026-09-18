#include "editorjournalintegrity.hpp"

#include "../loader/resolvedscene.hpp"
#include "../../project/prefab.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Pelican {
namespace EditorJournalInternal {

std::vector<OriginTransitionAuthority> unpackAuthorityFromJson(
    const Json &value) {
    if (!value.is_array()) {
        throw std::invalid_argument(
            "unpack expanded_components must be an array");
    }
    std::vector<OriginTransitionAuthority> result;
    result.reserve(value.size());
    for (const auto &entry : value) {
        requireOnly(entry, {"component_key", "body"},
                    "unpack expanded component");
        result.push_back({
            .component_key = requireString(entry, "component_key",
                                           "unpack expanded component"),
            .body = entry.at("body"),
        });
    }
    return result;
}

bool isCanonicalUnpackOperation(const Json &operation) {
    const auto op = operation.value("op", std::string{});
    return op == "unpack_prefab" || op == "restore_prefab";
}

std::optional<std::string> unpackJournalIntegrityFailureImpl(
    const OrderedJson &forward, const OrderedJson &inverse) {
    try {
        if (!isCanonicalUnpackOperation(forward) ||
            !isCanonicalUnpackOperation(inverse)) {
            return "operation";
        }
        const auto forward_op = forward.at("op").get<std::string>();
        const auto inverse_op = inverse.at("op").get<std::string>();
        if ((forward_op == "unpack_prefab") ==
            (inverse_op == "unpack_prefab")) {
            return "operation";
        }
        constexpr std::array<std::string_view, 6> common_fields{
            "object", "scene_id", "prefab", "expanded_components",
            "dependency_witness", "origin_transition"};
        if (forward.size() != common_fields.size() + 1U ||
            inverse.size() != common_fields.size() + 1U) {
            return "shape";
        }
        for (const auto field : common_fields) {
            if (!forward.contains(std::string{field}) ||
                !inverse.contains(std::string{field})) {
                return "shape";
            }
            if (field != "origin_transition" && field != "prefab" &&
                field != "dependency_witness" &&
                forward.at(std::string{field}) !=
                    inverse.at(std::string{field})) {
                return std::string{field};
            }
        }
        const auto &forward_prefab = forward.at("prefab");
        const auto &inverse_prefab = inverse.at("prefab");
        const auto &forward_witness = forward.at("dependency_witness");
        const auto &inverse_witness = inverse.at("dependency_witness");
        if (!forward_prefab.is_object() || !inverse_prefab.is_object() ||
            !forward_witness.is_object() || !inverse_witness.is_object() ||
            !forward_prefab.contains("ref") ||
            !forward_prefab.at("ref").is_string() ||
            !inverse_prefab.contains("ref") ||
            !inverse_prefab.at("ref").is_string() ||
            !forward_witness.contains("ref") ||
            !forward_witness.at("ref").is_string() ||
            !inverse_witness.contains("ref") ||
            !inverse_witness.at("ref").is_string()) {
            return "ref";
        }
        if (forward_prefab.at("ref") != inverse_prefab.at("ref") ||
            forward_prefab.at("ref") != forward_witness.at("ref") ||
            forward_prefab.at("ref") != inverse_witness.at("ref")) {
            return "ref";
        }
        if (!forward_prefab.contains("instance_id") ||
            !forward_prefab.at("instance_id").is_string() ||
            !inverse_prefab.contains("instance_id") ||
            !inverse_prefab.at("instance_id").is_string() ||
            !forward_witness.contains("instance_id") ||
            !forward_witness.at("instance_id").is_string() ||
            !inverse_witness.contains("instance_id") ||
            !inverse_witness.at("instance_id").is_string()) {
            return "instance_id";
        }
        if (forward_prefab.at("instance_id") !=
                inverse_prefab.at("instance_id") ||
            forward_prefab.at("instance_id") !=
                forward_witness.at("instance_id") ||
            forward_prefab.at("instance_id") !=
                inverse_witness.at("instance_id")) {
            return "instance_id";
        }
        if (forward_prefab != inverse_prefab) return "prefab";
        if (forward_witness != inverse_witness) {
            return "dependency_witness";
        }
        constexpr std::array<std::string_view, 6> witness_fields{
            "ref", "instance_id", "prefab_digest_sha256",
            "provider_fingerprint_sha256", "behaviors",
            "resolver_inputs_fingerprint_sha256"};
        if (forward_witness.size() != witness_fields.size()) {
            return "dependency_witness";
        }
        for (const auto field : witness_fields) {
            if (!forward_witness.contains(std::string{field})) {
                return "dependency_witness";
            }
        }
        for (const auto field : {"ref", "instance_id",
                                 "prefab_digest_sha256",
                                 "provider_fingerprint_sha256",
                                 "resolver_inputs_fingerprint_sha256"}) {
            if (!forward_witness.at(field).is_string()) {
                return "dependency_witness";
            }
        }
        const auto &behaviors = forward_witness.at("behaviors");
        if (!behaviors.is_array()) return "behaviors";
        std::string previous_behavior;
        for (const auto &behavior : behaviors) {
            if (!behavior.is_object() || behavior.size() != 3U ||
                !behavior.contains("stable_name") ||
                !behavior.at("stable_name").is_string() ||
                !behavior.contains("schema_version") ||
                !behavior.contains("params_schema_fingerprint") ||
                !behavior.at("params_schema_fingerprint").is_string()) {
                return "behaviors";
            }
            (void)exactUnsigned(behavior.at("schema_version"),
                                "unpack behavior schema_version");
            const auto name = behavior.at("stable_name").get<std::string>();
            if (name.empty() ||
                (!previous_behavior.empty() && name <= previous_behavior)) {
                return "behaviors";
            }
            previous_behavior = name;
        }
        const auto forward_map = originTransitionMapFromJson(
            forward.at("origin_transition"));
        const auto inverse_map = originTransitionMapFromJson(
            inverse.at("origin_transition"));
        if (invertOriginTransitionMap(forward_map) != inverse_map) {
            return "origin_transition";
        }
        if ((forward_op == "unpack_prefab" &&
             forward_map.direction !=
                 OriginTransitionDirection::GeneratedToAuthored) ||
            (forward_op == "restore_prefab" &&
             forward_map.direction !=
                 OriginTransitionDirection::AuthoredToGenerated)) {
            return "origin_transition_direction";
        }
        const auto authority = unpackAuthorityFromJson(
            forward.at("expanded_components"));
        if (authority.size() != forward_map.entries.size()) {
            return "origin_transition_count";
        }
        const auto object_id = exactUnsigned(
            forward.at("object"), "unpack journal object", true);
        const auto scene_id = requireString(forward, "scene_id",
                                            "unpack journal operation");
        const auto instance_id = requireString(
            forward_witness, "instance_id", "unpack dependency witness");
        std::unordered_set<std::string> component_keys;
        std::unordered_set<std::string> gids;
        std::unordered_set<std::size_t> authored_indices;
        for (std::size_t index = 0; index < authority.size(); ++index) {
            const auto &body = authority[index].body;
            const auto &entry = forward_map.entries[index];
            if (authority[index].component_key.empty() || !body.is_object() ||
                !component_keys.insert(authority[index].component_key).second ||
                authority[index].component_key != entry.component_key) {
                return "component_key";
            }
            for (auto field = body.begin(); field != body.end(); ++field) {
                if (prefabGeneratedMarkerRule(field.key(), field.value())) {
                    return "expanded_components";
                }
            }
            if (entry.scene_id != scene_id ||
                entry.authoring_object_id.value != object_id) {
                return "origin_transition_target";
            }
            if (entry.gid != prefabGeneratedId(instance_id,
                                                entry.component_key) ||
                !gids.insert(entry.gid).second) {
                return "gid";
            }
            if (!authored_indices.insert(entry.final_authored_index).second ||
                (index != 0U &&
                 (forward_map.entries[index - 1U].final_authored_index ==
                      std::numeric_limits<std::size_t>::max() ||
                  entry.final_authored_index !=
                      forward_map.entries[index - 1U]
                              .final_authored_index +
                          1U))) {
                return "final_authored_index";
            }
        }
        return std::nullopt;
    } catch (const std::exception &) {
        return "shape";
    }
}

void requireUnpackDependencies(const OrderedJson &operation,
                               const SceneProjectionState &state) {
    if (!isCanonicalUnpackOperation(operation)) return;
    const auto &witness = operation.at("dependency_witness");
    const auto ref = requireString(witness, "ref",
                                   "unpack dependency witness");
    const auto expected_digest = requireString(
        witness, "prefab_digest_sha256", "unpack dependency witness");
    const auto *record = state.resolved().prefabRegistry().find(ref);
    const auto current_digest =
        record == nullptr ? std::string{} : record->digest_sha256;
    if (current_digest != expected_digest) {
        editFailure(EditorEditErrorCode::prefab_digest_changed,
                    {{"expected", expected_digest},
                     {"current", current_digest},
                     {"ref", ref}},
                    "prefab digest changed since unpack");
    }

    const auto expected_provider = requireString(
        witness, "provider_fingerprint_sha256",
        "unpack dependency witness");
    const auto current_provider =
        state.resolved().prefabClosure().provider_fingerprint_sha256;
    if (current_provider != expected_provider) {
        editFailure(EditorEditErrorCode::prefab_provider_changed,
                    {{"expected", expected_provider},
                     {"current", current_provider}},
                    "prefab provider fingerprint changed since unpack");
    }

    const auto expected_behaviors = witness.find("behaviors");
    if (expected_behaviors == witness.end() ||
        !expected_behaviors->is_array()) {
        editFailure(EditorEditErrorCode::journal_integrity_error,
                    {{"reason", "behaviors"}},
                    "unpack behavior dependency witness is invalid");
    }
    OrderedJson current_behaviors = OrderedJson::array();
    const auto dependencies =
        state.resolved().bindableProvider().behaviorDependencies();
    for (const auto &expected : *expected_behaviors) {
        const auto name = requireString(expected, "stable_name",
                                        "unpack behavior dependency");
        const auto found = std::lower_bound(
            dependencies.begin(), dependencies.end(), name,
            [](const auto &dependency, std::string_view stable_name) {
                return dependency.stable_name < stable_name;
            });
        if (found == dependencies.end() || found->stable_name != name) {
            current_behaviors.push_back(nullptr);
        } else {
            current_behaviors.push_back(
                {{"stable_name", found->stable_name},
                 {"schema_version", found->schema_version},
                 {"params_schema_fingerprint",
                  found->params_schema_fingerprint}});
        }
    }
    const auto expected_resolver = requireString(
        witness, "resolver_inputs_fingerprint_sha256",
        "unpack dependency witness");
    const auto current_resolver = state.resolved()
                                      .prefabClosure()
                                      .resolver_inputs_fingerprint_sha256;
    if (current_behaviors != OrderedJson(*expected_behaviors) ||
        current_resolver != expected_resolver) {
        editFailure(EditorEditErrorCode::prefab_behavior_dependency_changed,
                    {{"expected_behaviors", *expected_behaviors},
                     {"current_behaviors", current_behaviors},
                     {"expected_resolver_inputs", expected_resolver},
                     {"current_resolver_inputs", current_resolver}},
                    "prefab behavior dependency or resolver inputs changed since unpack");
    }
}

bool journalUsesUnpack(const EditorJournalRecord &record) {
    for (const auto &operation : record.ordered_forward) {
        if (isCanonicalUnpackOperation(operation)) return true;
    }
    for (const auto &operation : record.ordered_inverse) {
        if (isCanonicalUnpackOperation(operation)) return true;
    }
    return false;
}

void requireUnpackJournalRecord(const EditorJournalRecord &record,
                                const SceneProjectionState &state) {
    if (!journalUsesUnpack(record)) return;
    if (record.ordered_forward.size() != 1U ||
        record.ordered_inverse.size() != 1U ||
        record.commands.size() != 1U ||
        record.commands.front().forward != record.ordered_forward.front() ||
        record.commands.front().inverse != record.ordered_inverse.front()) {
        editFailure(EditorEditErrorCode::journal_integrity_error,
                    {{"reason", "record"}},
                    "unpack journal record is internally inconsistent");
    }
    if (const auto failure = unpackJournalIntegrityFailureImpl(
            record.ordered_forward.front(),
            record.ordered_inverse.front())) {
        editFailure(EditorEditErrorCode::journal_integrity_error,
                    {{"reason", *failure}},
                    "unpack journal forward and inverse are inconsistent");
    }
    requireUnpackDependencies(record.ordered_forward.front(), state);
}

} // namespace EditorJournalInternal
} // namespace Pelican
