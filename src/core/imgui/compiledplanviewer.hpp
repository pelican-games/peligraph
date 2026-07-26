#pragma once

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {

// Compiled Pass Inspector.
//
// Shows what the render graph compiler actually produced for the currently
// published runtime generation: execution order, barriers, resource bindings,
// material routes, and the versioned `pelican.vulkan_target_plan` document.
//
// Data acquisition rule: this viewer only calls existing read-only public
// surfaces (FrameGraphRuntimeContainer::snapshot and the plan's own JSON
// serializer). It never reaches into compiler internals and never mutates
// runtime state, so the render-graph work can keep changing plan internals
// without breaking or conflicting with this panel. Unknown plan fields stay
// visible through the raw JSON tree instead of silently disappearing.

struct CompiledPlanBarrierRow {
    std::string resource;
    std::string from_node;
    std::string from_kind;
    std::string to_kind;
};

struct CompiledPlanNodeRow {
    std::string name;
    std::string kind;
    std::size_t index = 0;
    std::vector<CompiledPlanBarrierRow> incoming_barriers;
};

struct CompiledPlanBindingRow {
    std::string logical_name;
    std::string binding;
    std::string kind; // render_target / buffer
};

struct CompiledPlanRouteRow {
    std::string route;
    std::string pass_contract;
    std::string pass_id;
};

struct CompiledPlanFact {
    std::string key;
    std::string value;
};

// One published render program (one graph variant: flat / xr / preview ...).
struct CompiledPlanProgram {
    std::string variant;
    std::string owner_scope;
    std::string rendering_pass_id;
    bool has_target_plan = false;
    std::vector<CompiledPlanFact> facts;
    std::vector<CompiledPlanNodeRow> nodes;
    std::vector<CompiledPlanBindingRow> bindings;
    std::vector<CompiledPlanRouteRow> routes;
    std::vector<std::string> planning_opportunities;
    nlohmann::ordered_json target_plan_json;
};

struct CompiledPlanModel {
    std::uint64_t generation = 0;
    std::vector<std::string> enabled_features;
    std::vector<CompiledPlanProgram> programs;
    std::string error;
};

// Pure transformation, unit-testable without a live runtime. `plan_json` is the
// `pelican.vulkan_target_plan` document (may be null/empty when a variant was
// compiled without one).
std::vector<CompiledPlanFact> buildCompiledPlanFacts(
    const nlohmann::json &plan_json);
std::vector<std::string> buildCompiledPlanOpportunities(
    const nlohmann::json &plan_json);

// Snapshots the currently published runtime generation.
CompiledPlanModel buildCompiledPlanModel();

class CompiledPlanViewer {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    CompiledPlanViewer();
    ~CompiledPlanViewer();
    void draw(bool *open);
};

} // namespace Pelican
