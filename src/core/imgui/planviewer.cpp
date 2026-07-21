#include "planviewer.hpp"

#include "imguiruntime.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../renderingpass/rendertargetjsonparser.hpp"
#include "../vkcore/renderer.hpp"
#include "../vkcore/rendertarget.hpp"
#include "../../project/renderpipeline.hpp"
#include "../../project/materialformat.hpp"
#include "../../project/surfaceformat.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pelican {
namespace {

std::string blendName(SurfaceBlendMode mode) {
    switch (mode) {
    case SurfaceBlendMode::opaque: return "opaque";
    case SurfaceBlendMode::blend: return "blend";
    case SurfaceBlendMode::additive: return "additive";
    }
    return "unknown";
}

std::string cullName(SurfaceCullMode mode) {
    switch (mode) {
    case SurfaceCullMode::none: return "none";
    case SurfaceCullMode::front: return "front";
    case SurfaceCullMode::back: return "back";
    }
    return "unknown";
}

std::string depthCompareName(SurfaceDepthCompare mode) {
    switch (mode) {
    case SurfaceDepthCompare::never: return "never";
    case SurfaceDepthCompare::less: return "less";
    case SurfaceDepthCompare::equal: return "equal";
    case SurfaceDepthCompare::less_equal: return "less_equal";
    case SurfaceDepthCompare::greater: return "greater";
    case SurfaceDepthCompare::not_equal: return "not_equal";
    case SurfaceDepthCompare::greater_equal: return "greater_equal";
    case SurfaceDepthCompare::always: return "always";
    }
    return "unknown";
}

std::string renderStateString(const SurfaceRenderState &state) {
    return "blend=" + blendName(state.blend) + " cull=" + cullName(state.cull) +
           " depth_test=" + (state.depth_test ? "true" : "false") +
           " depth_write=" + (state.depth_write ? "true" : "false") +
           " compare=" + depthCompareName(state.depth_compare);
}

std::string surfaceStem(std::string_view reference) {
    const auto slash = reference.find_last_of("/\\");
    auto stem = std::string{reference.substr(slash == std::string_view::npos ? 0 : slash + 1)};
    constexpr std::string_view suffix = ".surface";
    if (stem.ends_with(suffix)) stem.resize(stem.size() - suffix.size());
    return stem;
}

std::vector<std::string> stringArray(const nlohmann::json &value, std::string_view field) {
    if (!value.contains(field)) return {};
    const auto &array = value.at(field);
    if (!array.is_array()) {
        throw std::runtime_error("plan viewer field '" + std::string{field} + "' must be an array");
    }
    return array.get<std::vector<std::string>>();
}

bool contains(const std::vector<std::string> &values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

const nlohmann::json *objectEntry(const nlohmann::json &root, std::string_view group,
                                  const std::string &name) {
    if (!root.contains(group) || !root.at(group).is_object()) return nullptr;
    const auto &entries = root.at(group);
    const auto found = entries.find(name);
    return found == entries.end() || !found->is_object() ? nullptr : &*found;
}

std::string readTextFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios_base::binary};
    if (!input.is_open()) throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void collectNamedNodes(const nlohmann::json &feature, const std::string &feature_name,
                       std::unordered_map<std::string, std::string> &origins) {
    if (feature.contains("passes") && feature.at("passes").is_array()) {
        for (const auto &entry : feature.at("passes")) {
            if (entry.is_object() && entry.contains("pass") && entry.at("pass").is_object()) {
                const auto name = entry.at("pass").value("name", std::string{});
                if (!name.empty()) origins[name] = feature_name;
            }
        }
    }
    if (feature.contains("compute_tasks") && feature.at("compute_tasks").is_array()) {
        for (const auto &task : feature.at("compute_tasks")) {
            const auto name = task.is_object() ? task.value("name", std::string{}) : std::string{};
            if (!name.empty()) origins[name] = feature_name;
        }
    }
}

std::unordered_set<std::string> authoredNodeNames(const nlohmann::json &config) {
    std::unordered_set<std::string> names;
    if (config.contains("rendering_passes") && config.at("rendering_passes").is_array()) {
        for (const auto &graph : config.at("rendering_passes")) {
            if (!graph.is_object() || !graph.contains("passes") || !graph.at("passes").is_array()) continue;
            for (const auto &pass : graph.at("passes")) {
                const auto name = pass.is_object() ? pass.value("name", std::string{}) : std::string{};
                if (!name.empty()) names.insert(name);
            }
        }
    }
    if (config.contains("compute_tasks") && config.at("compute_tasks").is_array()) {
        for (const auto &task : config.at("compute_tasks")) {
            const auto name = task.is_object() ? task.value("name", std::string{}) : std::string{};
            if (!name.empty()) names.insert(name);
        }
    }
    return names;
}

std::string normalizedOp(const nlohmann::json &pass, std::string_view key,
                         std::string_view fallback) {
    if (!pass.contains(key) || !pass.at(key).is_string()) return std::string{fallback};
    auto value = pass.at(key).get<std::string>();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

nlohmann::json buildRuntimeAnnotations() {
    const auto raw = nlohmann::json::parse(GET_MODULE(ProjectBasicConfig).renderingConfigJson());
    auto &resolver = GET_MODULE(PathResolver);
    const auto authored = authoredNodeNames(raw);
    std::unordered_map<std::string, std::string> feature_origins;

    if (raw.contains("features") && raw.at("features").is_array()) {
        for (const auto &ref_json : raw.at("features")) {
            if (!ref_json.is_string()) continue;
            const auto ref = ref_json.get<std::string>();
            const auto feature = nlohmann::json::parse(resolver.loadText(ref));
            const auto feature_name = feature.value("name", ref);
            collectNamedNodes(feature, feature_name, feature_origins);
        }
    }

    const auto swapchain_format = GET_MODULE(RenderTarget).getSwapchainFormat();
    const auto target_extent = GET_MODULE(RenderTarget).getExtent();
    auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{raw, "plan viewer annotations"},
        RenderEnvironmentCapabilities{true,
                                      RenderPipelineGraphVariant::flat},
        RenderPipelineResolveDependencies{
            .load_feature_json = [&resolver](std::string_view ref) {
                return resolver.loadText(ref);
            },
            .load_pipeline_json = [&resolver](std::string_view ref) {
                return resolver.loadText(ref);
            },
            .normalize_config = [swapchain_format, target_extent](
                const nlohmann::json &config,
                const std::vector<std::string> &feature_names) {
                const bool hdr =
                    std::find(feature_names.begin(), feature_names.end(),
                              "hdr") != feature_names.end();
                return resolveRenderTargetFormatClassesV2(
                    config, swapchain_format, target_extent, hdr);
            },
        });
    auto composed = std::move(resolved.normalized_config);
    appendImGuiPassToCanonicalGraphs(composed);

    nlohmann::json annotations{{"nodes", nlohmann::json::object()},
                               {"resources", nlohmann::json::object()}};
    if (composed.contains("render_targets") && composed.at("render_targets").is_array()) {
        for (const auto &target : composed.at("render_targets")) {
            if (!target.is_object()) continue;
            const auto name = target.value("name", std::string{});
            if (name.empty()) continue;
            annotations["resources"][name] = {
                {"format", target.value("format", std::string{"unknown"})},
                {"format_class", target.value("format_class", std::string{"unknown"})},
            };
        }
    }
    if (composed.contains("buffers") && composed.at("buffers").is_array()) {
        for (const auto &buffer : composed.at("buffers")) {
            const auto name = buffer.is_string() ? buffer.get<std::string>()
                                                 : buffer.value("name", std::string{});
            if (!name.empty()) {
                annotations["resources"][name] = {{"format", "buffer"},
                                                   {"format_class", "buffer"}};
            }
        }
    }
    annotations["resources"]["swapchain"] = {
        {"format", vk::to_string(GET_MODULE(RenderTarget).getSwapchainFormat())},
        {"format_class", "display"},
    };

    if (composed.contains("rendering_passes") && composed.at("rendering_passes").is_array()) {
        for (const auto &graph : composed.at("rendering_passes")) {
            if (!graph.is_object() || !graph.contains("passes") || !graph.at("passes").is_array()) continue;
            std::string active_anchor;
            for (const auto &pass : graph.at("passes")) {
                if (!pass.is_object()) continue;
                const auto name = pass.value("name", std::string{});
                const auto type = pass.value("type", std::string{"render"});
                if (name.empty()) continue;
                if (type == "canonical_anchor") {
                    active_anchor = pass.value("anchor", std::string{});
                }
                const bool overlay = type == "ui" || type == "imgui";
                auto source = authored.contains(name) ? std::string{"project"} : std::string{"engine"};
                auto feature = std::string{};
                if (const auto found = feature_origins.find(name); found != feature_origins.end()) {
                    source = "feature";
                    feature = found->second;
                }
                annotations["nodes"][name] = {
                    {"type", type},
                    {"source", source},
                    {"feature", feature},
                    {"anchor", type == "canonical_anchor" ? pass.value("anchor", std::string{})
                                                            : active_anchor},
                    {"color_load_op", normalizedOp(pass, "color_load_op", overlay ? "load" : "clear")},
                    {"color_store_op", normalizedOp(pass, "color_store_op", "store")},
                    {"depth_load_op", normalizedOp(pass, "depth_load_op", "clear")},
                    {"depth_store_op", normalizedOp(pass, "depth_store_op", "dont_care")},
                };
            }
        }
    }
    if (composed.contains("compute_tasks") && composed.at("compute_tasks").is_array()) {
        for (const auto &task : composed.at("compute_tasks")) {
            if (!task.is_object()) continue;
            const auto name = task.value("name", std::string{});
            if (name.empty()) continue;
            auto source = authored.contains(name) ? std::string{"project"} : std::string{"engine"};
            auto feature = std::string{};
            if (const auto found = feature_origins.find(name); found != feature_origins.end()) {
                source = "feature";
                feature = found->second;
            }
            annotations["nodes"][name] = {{"type", "compute"}, {"source", source},
                                           {"feature", feature}, {"anchor", "compute"}};
        }
    }
    return annotations;
}

std::vector<LoweredMaterial> loadRuntimeMaterials() {
    auto &resolver = GET_MODULE(PathResolver);
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it{
             resolver.projectRoot(), std::filesystem::directory_options::skip_permission_denied, error},
         end;
         it != end; it.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!it->is_regular_file()) continue;
        const auto filename = it->path().filename().string();
        if (filename.ends_with(".material.json")) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    std::vector<LoweredMaterial> lowered;
    for (const auto &file : files) {
        const auto document_json = nlohmann::json::parse(readTextFile(file));
        MaterialSurfaceCatalog surfaces;
        if (document_json.contains("materials") && document_json.at("materials").is_array()) {
            for (const auto &entry : document_json.at("materials")) {
                if (!entry.is_object() || !entry.contains("surface") || !entry.at("surface").is_string()) continue;
                const auto ref = entry.at("surface").get<std::string>();
                if (!surfaces.contains(ref)) {
                    surfaces.emplace(ref, parseSurfaceFormat(resolver.loadText(ref), ref));
                }
            }
        }
        const auto document = parseMaterialFormatJson(document_json, surfaces);
        for (const auto &material : document.materials) {
            if (!material.surface) continue;
            lowered.push_back(lowerMaterial(material, surfaces.at(*material.surface)));
        }
    }
    return lowered;
}

