#include "physicaltargetplanwire.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;

struct ValidationFailure final : std::exception {
    std::string code;
    std::string message;

    ValidationFailure(std::string reason_code, std::string detail)
        : code{std::move(reason_code)}, message{std::move(detail)} {}

    const char *what() const noexcept override { return message.c_str(); }
};

[[noreturn]] void fail(std::string code, std::string detail) {
    throw ValidationFailure{std::move(code), std::move(detail)};
}

const Json &requireField(const Json &object, std::string_view key,
                         std::string_view context) {
    if (!object.is_object()) {
        fail("physical_plan_type_error",
             std::string{context} + " must be an object");
    }
    const auto found = object.find(std::string{key});
    if (found == object.end()) {
        fail("physical_plan_missing_field",
             std::string{context} + " requires field '" +
                 std::string{key} + "'");
    }
    return *found;
}

const Json &requireObject(const Json &object, std::string_view key,
                          std::string_view context) {
    const auto &value = requireField(object, key, context);
    if (!value.is_object()) {
        fail("physical_plan_type_error",
             std::string{context} + "." + std::string{key} +
                 " must be an object");
    }
    return value;
}

const Json &requireArray(const Json &object, std::string_view key,
                         std::string_view context) {
    const auto &value = requireField(object, key, context);
    if (!value.is_array()) {
        fail("physical_plan_type_error",
             std::string{context} + "." + std::string{key} +
                 " must be an array");
    }
    return value;
}

std::string requireString(const Json &object, std::string_view key,
                          std::string_view context) {
    const auto &value = requireField(object, key, context);
    if (!value.is_string()) {
        fail("physical_plan_type_error",
             std::string{context} + "." + std::string{key} +
                 " must be a string");
    }
    const auto result = value.get<std::string>();
    if (result.empty()) {
        fail("physical_plan_type_error",
             std::string{context} + "." + std::string{key} +
                 " must not be empty");
    }
    return result;
}

bool requireBool(const Json &object, std::string_view key,
                 std::string_view context) {
    const auto &value = requireField(object, key, context);
    if (!value.is_boolean()) {
        fail("physical_plan_type_error",
             std::string{context} + "." + std::string{key} +
                 " must be a boolean");
    }
    return value.get<bool>();
}

std::uint64_t requireUnsigned(const Json &object, std::string_view key,
                              std::string_view context) {
    const auto &value = requireField(object, key, context);
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value >= 0) {
            return static_cast<std::uint64_t>(signed_value);
        }
    }
    fail("physical_plan_type_error",
         std::string{context} + "." + std::string{key} +
             " must be a non-negative integer");
}

double requireFiniteNumber(const Json &object, std::string_view key,
                           std::string_view context) {
    const auto &value = requireField(object, key, context);
    if (!value.is_number()) {
        fail("physical_plan_type_error",
             std::string{context} + "." + std::string{key} +
                 " must be a number");
    }
    const auto result = value.get<double>();
    if (!std::isfinite(result)) {
        fail("physical_plan_type_error",
             std::string{context} + "." + std::string{key} +
                 " must be finite");
    }
    return result;
}

std::vector<std::string> requireStringArray(const Json &object,
                                            std::string_view key,
                                            std::string_view context) {
    const auto &values = requireArray(object, key, context);
    std::vector<std::string> result;
    result.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (!values[index].is_string() ||
            values[index].get_ref<const std::string &>().empty()) {
            fail("physical_plan_type_error",
                 std::string{context} + "." + std::string{key} + "[" +
                     std::to_string(index) +
                     "] must be a non-empty string");
        }
        result.push_back(values[index].get<std::string>());
    }
    return result;
}

void requireUniqueStrings(const std::vector<std::string> &values,
                          std::string_view subject) {
    std::unordered_set<std::string> seen;
    for (const auto &value : values) {
        if (!seen.insert(value).second) {
            fail("physical_plan_duplicate",
                 std::string{"duplicate "} + std::string{subject} + " '" +
                     value + "'");
        }
    }
}

