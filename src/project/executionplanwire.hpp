#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace Pelican {

// Shared availability contract for consumers of the serialized
// pelican.frame_execution_plan. A missing document and a valid document with
// no dependencies are deliberately different states.
enum class ExecutionPlanWireState {
    unavailable,
    available,
};

struct ExecutionPlanWireContext {
    std::string expected_graph;
    std::vector<std::string> frame_nodes;
    std::vector<std::string> logical_resources;
};

struct ExecutionPlanWireValidation {
    ExecutionPlanWireState state = ExecutionPlanWireState::unavailable;
    std::string reason_code;
    std::string detail;

    [[nodiscard]] bool available() const noexcept {
        return state == ExecutionPlanWireState::available;
    }
    [[nodiscard]] std::string reason() const;

    bool operator==(const ExecutionPlanWireValidation &) const = default;
};

// `document == nullptr` represents a producer that did not publish an
// execution plan. Validation is failure-closed and never throws: missing or
// malformed documents become unavailable with a stable, named reason code.
ExecutionPlanWireValidation validateExecutionPlanWire(
    const nlohmann::json *document,
    const ExecutionPlanWireContext &context = {});

} // namespace Pelican
