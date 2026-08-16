#include "executionplanwire.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <exception>
#include <set>
#include <string_view>
#include <tuple>
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
        fail("execution_plan_type_error",
             std::string{context} + " must be an object");
    }
    const auto found = object.find(std::string{key});
    if (found == object.end()) {
        fail("execution_plan_missing_field",
             std::string{context} + " requires field '" +
                 std::string{key} + "'");
    }
    return *found;
}

const Json &requireArray(const Json &object, std::string_view key,
                         std::string_view context) {
    const auto &value = requireField(object, key, context);
    if (!value.is_array()) {
        fail("execution_plan_type_error",
             std::string{context} + "." + std::string{key} +
                 " must be an array");
    }
    return value;
}

std::string requireString(const Json &object, std::string_view key,
                          std::string_view context) {
    const auto &value = requireField(object, key, context);
    if (!value.is_string()) {
        fail("execution_plan_type_error",
             std::string{context} + "." + std::string{key} +
                 " must be a string");
    }
    const auto result = value.get<std::string>();
    if (result.empty()) {
        fail("execution_plan_type_error",
             std::string{context} + "." + std::string{key} +
                 " must not be empty");
    }
    return result;
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
    fail("execution_plan_type_error",
         std::string{context} + "." + std::string{key} +
             " must be a non-negative integer");
}

std::string optionalString(const Json &object, std::string_view key,
                           std::string_view context) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || found->is_null()) {
        return {};
    }
    if (!found->is_string()) {
        fail("execution_plan_type_error",
             std::string{context} + "." + std::string{key} +
                 " must be a string");
    }
    return found->get<std::string>();
}

void validateStringArray(const Json &object, std::string_view key,
                         std::string_view context) {
    const auto found = object.find(std::string{key});
    if (found == object.end()) {
        return;
    }
    if (!found->is_array()) {
        fail("execution_plan_type_error",
             std::string{context} + "." + std::string{key} +
                 " must be an array");
    }
    for (std::size_t index = 0; index < found->size(); ++index) {
        if (!found->at(index).is_string()) {
            fail("execution_plan_type_error",
                 std::string{context} + "." + std::string{key} + "[" +
                     std::to_string(index) + "] must be a string");
        }
    }
}

void validateResourceUses(
    const Json &node, std::string_view context,
    const std::unordered_set<std::string> &logical_resources) {
    const auto found = node.find("resource_uses");
    if (found == node.end()) {
        return;
    }
    if (!found->is_array()) {
        fail("execution_plan_type_error",
             std::string{context} + ".resource_uses must be an array");
    }
    for (std::size_t index = 0; index < found->size(); ++index) {
        const auto &use = found->at(index);
        const std::string use_context = std::string{context} +
                                        ".resource_uses[" +
                                        std::to_string(index) + "]";
        const auto resource = requireString(use, "resource", use_context);
        if (!logical_resources.empty() &&
            !logical_resources.contains(resource)) {
            fail("execution_plan_missing_reference",
                 use_context + " references unknown resource '" + resource +
                     "'");
        }
        (void)optionalString(use, "epoch", use_context);
        (void)optionalString(use, "access", use_context);
        (void)optionalString(use, "intent", use_context);
        const auto footprint = use.find("footprint");
        if (footprint == use.end() || footprint->is_null() ||
            footprint->is_string()) {
            continue;
        }
        if (!footprint->is_object()) {
            fail("execution_plan_type_error",
                 use_context + ".footprint must be a string or object");
        }
        (void)optionalString(*footprint, "kind", use_context + ".footprint");
    }
}