void validateSchemaVersion(const Json &object, std::string_view schema,
                           std::uint64_t version,
                           std::string_view context) {
    const auto actual_schema = requireString(object, "schema", context);
    if (actual_schema != schema) {
        fail("physical_plan_schema_mismatch",
             std::string{context} + " requires schema '" +
                 std::string{schema} + "', got '" + actual_schema + "'");
    }
    const auto actual_version = requireUnsigned(object, "version", context);
    if (actual_version != version) {
        fail("physical_plan_version_mismatch",
             std::string{context} + " requires version " +
                 std::to_string(version) + ", got " +
                 std::to_string(actual_version));
    }
}

void validateGraph(const Json &object, std::string_view expected,
                   std::string_view context) {
    const auto actual = requireString(object, "graph", context);
    if (!expected.empty() && actual != expected) {
        fail("physical_plan_graph_mismatch",
             std::string{context} + " graph '" + actual +
                 "' does not match '" + std::string{expected} + "'");
    }
}

void validateExtent(const Json &extent, std::string_view context) {
    if (!extent.is_object()) {
        fail("physical_plan_type_error",
             std::string{context} + " must be an object");
    }
    const auto kind = requireString(extent, "kind", context);
    const auto scale_x = requireFiniteNumber(extent, "scale_x", context);
    const auto scale_y = requireFiniteNumber(extent, "scale_y", context);
    const auto width = requireUnsigned(extent, "width", context);
    const auto height = requireUnsigned(extent, "height", context);
    if (kind == "fixed") {
        if (width == 0 || height == 0) {
            fail("physical_plan_extent_invalid",
                 std::string{context} +
                     " fixed extent requires positive width and height");
        }
    } else if (kind == "output_relative") {
        if (scale_x <= 0.0 || scale_y <= 0.0) {
            fail("physical_plan_extent_invalid",
                 std::string{context} +
                     " output_relative extent requires positive scales");
        }
    } else {
        fail("physical_plan_extent_invalid",
             std::string{context} + " has unknown extent kind '" + kind +
                 "'");
    }
}

void validateLifetime(const Json &lifetime, std::string_view context) {
    if (!lifetime.is_object()) {
        fail("physical_plan_type_error",
             std::string{context} + " must be an object");
    }
    const auto used = requireBool(lifetime, "used", context);
    if (!used) {
        return;
    }
    const auto first = requireUnsigned(lifetime, "first_use", context);
    const auto last = requireUnsigned(lifetime, "last_use", context);
    if (first > last) {
        fail("physical_plan_lifetime_invalid",
             std::string{context} +
                 " requires first_use to be no later than last_use");
    }
}

bool optionalFieldsEqual(const Json &left, const Json &right,
                         std::string_view key) {
    const auto left_value = left.find(std::string{key});
    const auto right_value = right.find(std::string{key});
    if (left_value == left.end() || right_value == right.end()) {
        return left_value == left.end() && right_value == right.end();
    }
    return *left_value == *right_value;
}

bool aliasContractsMatch(const Json &left, const Json &right) {
    constexpr std::string_view keys[]{
        "representation", "format",     "view_layout", "mip_levels",
        "array_layers",   "dimension",  "extent",
    };
    for (const auto key : keys) {
        if (left.at(key) != right.at(key)) {
            return false;
        }
    }
    return optionalFieldsEqual(left, right, "rasterization_samples") &&
           optionalFieldsEqual(left, right, "resolve_required");
}

bool aliasLifetimesDoNotOverlap(const Json &left, const Json &right) {
    const auto &left_lifetime = left.at("lifetime");
    const auto &right_lifetime = right.at("lifetime");
    if (!left_lifetime.at("used").get<bool>() ||
        !right_lifetime.at("used").get<bool>()) {
        return false;
    }
    return left_lifetime.at("last_use").get<std::uint64_t>() <
               right_lifetime.at("first_use").get<std::uint64_t>() ||
           right_lifetime.at("last_use").get<std::uint64_t>() <
               left_lifetime.at("first_use").get<std::uint64_t>();
}

