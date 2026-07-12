#pragma once

#include "../../project/materiallowering.hpp"

#include <cstddef>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>

namespace Pelican {

struct PlanViewerNode {
    std::string name;
    std::string kind;
    std::size_t order = 0;
    std::size_t level = 0;
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    std::string source;
    std::string feature;
    std::string anchor;
    std::string color_load_op;
    std::string color_store_op;
    std::string depth_load_op;
    std::string depth_store_op;
    std::string snapshot_after;
    std::size_t byte_size = 0;
};

struct PlanViewerEdge {
    std::string kind;
    std::string resource;
    std::string from;
    std::string to;
};

struct PlanViewerResource {
    std::string name;
    std::string format;
    std::string format_class;
    std::vector<std::string> readers;
    std::vector<std::string> writers;
};

struct PlanViewerMaterial {
    std::string name;
    std::string surface;
    std::string surface_stem;
    std::vector<std::string> screen_inputs;
    std::string target_pass;
    std::string render_state;
};

struct PlanViewerModel {
    std::string graph;
    std::vector<PlanViewerNode> nodes;
    std::vector<PlanViewerEdge> edges;
    std::vector<PlanViewerResource> resources;
    std::vector<PlanViewerMaterial> materials;
};

// plan_json is the public pelican.frame_plan v1 contract. annotations only add
// display metadata and never add, remove, or reorder plan nodes or edges.
PlanViewerModel buildPlanViewerModel(const nlohmann::json &plan_json,
                                     const nlohmann::json &annotations,
                                     std::span<const LoweredMaterial> materials = {});

class PlanViewer {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    PlanViewer();
    ~PlanViewer();
    void draw(bool *open);
};

} // namespace Pelican