void validatePlan(const Json &document,
                  const ExecutionPlanWireContext &context) {
    if (!document.is_object()) {
        fail("execution_plan_type_error",
             "execution_plan must be an object");
    }
    const auto schema =
        requireString(document, "schema", "execution_plan");
    if (schema != "pelican.frame_execution_plan") {
        fail("execution_plan_schema_mismatch",
             "execution_plan requires schema 'pelican.frame_execution_plan', "
             "got '" +
                 schema + "'");
    }
    const auto version =
        requireUnsigned(document, "schema_version", "execution_plan");
    if (version != 1) {
        fail("execution_plan_version_mismatch",
             "execution_plan requires schema version 1, got " +
                 std::to_string(version));
    }
    const auto graph =
        requireString(document, "graph", "execution_plan");
    if (!context.expected_graph.empty() &&
        graph != context.expected_graph) {
        fail("execution_plan_graph_mismatch",
             "execution_plan graph '" + graph + "' does not match '" +
                 context.expected_graph + "'");
    }
    (void)requireString(document, "fingerprint", "execution_plan");
    (void)requireArray(document, "endpoints", "execution_plan");
    (void)requireArray(document, "bridges", "execution_plan");

    const std::unordered_set<std::string> expected_nodes{
        context.frame_nodes.begin(), context.frame_nodes.end()};
    const std::unordered_set<std::string> logical_resources{
        context.logical_resources.begin(), context.logical_resources.end()};
    const auto &nodes = requireArray(document, "nodes", "execution_plan");
    std::unordered_set<std::string> seen_nodes;
    seen_nodes.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto &node = nodes[index];
        const std::string node_context =
            "execution_plan.nodes[" + std::to_string(index) + "]";
        const auto name = requireString(node, "name", node_context);
        if (!seen_nodes.insert(name).second) {
            fail("execution_plan_duplicate",
                 "duplicate execution node '" + name + "'");
        }
        if (!expected_nodes.empty() && !expected_nodes.contains(name)) {
            fail("execution_plan_missing_reference",
                 "execution node '" + name +
                     "' is absent from the frame plan nodes");
        }
        (void)optionalString(node, "semantic_dialect", node_context);
        (void)optionalString(node, "selected_implementation", node_context);
        (void)optionalString(node, "selected_endpoint", node_context);
        validateStringArray(node, "required_capabilities", node_context);
        validateResourceUses(node, node_context, logical_resources);
    }
    if (!expected_nodes.empty() && seen_nodes != expected_nodes) {
        for (const auto &name : expected_nodes) {
            if (!seen_nodes.contains(name)) {
                fail("execution_plan_missing_reference",
                     "frame node '" + name +
                         "' is absent from execution_plan.nodes");
            }
        }
    }

    const auto &dependencies =
        requireArray(document, "dependencies", "execution_plan");
    std::set<std::tuple<std::string, std::string, std::string, std::string>>
        seen_dependencies;
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        const auto &dependency = dependencies[index];
        const std::string dependency_context =
            "execution_plan.dependencies[" + std::to_string(index) + "]";
        const auto from =
            requireString(dependency, "from", dependency_context);
        const auto to = requireString(dependency, "to", dependency_context);
        const auto reason =
            requireString(dependency, "reason", dependency_context);
        const auto resource =
            optionalString(dependency, "resource", dependency_context);
        if (!seen_nodes.contains(from) || !seen_nodes.contains(to)) {
            fail("execution_plan_missing_reference",
                 dependency_context +
                     " references an unknown execution node");
        }
        if (!resource.empty() && !logical_resources.empty() &&
            !logical_resources.contains(resource)) {
            fail("execution_plan_missing_reference",
                 dependency_context + " references unknown resource '" +
                     resource + "'");
        }
        if (!seen_dependencies.emplace(from, to, reason, resource).second) {
            fail("execution_plan_duplicate", "duplicate dependency");
        }
    }
}

} // namespace

std::string ExecutionPlanWireValidation::reason() const {
    if (reason_code.empty()) {
        return detail;
    }
    if (detail.empty()) {
        return reason_code;
    }
    return reason_code + ": " + detail;
}

ExecutionPlanWireValidation validateExecutionPlanWire(
    const nlohmann::json *document,
    const ExecutionPlanWireContext &context) {
    if (document == nullptr || document->is_null()) {
        return {
            .state = ExecutionPlanWireState::unavailable,
            .reason_code = "execution_plan_missing",
            .detail = "execution_plan was not published",
        };
    }
    try {
        validatePlan(*document, context);
        return {.state = ExecutionPlanWireState::available};
    } catch (const ValidationFailure &failure) {
        return {
            .state = ExecutionPlanWireState::unavailable,
            .reason_code = failure.code,
            .detail = failure.message,
        };
    } catch (const std::exception &error) {
        return {
            .state = ExecutionPlanWireState::unavailable,
            .reason_code = "execution_plan_malformed",
            .detail = error.what(),
        };
    }
}

} // namespace Pelican