std::set<std::string, std::less<>> toSet(
    const std::vector<std::string> &values) {
    return {values.begin(), values.end()};
}

void requireSameSet(const std::set<std::string, std::less<>> &actual,
                    const std::set<std::string, std::less<>> &expected_set,
                    std::string_view subject) {
    if (actual == expected_set) {
        return;
    }
    const auto missing = std::find_if(
        expected_set.begin(), expected_set.end(),
        [&](const std::string &name) { return !actual.contains(name); });
    if (missing != expected_set.end()) {
        fail("physical_plan_missing_reference",
             std::string{subject} + " is missing '" + *missing + "'");
    }
    const auto unknown = std::find_if(
        actual.begin(), actual.end(),
        [&](const std::string &name) { return !expected_set.contains(name); });
    fail("physical_plan_missing_reference",
         std::string{subject} + " contains unknown entry '" +
             (unknown == actual.end() ? std::string{"?"} : *unknown) + "'");
}

void requireSameSet(const std::set<std::string, std::less<>> &actual,
                    const std::vector<std::string> &expected,
                    std::string_view subject) {
    requireSameSet(actual, toSet(expected), subject);
}

void validateFingerprint(const Json &package, std::string_view key,
                         std::string_view expected,
                         std::string_view context) {
    const auto actual = requireString(package, key, context);
    if (actual != expected) {
        fail("physical_plan_fingerprint_mismatch",
             std::string{context} + "." + std::string{key} + " '" + actual +
                 "' does not match '" + std::string{expected} + "'");
    }
}

using Pair = std::pair<std::string, std::string>;

Pair normalizedPair(std::string first, std::string second) {
    if (second < first) {
        std::swap(first, second);
    }
    return {std::move(first), std::move(second)};
}

std::set<Pair> validateOpportunityPairs(
    const Json &report, std::string_view key,
    const std::set<std::string, std::less<>> &references,
    std::string_view reference_kind) {
    const auto &entries = requireArray(report, key, "planning_opportunities");
    std::set<Pair> pairs;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto context = "planning_opportunities." + std::string{key} +
                             "[" + std::to_string(index) + "]";
        if (!entries[index].is_object()) {
            fail("physical_plan_type_error", context + " must be an object");
        }
        auto first = requireString(entries[index], "first", context);
        auto second = requireString(entries[index], "second", context);
        if (first == second) {
            fail("physical_plan_duplicate", context + " repeats one member");
        }
        if (!references.contains(first) || !references.contains(second)) {
            fail("physical_plan_missing_reference",
                 context + " references an unknown " +
                     std::string{reference_kind});
        }
        if (!pairs.insert(normalizedPair(std::move(first), std::move(second)))
                 .second) {
            fail("physical_plan_duplicate",
                 "duplicate pair in planning_opportunities." +
                     std::string{key});
        }
    }
    return pairs;
}

void validateNestedPackage(const Json &plan, std::string_view key,
                           std::string_view schema, std::uint64_t version,
                           std::string_view graph,
                           std::string_view logical_fingerprint,
                           std::string_view automatic_fingerprint,
                           bool require_automatic) {
    const auto found = plan.find(std::string{key});
    if (found == plan.end() || found->is_null()) {
        fail("physical_plan_missing_field",
             "physical_target_plan requires object field '" +
                 std::string{key} + "'");
    }
    if (!found->is_object()) {
        fail("physical_plan_type_error",
             "physical_target_plan." + std::string{key} +
                 " must be an object");
    }
    const auto context = "physical_target_plan." + std::string{key};
    validateSchemaVersion(*found, schema, version, context);
    validateGraph(*found, graph, context);
    validateFingerprint(*found, "logical_graph_fingerprint",
                        logical_fingerprint, context);
    if (require_automatic) {
        validateFingerprint(*found, "automatic_plan_fingerprint",
                            automatic_fingerprint, context);
    }
}

