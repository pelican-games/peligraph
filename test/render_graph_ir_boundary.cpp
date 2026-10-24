// RC-3c1a: the render graph's planning path with only pelican_project
// linked: a normal-form config through parseRenderGraph, the engine's
// additions (namespacing, the acceleration-structure builds, frame_node_order),
// planRenderGraph and toLogicalGraph. A link error here means the path still
// needs pelican_core.
#include "rendergraphparse.hpp"
#include "renderingjsonnormalize.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void require(bool condition, const std::string &what) {
    if (!condition) throw std::runtime_error("render_graph_ir_boundary: " + what);
}

} // namespace

int main() {
    try {
        using namespace Pelican;
        const auto config = normalizeRenderingJson(nlohmann::json::parse(R"json({
          "render_targets": [
            {"name": "scene", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
             "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
            {"name": "accum", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT",
             "usage": ["COLOR_ATTACHMENT", "SAMPLED"], "history": true},
            {"name": "debug_out", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",
             "usage": ["COLOR_ATTACHMENT", "SAMPLED"]}],
          "buffers": ["particles"],
          "compute_tasks": [{"name": "simulate", "writes": ["particles"]}],
          "frame_node_order": {"main": ["simulate#xr", "accumulate"]},
          "rendering_passes": [{"name": "main", "passes": [
            {"name": "draw", "type": "fullscreen", "input": ["particles"],
             "ray_query": true, "output": {"color": "scene", "depth": null}},
            {"name": "overlay", "type": "fullscreen", "color_load_op": "Load",
             "output": {"color": "scene", "depth": null}},
            {"name": "accumulate", "type": "fullscreen", "input": ["scene", "accum@history"],
             "output": {"color": "accum", "depth": null}},
            {"name": "present", "type": "fullscreen", "input": ["accum"],
             "output": {"color": "swapchain", "depth": null}},
            {"name": "debug", "type": "fullscreen",
             "output": {"color": "debug_out", "depth": null}}]}]
        })json"));

        auto ir = parseRenderGraph(config, PassFieldOwnershipCapabilities{});
        ir = namespaceSharedComputeTasks(std::move(ir), "#xr");
        ir = insertAccelerationStructureNodes(std::move(ir), AccelerationStructureNodeRequest{});
        ir = applyFrameNodeOrder(std::move(ir), true);
        require(ir.graphs.size() == 1, "one graph");
        const auto &graph = ir.graphs.front();

        const auto plan = planRenderGraph(ir, graph);
        validateFramePlanStructure(plan);
        const auto order = framePlanOrder(plan);
        // The build first (draw queries the scene), the task (named for the
        // variant) where frame_node_order puts it: before accumulate.
        const std::vector<std::string> expected{"__scene_acceleration_structure", "draw",
                                                "overlay", "debug", "simulate#xr",
                                                "accumulate", "present"};
        require(order == expected, "plan order");
        std::unordered_map<std::string, std::vector<std::string>> usage;
        for (const auto &image : ir.images) usage[image.name] = imageUsageNames(image.usage);
        const auto unread = findFramePlanUnreadOutputs(plan, usage);
        require(unread == std::vector<FramePlanUnreadOutput>{{"debug", "debug_out"}},
                "the one output nothing reads");
        const auto overlay = std::find_if(plan.nodes.begin(), plan.nodes.end(),
                                          [](const auto &node) { return node.name == "overlay"; });
        require(overlay != plan.nodes.end() && overlay->reads == std::vector<std::string>{"scene"},
                "a loading attachment reads what it writes");

        const auto logical = toLogicalGraph(ir, graph);
        require(logical.nodes.size() == graph.nodes.size(), "logical nodes");
        const auto accum = std::find_if(logical.resources.begin(), logical.resources.end(),
                                        [](const auto &resource) { return resource.name == "accum"; });
        require(accum != logical.resources.end() &&
                    accum->materialization == LogicalMaterializationRequirement::required,
                "a history image is materialized");
        std::cout << "render_graph_ir_boundary: " << order.size() << " nodes planned, "
                  << logical.resources.size() << " logical resources\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
