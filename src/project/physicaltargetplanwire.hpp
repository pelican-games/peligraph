#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace Pelican {

// Shared availability contract for consumers of the serialized
// pelican.vulkan_target_plan.  A missing document and a valid document with
// empty collections are deliberately different states.
enum class PhysicalTargetPlanWireState {
    unavailable,
    available,
};

struct PhysicalTargetPlanWireContext {
    std::string expected_graph;
    std::vector<std::string> execution_nodes;
    std::vector<std::string> logical_resources;
};

struct PhysicalTargetPlanWireValidation {
    PhysicalTargetPlanWireState state =
        PhysicalTargetPlanWireState::unavailable;
    std::string reason_code;
    std::string detail;

    [[nodiscard]] bool available() const noexcept {
        return state == PhysicalTargetPlanWireState::available;
    }
    [[nodiscard]] std::string reason() const;

    bool operator==(const PhysicalTargetPlanWireValidation &) const = default;
};

// `document == nullptr` represents a producer that did not publish a physical
// plan.  Validation is failure-closed and never throws: malformed documents
// become unavailable with a stable, named reason code shared by Studio and the
// in-engine ImGui viewer.
PhysicalTargetPlanWireValidation validatePhysicalTargetPlanWire(
    const nlohmann::json *document,
    const PhysicalTargetPlanWireContext &context = {});

} // namespace Pelican