void validatePlan(const Json &plan,
                  const PhysicalTargetPlanWireContext &external_context) {
    if (!plan.is_object()) {
        fail("physical_plan_type_error",
             "physical_target_plan must be an object");
    }
    validateSchemaVersion(plan, "pelican.vulkan_target_plan", 1,
                          "physical_target_plan");
    const auto graph = requireString(plan, "graph", "physical_target_plan");
    if (!external_context.expected_graph.empty() &&
        graph != external_context.expected_graph) {
        fail("physical_plan_graph_mismatch",
             "physical_target_plan graph '" + graph +
                 "' does not match frame plan graph '" +
                 external_context.expected_graph + "'");
    }
    const auto logical_fingerprint = requireString(
        plan, "logical_graph_fingerprint", "physical_target_plan");
    const auto automatic_fingerprint = requireString(
        plan, "automatic_plan_fingerprint", "physical_target_plan");

    validateNestedPackage(plan, "ejectable_pin_package",
                          "pelican.vulkan_target_plan_pins", 1, graph,
                          logical_fingerprint, automatic_fingerprint, false);
    validateNestedPackage(plan, "ejectable_physical_fragment",
                          "pelican.vulkan_physical_fragment", 3, graph,
                          logical_fingerprint, automatic_fingerprint, true);
    validateNestedPackage(plan, "ejectable_complete_physical_plan",
                          "pelican.vulkan_complete_physical_plan", 1, graph,
                          logical_fingerprint, automatic_fingerprint, true);

    requireArray(plan, "graph_transforms", "physical_target_plan");
    requireArray(plan, "subgraph_replacements", "physical_target_plan");
    requireStringArray(plan, "required_physical_features",
                       "physical_target_plan");
    requireArray(plan, "decisions", "physical_target_plan");
    requireObject(plan, "view_execution_plan", "physical_target_plan");

    const auto &backend =
        requireObject(plan, "backend_selection", "physical_target_plan");
    validateSchemaVersion(backend, "pelican.backend_selection", 1,
                          "backend_selection");
    const auto selected_candidate =
        requireString(backend, "selected_candidate", "backend_selection");
    const auto &backend_candidates =
        requireArray(backend, "candidates", "backend_selection");
    std::unordered_set<std::string> candidate_names;
    bool selected_found = false;
    for (std::size_t index = 0; index < backend_candidates.size(); ++index) {
        const auto context =
            "backend_selection.candidates[" + std::to_string(index) + "]";
        const auto &candidate = backend_candidates[index];
        const auto name = requireString(candidate, "candidate", context);
        if (!candidate_names.insert(name).second) {
            fail("physical_plan_duplicate",
                 "duplicate backend candidate '" + name + "'");
        }
        requireBool(candidate, "feasible", context);
        requireArray(candidate, "failures", context);
        requireArray(candidate, "diagnostics", context);
        selected_found = selected_found || name == selected_candidate;
    }
    if (!selected_found) {
        fail("physical_plan_missing_reference",
             "backend_selection.selected_candidate references an unknown "
             "candidate");
    }

    const auto &resources =
        requireArray(plan, "resources", "physical_target_plan");
    std::set<std::string, std::less<>> resource_names;
    std::unordered_map<std::string, bool> resource_aliasable;
    std::unordered_map<std::string, const Json *> resource_documents;
    for (std::size_t index = 0; index < resources.size(); ++index) {
        const auto context =
            "physical_target_plan.resources[" + std::to_string(index) + "]";
        const auto &resource = resources[index];
        const auto name =
            requireString(resource, "logical_resource", context);
        if (!resource_names.insert(name).second) {
            fail("physical_plan_duplicate",
                 "duplicate physical resource '" + name + "'");
        }
        resource_aliasable.emplace(name,
                                   requireBool(resource, "aliasable", context));
        resource_documents.emplace(name, &resource);
        requireString(resource, "pattern", context);
        requireString(resource, "format", context);
        requireString(resource, "representation", context);
        requireString(resource, "widest_read", context);
        validateLifetime(requireField(resource, "lifetime", context),
                         context + ".lifetime");
        requireBool(resource, "stored", context);
        requireString(resource, "view_layout", context);
        requireUnsigned(resource, "array_layers", context);
        requireString(resource, "dimension", context);
        const auto &mip_levels = requireObject(resource, "mip_levels", context);
        requireString(mip_levels, "mode", context + ".mip_levels");
        requireUnsigned(mip_levels, "count", context + ".mip_levels");
        if (const auto samples = resource.find("rasterization_samples");
            samples != resource.end()) {
            (void)requireUnsigned(resource, "rasterization_samples", context);
        }
        if (const auto resolve = resource.find("resolve_required");
            resolve != resource.end()) {
            (void)requireBool(resource, "resolve_required", context);
        }
        requireStringArray(resource, "required_physical_features", context);
        const auto &extent = requireField(resource, "extent", context);
        if (!extent.is_null()) {
            validateExtent(extent, context + ".extent");
        }
    }
    if (!external_context.logical_resources.empty()) {
        requireUniqueStrings(external_context.logical_resources,
                             "logical resource context");
        requireSameSet(resource_names, external_context.logical_resources,
                       "physical resources");
    }

    const auto &lowering =
        requireObject(plan, "lowering_graph", "physical_target_plan");
    validateSchemaVersion(lowering, "pelican.target_lowering_graph", 1,
                          "lowering_graph");
    validateGraph(lowering, graph, "lowering_graph");
    requireString(lowering, "stage", "lowering_graph");
    requireArray(lowering, "decisions", "lowering_graph");
    const auto &lowering_nodes =
        requireArray(lowering, "nodes", "lowering_graph");
    std::set<std::string, std::less<>> node_names;
    for (std::size_t index = 0; index < lowering_nodes.size(); ++index) {
        const auto context =
            "lowering_graph.nodes[" + std::to_string(index) + "]";
        const auto &node = lowering_nodes[index];
        const auto name = requireString(node, "name", context);
        if (!node_names.insert(name).second) {
            fail("physical_plan_duplicate",
                 "duplicate lowering node '" + name + "'");
        }
        requireString(node, "kind", context);
        requireString(node, "dialect", context);
        requireStringArray(node, "sources", context);
        requireStringArray(node, "regions", context);
        requireStringArray(node, "required_physical_features", context);
    }
    if (!external_context.execution_nodes.empty()) {
        requireUniqueStrings(external_context.execution_nodes,
                             "execution node context");
        requireSameSet(node_names, external_context.execution_nodes,
                       "lowering graph nodes");
    }
    const auto &lowering_resources =
        requireArray(lowering, "resources", "lowering_graph");
    std::set<std::string, std::less<>> lowering_resource_names;
    for (std::size_t index = 0; index < lowering_resources.size(); ++index) {
        const auto context =
            "lowering_graph.resources[" + std::to_string(index) + "]";
        const auto name =
            requireString(lowering_resources[index], "name", context);
        if (!lowering_resource_names.insert(name).second) {
            fail("physical_plan_duplicate",
                 "duplicate lowering resource '" + name + "'");
        }
    }
    if (lowering_resource_names != resource_names) {
        fail("physical_plan_missing_reference",
             "lowering graph resources do not match physical resources");
    }

    const auto &scopes = requireArray(plan, "scopes", "physical_target_plan");
    std::unordered_set<std::string> scope_ids;
    std::unordered_set<std::string> scoped_nodes;
    for (std::size_t index = 0; index < scopes.size(); ++index) {
        const auto context =
            "physical_target_plan.scopes[" + std::to_string(index) + "]";
        const auto &scope = scopes[index];
        const auto id = requireString(scope, "id", context);
        if (!scope_ids.insert(id).second) {
            fail("physical_plan_duplicate", "duplicate scope id '" + id + "'");
        }
        requireString(scope, "kind", context);
        const auto scope_nodes = requireStringArray(scope, "nodes", context);
        requireUniqueStrings(scope_nodes, "node in scope " + id);
        for (const auto &node : scope_nodes) {
            if (!node_names.contains(node)) {
                fail("physical_plan_missing_reference",
                     context + " references unknown node '" + node + "'");
            }
            if (!scoped_nodes.insert(node).second) {
                fail("physical_plan_duplicate",
                     "node '" + node + "' belongs to more than one scope");
            }
        }
        const auto local_reads =
            requireStringArray(scope, "local_reads", context);
        requireUniqueStrings(local_reads, "local read in scope " + id);
        for (const auto &resource : local_reads) {
            if (!resource_names.contains(resource)) {
                fail("physical_plan_missing_reference",
                     context + " references unknown local-read resource '" +
                         resource + "'");
            }
        }
        requireStringArray(scope, "regions", context);
        requireBool(scope, "single_rendering_instance", context);
        requireString(scope, "view_execution", context);
        requireUnsigned(scope, "view_count", context);
        requireUnsigned(scope, "execution_count", context);
        requireUnsigned(scope, "view_mask", context);
    }
    const std::set<std::string, std::less<>> scoped_node_names{
        scoped_nodes.begin(), scoped_nodes.end()};
    if (scoped_node_names != node_names) {
        fail("physical_plan_missing_reference",
             "physical scopes do not cover every lowering node exactly once");
    }

    if (const auto attachments = plan.find("attachments");
        attachments != plan.end()) {
        if (!attachments->is_array()) {
            fail("physical_plan_type_error",
                 "physical_target_plan.attachments must be an array");
        }
        std::set<std::pair<std::string, std::string>> identities;
        for (std::size_t index = 0; index < attachments->size(); ++index) {
            const auto context = "physical_target_plan.attachments[" +
                                 std::to_string(index) + "]";
            const auto &attachment = attachments->at(index);
            const auto node = requireString(attachment, "node", context);
            const auto resource =
                requireString(attachment, "logical_resource", context);
            requireString(attachment, "aspect", context);
            requireString(attachment, "load_op", context);
            requireString(attachment, "store_op", context);
            if (!node_names.contains(node)) {
                fail("physical_plan_missing_reference",
                     context + " references unknown node '" + node + "'");
            }
            if (!resource_names.contains(resource)) {
                fail("physical_plan_missing_reference",
                     context + " references unknown resource '" + resource +
                         "'");
            }
            if (!identities.emplace(node, resource).second) {
                fail("physical_plan_duplicate",
                     "duplicate physical attachment for node/resource");
            }
        }
    }

    const auto &opportunities = requireObject(
        plan, "planning_opportunities", "physical_target_plan");
    validateSchemaVersion(opportunities,
                          "pelican.logical_planning_opportunities", 1,
                          "planning_opportunities");
    validateGraph(opportunities, graph, "planning_opportunities");
    requireString(opportunities, "profile", "planning_opportunities");
    requireArray(opportunities, "decisions", "planning_opportunities");
    const auto node_order = requireStringArray(
        opportunities, "node_order", "planning_opportunities");
    requireUniqueStrings(node_order, "planning node order entry");
    requireSameSet(toSet(node_order), node_names, "planning node order");
    (void)validateOpportunityPairs(opportunities, "alias_candidates",
                                   resource_names, "resource");
    (void)validateOpportunityPairs(opportunities, "fusion_candidates",
                                   node_names, "node");
    (void)validateOpportunityPairs(opportunities, "parallel_candidates",
                                   node_names, "node");

    const auto &alias_groups =
        requireArray(plan, "alias_groups", "physical_target_plan");
    std::unordered_set<std::string> alias_group_ids;
    std::unordered_map<std::string, std::string> memberships;
    for (std::size_t index = 0; index < alias_groups.size(); ++index) {
        const auto context = "physical_target_plan.alias_groups[" +
                             std::to_string(index) + "]";
        const auto &group = alias_groups[index];
        const auto id = requireString(group, "id", context);
        if (!alias_group_ids.insert(id).second) {
            fail("physical_plan_duplicate",
                 "duplicate alias group id '" + id + "'");
        }
        const auto members = requireStringArray(group, "resources", context);
        requireUniqueStrings(members, "resource in alias group " + id);
        if (members.size() < 2) {
            fail("physical_plan_alias_membership_conflict",
                 "alias group '" + id + "' requires at least two resources");
        }
        for (const auto &member : members) {
            if (!resource_names.contains(member)) {
                fail("physical_plan_missing_reference",
                     context + " references unknown resource '" + member +
                         "'");
            }
            if (!resource_aliasable.at(member)) {
                fail("physical_plan_alias_membership_conflict",
                     "non-aliasable resource '" + member +
                         "' belongs to alias group '" + id + "'");
            }
            const auto [found, inserted] = memberships.emplace(member, id);
            if (!inserted) {
                fail("physical_plan_alias_membership_conflict",
                     "resource '" + member + "' belongs to both alias group '" +
                         found->second + "' and '" + id + "'");
            }
        }
        for (std::size_t left = 0; left < members.size(); ++left) {
            for (std::size_t right = left + 1; right < members.size(); ++right) {
                const auto &left_resource =
                    *resource_documents.at(members[left]);
                const auto &right_resource =
                    *resource_documents.at(members[right]);
                if (!aliasContractsMatch(left_resource, right_resource)) {
                    fail("physical_plan_alias_membership_conflict",
                         "alias group '" + id +
                             "' contains incompatible resource contracts");
                }
                if (!aliasLifetimesDoNotOverlap(left_resource,
                                                right_resource)) {
                    fail("physical_plan_alias_membership_conflict",
                         "alias group '" + id +
                             "' contains overlapping resource lifetimes");
                }
            }
        }
    }

    if (const auto resolution = plan.find("resolution_plan");
        resolution != plan.end() && !resolution->is_null()) {
        if (!resolution->is_object()) {
            fail("physical_plan_type_error",
                 "physical_target_plan.resolution_plan must be an object");
        }
        const auto render_source = requireString(
            *resolution, "render_source_resource", "resolution_plan");
        const auto output_source = requireString(
            *resolution, "output_source_resource", "resolution_plan");
        if (!resource_names.contains(render_source) ||
            !resource_names.contains(output_source)) {
            fail("physical_plan_missing_reference",
                 "resolution_plan references an unknown source resource");
        }
        validateExtent(requireField(*resolution, "render_extent",
                                    "resolution_plan"),
                       "resolution_plan.render_extent");
        validateExtent(requireField(*resolution, "output_extent",
                                    "resolution_plan"),
                       "resolution_plan.output_extent");
        const auto scene_resources = requireStringArray(
            *resolution, "scene_resources", "resolution_plan");
        requireUniqueStrings(scene_resources, "resolution scene resource");
        for (const auto &resource : scene_resources) {
            if (!resource_names.contains(resource)) {
                fail("physical_plan_missing_reference",
                     "resolution_plan.scene_resources references unknown "
                     "resource '" +
                         resource + "'");
            }
        }
    }
}

} // namespace