ImU32 nodeColor(const PlanViewerNode &node) {
    if (node.kind == "snapshot_copy") return IM_COL32(232, 151, 52, 255);
    if (node.kind == "compute") return IM_COL32(147, 94, 214, 255);
    if (node.source == "feature") return IM_COL32(57, 160, 118, 255);
    if (node.source == "engine" || node.kind == "anchor" || node.kind == "output_transform")
        return IM_COL32(72, 103, 142, 255);
    return IM_COL32(54, 126, 190, 255);
}

ImVec2 add(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 sub(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
ImVec2 mul(ImVec2 a, float value) { return {a.x * value, a.y * value}; }

} // namespace

PlanViewerModel buildPlanViewerModel(const nlohmann::json &plan_json,
                                     const nlohmann::json &annotations,
                                     std::span<const LoweredMaterial> materials) {
    if (!plan_json.is_object() || plan_json.value("schema", std::string{}) != "pelican.frame_plan" ||
        plan_json.value("version", 0) != 1) {
        throw std::runtime_error("plan viewer requires pelican.frame_plan version 1");
    }
    if (!plan_json.contains("nodes") || !plan_json.at("nodes").is_array() ||
        !plan_json.contains("barriers") || !plan_json.at("barriers").is_array()) {
        throw std::runtime_error("plan viewer frame plan requires nodes and barriers arrays");
    }

    PlanViewerModel model;
    model.graph = plan_json.value("graph", std::string{"frame_graph"});
    std::map<std::string, PlanViewerResource> resources;
    std::unordered_set<std::string> node_names;
    for (const auto &node_json : plan_json.at("nodes")) {
        if (!node_json.is_object()) throw std::runtime_error("plan viewer node must be an object");
        PlanViewerNode node;
        node.name = node_json.at("name").get<std::string>();
        node.kind = node_json.at("kind").get<std::string>();
        node.order = node_json.at("order").get<std::size_t>();
        node.level = node_json.at("level").get<std::size_t>();
        node.reads = stringArray(node_json, "reads");
        node.writes = stringArray(node_json, "writes");
        node.snapshot_after = node_json.value("snapshot_after", std::string{});
        node.byte_size = node_json.value("byte_size", std::size_t{0});
        if (!node_names.insert(node.name).second) {
            throw std::runtime_error("plan viewer duplicate node: " + node.name);
        }
        if (const auto *annotation = objectEntry(annotations, "nodes", node.name)) {
            node.source = annotation->value("source", std::string{});
            node.feature = annotation->value("feature", std::string{});
            node.anchor = annotation->value("anchor", std::string{});
            node.color_load_op = annotation->value("color_load_op", std::string{});
            node.color_store_op = annotation->value("color_store_op", std::string{});
            node.depth_load_op = annotation->value("depth_load_op", std::string{});
            node.depth_store_op = annotation->value("depth_store_op", std::string{});
        }
        if (node.kind == "snapshot_copy") node.source = "snapshot";
        if (node.source.empty()) {
            node.source = node.kind == "anchor" || node.kind == "output_transform" ? "engine" : "project";
        }
        for (const auto &resource_name : node.reads) {
            auto &resource = resources[resource_name];
            resource.name = resource_name;
            resource.readers.push_back(node.name);
        }
        for (const auto &resource_name : node.writes) {
            auto &resource = resources[resource_name];
            resource.name = resource_name;
            resource.writers.push_back(node.name);
        }
        model.nodes.push_back(std::move(node));
    }
    std::stable_sort(model.nodes.begin(), model.nodes.end(), [](const auto &left, const auto &right) {
        return left.order < right.order;
    });

    for (const auto &edge_json : plan_json.at("barriers")) {
        PlanViewerEdge edge{edge_json.at("kind").get<std::string>(),
                            edge_json.at("resource").get<std::string>(),
                            edge_json.at("from").get<std::string>(),
                            edge_json.at("to").get<std::string>()};
        if (!node_names.contains(edge.from) || !node_names.contains(edge.to)) {
            throw std::runtime_error("plan viewer barrier references an unknown node");
        }
        model.edges.push_back(std::move(edge));
    }

    for (auto &[name, resource] : resources) {
        if (const auto *annotation = objectEntry(annotations, "resources", name)) {
            resource.format = annotation->value("format", std::string{"unknown"});
            resource.format_class = annotation->value("format_class", std::string{"unknown"});
        } else {
            resource.format = "unknown";
            resource.format_class = "unknown";
        }
        model.resources.push_back(std::move(resource));
    }

    model.materials.reserve(materials.size());
    for (const auto &material : materials) {
        model.materials.push_back(PlanViewerMaterial{
            material.name, material.surface, surfaceStem(material.surface), material.screen_inputs,
            material.target_pass, renderStateString(material.render_state)});
    }
    return model;
}

struct PlanViewer::Impl {
    PlanViewerModel model;
    std::string error;
    float zoom = 0.82f;
    ImVec2 pan{24.0f, 44.0f};
    int selected_node = -1;
    int selected_resource = -1;
    int selected_material = -1;
    bool initialized = false;

    void refresh() {
        try {
            const auto annotations = buildRuntimeAnnotations();
            const auto materials = loadRuntimeMaterials();
            model = buildPlanViewerModel(GET_MODULE(Renderer).currentFramePlanJson(), annotations, materials);
            error.clear();
            selected_node = selected_node < static_cast<int>(model.nodes.size()) ? selected_node : -1;
            selected_resource = selected_resource < static_cast<int>(model.resources.size()) ? selected_resource : -1;
            selected_material = selected_material < static_cast<int>(model.materials.size()) ? selected_material : -1;
        } catch (const std::exception &exception) {
            error = exception.what();
        }
        initialized = true;
    }

    bool materialHighlightsNode(const PlanViewerNode &node) const {
        if (selected_material < 0) return false;
        const auto &material = model.materials[static_cast<std::size_t>(selected_material)];
        if (node.name == material.target_pass) return true;
        for (const auto &input : material.screen_inputs) {
            if (contains(node.reads, input) || contains(node.writes, input)) return true;
        }
        return false;
    }

    bool resourceHighlightsNode(const PlanViewerNode &node) const {
        if (selected_resource < 0) return false;
        const auto &name = model.resources[static_cast<std::size_t>(selected_resource)].name;
        return contains(node.reads, name) || contains(node.writes, name);
    }

    bool edgeHighlighted(const PlanViewerEdge &edge) const {
        if (selected_resource >= 0 &&
            edge.resource == model.resources[static_cast<std::size_t>(selected_resource)].name) return true;
        if (selected_material >= 0) {
            const auto &material = model.materials[static_cast<std::size_t>(selected_material)];
            return contains(material.screen_inputs, edge.resource) || edge.to == material.target_pass;
        }
        return selected_resource < 0 && selected_material < 0;
    }

    void drawSources() {
        ImGui::TextUnformatted("Resources");
        ImGui::Separator();
        ImGui::BeginChild("resources", ImVec2{0.0f, 230.0f}, ImGuiChildFlags_Borders);
        for (std::size_t i = 0; i < model.resources.size(); ++i) {
            const auto &resource = model.resources[i];
            const auto selected = selected_resource == static_cast<int>(i);
            if (ImGui::Selectable((resource.name + "##resource").c_str(), selected)) {
                selected_resource = selected ? -1 : static_cast<int>(i);
                selected_material = -1;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n%s (%s)", resource.name.c_str(), resource.format.c_str(),
                                  resource.format_class.c_str());
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::TextUnformatted("Lowered materials");
        ImGui::Separator();
        ImGui::BeginChild("materials", ImVec2{0.0f, 0.0f}, ImGuiChildFlags_Borders);
        if (model.materials.empty()) ImGui::TextDisabled("No .material.json surfaces");
        for (std::size_t i = 0; i < model.materials.size(); ++i) {
            const auto &material = model.materials[i];
            const auto selected = selected_material == static_cast<int>(i);
            const auto label = material.name + "  [" + material.surface_stem + "]##material";
            if (ImGui::Selectable(label.c_str(), selected)) {
                selected_material = selected ? -1 : static_cast<int>(i);
                selected_resource = -1;
            }
        }
        ImGui::EndChild();
    }

    void drawGraph() {
        const auto canvas_pos = ImGui::GetCursorScreenPos();
        auto canvas_size = ImGui::GetContentRegionAvail();
        canvas_size.x = std::max(canvas_size.x, 64.0f);
        canvas_size.y = std::max(canvas_size.y, 64.0f);
        ImGui::InvisibleButton("plan_canvas", canvas_size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                                   ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        auto *draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(canvas_pos, add(canvas_pos, canvas_size), IM_COL32(21, 24, 29, 255));
        draw->PushClipRect(canvas_pos, add(canvas_pos, canvas_size), true);

        auto &io = ImGui::GetIO();
        if (hovered && io.MouseWheel != 0.0f) {
            const auto before = mul(sub(sub(io.MousePos, canvas_pos), pan), 1.0f / zoom);
            zoom = std::clamp(zoom * std::pow(1.13f, io.MouseWheel), 0.28f, 2.2f);
            pan = sub(sub(io.MousePos, canvas_pos), mul(before, zoom));
        }
        if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
                        ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f))) {
            pan = add(pan, io.MouseDelta);
        }

        constexpr ImVec2 node_size{184.0f, 76.0f};
        constexpr float level_gap = 228.0f;
        constexpr float row_gap = 104.0f;
        std::unordered_map<std::size_t, std::size_t> rows;
        std::vector<ImVec2> graph_positions(model.nodes.size());
        std::unordered_map<std::string, std::size_t> node_indices;
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            const auto row = rows[model.nodes[i].level]++;
            graph_positions[i] = {model.nodes[i].level * level_gap, row * row_gap};
            node_indices.emplace(model.nodes[i].name, i);
        }
        const auto toScreen = [&](ImVec2 point) {
            return add(add(canvas_pos, pan), mul(point, zoom));
        };

        for (const auto &edge : model.edges) {
            const auto from_it = node_indices.find(edge.from);
            const auto to_it = node_indices.find(edge.to);
            if (from_it == node_indices.end() || to_it == node_indices.end()) continue;
            const auto from_graph = add(graph_positions[from_it->second], {node_size.x, node_size.y * 0.5f});
            const auto to_graph = add(graph_positions[to_it->second], {0.0f, node_size.y * 0.5f});
            const auto from = toScreen(from_graph);
            const auto to = toScreen(to_graph);
            const auto control = std::max(42.0f * zoom, (to.x - from.x) * 0.42f);
            const bool highlighted = edgeHighlighted(edge);
            const auto color = highlighted ? IM_COL32(241, 194, 73, 235) : IM_COL32(92, 107, 124, 95);
            const auto thickness = highlighted ? 2.8f : 1.4f;
            draw->AddBezierCubic(from, {from.x + control, from.y}, {to.x - control, to.y}, to,
                                 color, thickness);
            const float arrow = highlighted ? 7.0f : 5.0f;
            draw->AddTriangleFilled(to, {to.x - arrow, to.y - arrow * 0.62f},
                                    {to.x - arrow, to.y + arrow * 0.62f}, color);
            if (highlighted && zoom > 0.48f) {
                const ImVec2 midpoint{(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f};
                draw->AddText(midpoint, IM_COL32(242, 222, 157, 255), edge.resource.c_str());
            }
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            selected_node = -1;
            for (std::size_t i = model.nodes.size(); i-- > 0;) {
                const auto min = toScreen(graph_positions[i]);
                const auto max = toScreen(add(graph_positions[i], node_size));
                if (io.MousePos.x >= min.x && io.MousePos.x <= max.x &&
                    io.MousePos.y >= min.y && io.MousePos.y <= max.y) {
                    selected_node = static_cast<int>(i);
                    break;
                }
            }
        }

        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            const auto &node = model.nodes[i];
            const auto min = toScreen(graph_positions[i]);
            const auto max = toScreen(add(graph_positions[i], node_size));
            const bool filtered = selected_resource >= 0 || selected_material >= 0;
            const bool highlighted = resourceHighlightsNode(node) || materialHighlightsNode(node);
            auto color = nodeColor(node);
            if (filtered && !highlighted) color = (color & IM_COL32(255, 255, 255, 0)) | IM_COL32(0, 0, 0, 62);
            draw->AddRectFilled(min, max, color, 7.0f * zoom);
            const auto border = selected_node == static_cast<int>(i) ? IM_COL32(255, 224, 119, 255)
                                                                     : IM_COL32(194, 211, 226, 210);
            draw->AddRect(min, max, border, 7.0f * zoom, 0,
                          selected_node == static_cast<int>(i) ? 3.0f : 1.0f);
            if (zoom > 0.38f) {
                const auto font_size = std::clamp(15.0f * zoom, 10.0f, 22.0f);
                draw->AddText(ImGui::GetFont(), font_size, add(min, {9.0f * zoom, 8.0f * zoom}),
                              IM_COL32_WHITE, node.name.c_str());
                const auto subtitle = node.kind + "  L" + std::to_string(node.level);
                draw->AddText(ImGui::GetFont(), std::max(9.0f, 12.0f * zoom),
                              add(min, {9.0f * zoom, 31.0f * zoom}), IM_COL32(224, 232, 239, 220),
                              subtitle.c_str());
                const auto origin = node.feature.empty() ? node.source : "feature: " + node.feature;
                draw->AddText(ImGui::GetFont(), std::max(8.0f, 11.0f * zoom),
                              add(min, {9.0f * zoom, 51.0f * zoom}), IM_COL32(219, 228, 235, 190),
                              origin.c_str());
            }
        }
        draw->AddText(add(canvas_pos, {10.0f, 8.0f}), IM_COL32(180, 190, 200, 220),
                      "Wheel: zoom   Middle/right drag: pan   Left: select");
        draw->PopClipRect();
    }

    void drawDetails() {
        ImGui::TextUnformatted("Selection");
        ImGui::Separator();
        if (selected_resource >= 0) {
            const auto &resource = model.resources[static_cast<std::size_t>(selected_resource)];
            ImGui::TextWrapped("%s", resource.name.c_str());
            ImGui::Text("format: %s", resource.format.c_str());
            ImGui::Text("class: %s", resource.format_class.c_str());
            ImGui::SeparatorText("writers");
            for (const auto &name : resource.writers) ImGui::BulletText("%s", name.c_str());
            ImGui::SeparatorText("readers");
            for (const auto &name : resource.readers) ImGui::BulletText("%s", name.c_str());
            return;
        }
        if (selected_material >= 0) {
            const auto &material = model.materials[static_cast<std::size_t>(selected_material)];
            ImGui::TextWrapped("%s", material.name.c_str());
            ImGui::Text("surface: %s", material.surface_stem.c_str());
            ImGui::Text("target pass: %s", material.target_pass.c_str());
            ImGui::SeparatorText("screen inputs");
            if (material.screen_inputs.empty()) ImGui::TextDisabled("none");
            for (const auto &input : material.screen_inputs) ImGui::BulletText("%s", input.c_str());
            ImGui::SeparatorText("render state");
            ImGui::TextWrapped("%s", material.render_state.c_str());
            return;
        }
        if (selected_node < 0) {
            ImGui::TextDisabled("Select a node, resource, or material.");
            return;
        }
        const auto &node = model.nodes[static_cast<std::size_t>(selected_node)];
        ImGui::TextWrapped("%s", node.name.c_str());
        ImGui::Text("kind: %s", node.kind.c_str());
        ImGui::Text("order / level: %zu / %zu", node.order, node.level);
        ImGui::Text("origin: %s", node.source.c_str());
        if (!node.feature.empty()) ImGui::Text("feature: %s", node.feature.c_str());
        if (!node.anchor.empty()) ImGui::Text("anchor: %s", node.anchor.c_str());
        if (node.kind == "snapshot_copy") {
            ImGui::SeparatorText("snapshot copy");
            ImGui::Text("copy point: %s", node.snapshot_after.c_str());
            ImGui::Text("byte size: %zu", node.byte_size);
        }
        ImGui::SeparatorText("attachment ops");
        if (node.color_load_op.empty() && node.depth_load_op.empty()) ImGui::TextDisabled("not applicable");
        if (!node.color_load_op.empty()) {
            ImGui::Text("color: %s / %s", node.color_load_op.c_str(), node.color_store_op.c_str());
            ImGui::Text("depth: %s / %s", node.depth_load_op.c_str(), node.depth_store_op.c_str());
        }
        ImGui::SeparatorText("reads");
        if (node.reads.empty()) ImGui::TextDisabled("none");
        for (const auto &name : node.reads) {
            const auto found = std::find_if(model.resources.begin(), model.resources.end(),
                                            [&](const auto &resource) { return resource.name == name; });
            ImGui::BulletText("%s  [%s]", name.c_str(),
                              found == model.resources.end() ? "unknown" : found->format.c_str());
        }
        ImGui::SeparatorText("writes");
        if (node.writes.empty()) ImGui::TextDisabled("none");
        for (const auto &name : node.writes) {
            const auto found = std::find_if(model.resources.begin(), model.resources.end(),
                                            [&](const auto &resource) { return resource.name == name; });
            ImGui::BulletText("%s  [%s]", name.c_str(),
                              found == model.resources.end() ? "unknown" : found->format.c_str());
        }
    }
};

PlanViewer::PlanViewer() : impl(std::make_unique<Impl>()) {}
PlanViewer::~PlanViewer() = default;

void PlanViewer::draw(bool *open) {
    if (!impl->initialized) impl->refresh();
    ImGui::SetNextWindowPos({36.0f, 62.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({1460.0f, 820.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Pelican Frame Plan", open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::Text("graph: %s", impl->model.graph.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu nodes / %zu resources / %zu materials", impl->model.nodes.size(),
                        impl->model.resources.size(), impl->model.materials.size());
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) impl->refresh();
    if (!impl->error.empty()) {
        ImGui::TextColored({1.0f, 0.42f, 0.35f, 1.0f}, "Viewer data error: %s", impl->error.c_str());
    }
    const auto available = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("plan_sources", {245.0f, available.y}, ImGuiChildFlags_Borders);
    impl->drawSources();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("plan_graph", {std::max(420.0f, available.x - 590.0f), available.y},
                      ImGuiChildFlags_Borders);
    impl->drawGraph();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("plan_details", {0.0f, available.y}, ImGuiChildFlags_Borders);
    impl->drawDetails();
    ImGui::EndChild();
    ImGui::End();
}

} // namespace Pelican