std::string PhysicalTargetPlanWireValidation::reason() const {
    if (reason_code.empty()) {
        return detail;
    }
    if (detail.empty()) {
        return reason_code;
    }
    return reason_code + ": " + detail;
}

PhysicalTargetPlanWireValidation validatePhysicalTargetPlanWire(
    const nlohmann::json *document,
    const PhysicalTargetPlanWireContext &context) {
    if (document == nullptr || document->is_null()) {
        return {
            .state = PhysicalTargetPlanWireState::unavailable,
            .reason_code = "physical_plan_missing",
            .detail = "physical_target_plan was not published",
        };
    }
    try {
        validatePlan(*document, context);
        return {.state = PhysicalTargetPlanWireState::available};
    } catch (const ValidationFailure &failure) {
        return {
            .state = PhysicalTargetPlanWireState::unavailable,
            .reason_code = failure.code,
            .detail = failure.message,
        };
    } catch (const std::exception &error) {
        return {
            .state = PhysicalTargetPlanWireState::unavailable,
            .reason_code = "physical_plan_malformed",
            .detail = error.what(),
        };
    }
}

std::string PlanningOpportunitiesWire::reason() const {
    if (reason_code.empty()) {
        return detail;
    }
    if (detail.empty()) {
        return reason_code;
    }
    return reason_code + ": " + detail;
}

PlanningOpportunitiesWire readPlanningOpportunitiesWire(
    const nlohmann::json *document,
    const PhysicalTargetPlanWireContext &context) {
    const auto validation = validatePhysicalTargetPlanWire(document, context);
    if (!validation.available()) {
        return {
            .state = PhysicalTargetPlanWireState::unavailable,
            .reason_code = validation.reason_code,
            .detail = validation.detail,
        };
    }

    try {
        const auto &plan = *document;
        const auto &report = requireObject(
            plan, "planning_opportunities", "physical_target_plan");

        const auto collection_contains_pair =
            [&plan](std::string_view collection_key,
                    std::string_view members_key, std::string_view first,
                    std::string_view second, bool require_single_instance) {
            const auto &collection = requireArray(
                plan, collection_key, "physical_target_plan");
            for (std::size_t index = 0; index < collection.size(); ++index) {
                const auto entry_context =
                    "physical_target_plan." + std::string{collection_key} +
                    "[" + std::to_string(index) + "]";
                const auto &entry = collection[index];
                if (!entry.is_object()) {
                    fail("physical_plan_type_error",
                         entry_context + " must be an object");
                }
                if (require_single_instance &&
                    !requireBool(entry, "single_rendering_instance",
                                 entry_context)) {
                    continue;
                }
                const auto members = requireStringArray(
                    entry, members_key, entry_context);
                const auto contains = [&](std::string_view name) {
                    return std::find(members.begin(), members.end(), name) !=
                           members.end();
                };
                if (contains(first) && contains(second)) {
                    return true;
                }
            }
            return false;
        };

        const auto read_pairs = [&](std::string_view key, auto &&adoption) {
            const auto &entries = requireArray(
                report, key, "planning_opportunities");
            std::vector<PlanningOpportunityWirePair> pairs;
            pairs.reserve(entries.size());
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto entry_context =
                    "planning_opportunities." + std::string{key} + "[" +
                    std::to_string(index) + "]";
                if (!entries[index].is_object()) {
                    fail("physical_plan_type_error",
                         entry_context + " must be an object");
                }
                PlanningOpportunityWirePair pair{
                    .first = requireString(entries[index], "first",
                                           entry_context),
                    .second = requireString(entries[index], "second",
                                            entry_context),
                };
                pair.adoption = adoption(pair.first, pair.second);
                pairs.push_back(std::move(pair));
            }
            return pairs;
        };

        PlanningOpportunitiesWire result{
            .state = PhysicalTargetPlanWireState::available,
            .profile = requireString(report, "profile",
                                     "planning_opportunities"),
        };
        result.alias_candidates = read_pairs(
            "alias_candidates", [&](std::string_view first,
                                    std::string_view second) {
                return collection_contains_pair(
                           "alias_groups", "resources", first, second, false)
                           ? PlanningOpportunityAdoption::adopted
                           : PlanningOpportunityAdoption::not_adopted;
            });
        result.fusion_candidates = read_pairs(
            "fusion_candidates", [&](std::string_view first,
                                     std::string_view second) {
                return collection_contains_pair(
                           "scopes", "nodes", first, second, true)
                           ? PlanningOpportunityAdoption::adopted
                           : PlanningOpportunityAdoption::not_adopted;
            });
        result.parallel_candidates = read_pairs(
            "parallel_candidates", [](std::string_view, std::string_view) {
                // No validated physical schedule is present in v1.  A legal
                // opportunity alone is not evidence of either adoption state.
                return PlanningOpportunityAdoption::unknown;
            });
        return result;
    } catch (const ValidationFailure &failure) {
        return {
            .state = PhysicalTargetPlanWireState::unavailable,
            .reason_code = failure.code,
            .detail = failure.message,
        };
    } catch (const std::exception &error) {
        return {
            .state = PhysicalTargetPlanWireState::unavailable,
            .reason_code = "physical_plan_malformed",
            .detail = error.what(),
        };
    }
}

} // namespace Pelican
