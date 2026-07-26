#include "compiledplanviewer.hpp"

#include "../container.hpp"
#include "../renderingpass/framegraphruntime.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <string_view>

namespace Pelican {

namespace {

const char *nodeKindName(FramePlanNodeKind kind) {
    switch (kind) {
    case FramePlanNodeKind::render: return "render";
    case FramePlanNodeKind::compute: return "compute";
    case FramePlanNodeKind::anchor: return "anchor";
    case FramePlanNodeKind::snapshot_copy: return "snapshot_copy";
    case FramePlanNodeKind::output_transform: return "output_transform";
    }
    return "unknown";
}

const char *routeClassName(MaterialRouteClass route) {
    switch (route) {
    case MaterialRouteClass::deferred_geometry: return "deferred_geometry";
    case MaterialRouteClass::forward_opaque: return "forward_opaque";
    case MaterialRouteClass::forward_transparent: return "forward_transparent";
    }
    return "unknown";
}

const char *contractName(MaterialPassContract contract) {
    switch (contract) {
    case MaterialPassContract::legacy_gbuffer_v1: return "legacy_gbuffer_v1";
    case MaterialPassContract::deferred_geometry_v1: return "deferred_geometry_v1";
    case MaterialPassContract::forward_opaque_v1: return "forward_opaque_v1";
    case MaterialPassContract::forward_transparent_v1: return "forward_transparent_v1";
    }
    return "unknown";
}

// Scalars are shown verbatim; containers are summarised so the fact list stays
// readable. The full document is always available in the raw JSON tree.
std::string scalarText(const nlohmann::json &value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
    if (value.is_number_unsigned()) return std::to_string(value.get<std::uint64_t>());
    if (value.is_number_float()) return std::to_string(value.get<double>());
    if (value.is_null()) return "null";
    if (value.is_array()) return "[" + std::to_string(value.size()) + " entries]";
    if (value.is_object()) return "{" + std::to_string(value.size()) + " fields}";
    return "?";
}

bool containsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

void drawJsonTree(const nlohmann::json &value, const std::string &label,
                  std::string_view filter) {
    if (value.is_object() || value.is_array()) {
        const auto summary = label + "  " + scalarText(value);
        if (!ImGui::TreeNode(label.c_str(), "%s", summary.c_str())) return;
        if (value.is_object()) {
            for (const auto &entry : value.items()) {
                drawJsonTree(entry.value(), entry.key(), filter);
            }
        } else {
            std::size_t index = 0;
            for (const auto &entry : value) {
                drawJsonTree(entry, "[" + std::to_string(index++) + "]", filter);
            }
        }
        ImGui::TreePop();
        return;
    }
    const auto text = scalarText(value);
    if (!filter.empty() && !containsCaseInsensitive(label, filter) &&
        !containsCaseInsensitive(text, filter)) {
        return;
    }
    ImGui::Bullet();
    ImGui::TextDisabled("%s", label.c_str());
    ImGui::SameLine();
    ImGui::TextUnformatted(text.c_str());
}

} // namespace

std::vector<CompiledPlanFact> buildCompiledPlanFacts(
    const nlohmann::json &plan_json) {
    std::vector<CompiledPlanFact> facts;
    if (!plan_json.is_object()) return facts;

    // Promoted keys are the ones a person checks first when asking "what did
    // the compiler decide?". Everything else remains in the raw tree.
    static constexpr std::string_view promoted[] = {
        "schema",
        "version",
        "graph",
        "logical_graph_fingerprint",
        "automatic_plan_fingerprint",
        "view_count",
        "uses_multiview",
        "endpoint_supports_multiview",
        "max_multiview_view_count",
        "mixed_execution",
        "auto_gate",
        "reason",
        "requested",
    };
    for (const auto key : promoted) {
        const auto it = plan_json.find(key);
        if (it == plan_json.end()) continue;
        facts.push_back({std::string{key}, scalarText(*it)});
    }

    // Counted collections: the count is the useful signal at a glance.
    static constexpr std::string_view counted[] = {
        "view_execution_plan",       "backend_selection",
        "required_physical_features", "graph_transforms",
        "subgraph_replacements",      "planning_opportunities",
    };
    for (const auto key : counted) {
        const auto it = plan_json.find(key);
        if (it == plan_json.end()) continue;
        if (it->is_array()) {
            facts.push_back({std::string{key},
                             std::to_string(it->size()) + " entries"});
        } else {
            facts.push_back({std::string{key}, scalarText(*it)});
        }
    }

    for (const auto key : {"ejectable_pin_package", "ejectable_physical_fragment"}) {
        const auto it = plan_json.find(key);
        if (it == plan_json.end()) continue;
        facts.push_back({std::string{key}, it->is_null() ? "none" : "available"});
    }
    return facts;
}

std::vector<std::string> buildCompiledPlanOpportunities(
    const nlohmann::json &plan_json) {
    std::vector<std::string> rows;
    if (!plan_json.is_object()) return rows;
    const auto it = plan_json.find("planning_opportunities");
    if (it == plan_json.end() || !it->is_array()) return rows;
    rows.reserve(it->size());
    for (const auto &entry : *it) {
        if (entry.is_string()) {
            rows.push_back(entry.get<std::string>());
            continue;
        }
        if (entry.is_object()) {
            std::string text;
            for (const auto &field : entry.items()) {
                if (!text.empty()) text += "  ";
                text += field.key() + "=" + scalarText(field.value());
            }
            rows.push_back(text);
            continue;
        }
        rows.push_back(scalarText(entry));
    }
    return rows;
}

CompiledPlanModel buildCompiledPlanModel() {
    CompiledPlanModel model;
    try {
        const auto generation = GET_MODULE(FrameGraphRuntimeContainer).snapshot();
        if (!generation) {
            model.error = "no published runtime generation";
            return model;
        }
        model.generation = generation->generation;
        model.enabled_features = generation->enabled_feature_names;

        // Reverse the published name table so each program can show the graph
        // variant name a person recognises (flat / xr / preview / ...).
        std::vector<std::pair<RenderingPassId, std::string>> names;
        names.reserve(generation->name_to_id.size());
        for (const auto &[name, id] : generation->name_to_id) {
            names.emplace_back(id, name);
        }

        for (const auto &id : generation->rendering_pass_ids) {
            const auto *program = generation->find(id);
            if (program == nullptr) continue;

            CompiledPlanProgram row;
            row.rendering_pass_id = std::to_string(id.value);
            row.owner_scope = program->owner_scope;
            for (const auto &[named_id, name] : names) {
                if (named_id == id) {
                    row.variant = name;
                    break;
                }
            }
            if (row.variant.empty()) row.variant = "pass#" + row.rendering_pass_id;

            const auto &execution = program->frame_graph;
            row.nodes.reserve(execution.nodes.size());
            for (const auto &node : execution.nodes) {
                CompiledPlanNodeRow node_row;
                node_row.name = node.name;
                node_row.kind = nodeKindName(node.kind);
                node_row.index = node.index;
                node_row.incoming_barriers.reserve(node.incoming_barriers.size());
                for (const auto &barrier : node.incoming_barriers) {
                    CompiledPlanBarrierRow barrier_row;
                    barrier_row.resource = barrier.resource;
                    barrier_row.from_kind = nodeKindName(barrier.from_kind);
                    barrier_row.to_kind = nodeKindName(barrier.to_kind);
                    barrier_row.from_node =
                        barrier.from_node_index < execution.nodes.size()
                            ? execution.nodes[barrier.from_node_index].name
                            : "#" + std::to_string(barrier.from_node_index);
                    node_row.incoming_barriers.push_back(std::move(barrier_row));
                }
                row.nodes.push_back(std::move(node_row));
            }

            for (const auto &[name, target] : execution.render_target_bindings) {
                row.bindings.push_back(
                    {name, std::to_string(target.value), "render_target"});
            }
            for (const auto &[name, buffer] : execution.buffer_bindings) {
                row.bindings.push_back(
                    {name, std::to_string(buffer.value), "buffer"});
            }
            std::sort(row.bindings.begin(), row.bindings.end(),
                      [](const CompiledPlanBindingRow &a,
                         const CompiledPlanBindingRow &b) {
                          return a.logical_name < b.logical_name;
                      });

            for (const auto &route : execution.material_routes) {
                row.routes.push_back({routeClassName(route.route),
                                      contractName(route.pass_contract),
                                      std::to_string(route.pass_id.value)});
            }

            if (execution.target_plan) {
                row.has_target_plan = true;
                try {
                    row.target_plan_json =
                        vulkanTargetPlanToJson(*execution.target_plan);
                } catch (const std::exception &error) {
                    row.target_plan_json = nlohmann::ordered_json{
                        {"serialization_error", error.what()}};
                }
                row.facts = buildCompiledPlanFacts(row.target_plan_json);
                row.planning_opportunities =
                    buildCompiledPlanOpportunities(row.target_plan_json);
            }
            model.programs.push_back(std::move(row));
        }
    } catch (const std::exception &error) {
        model.error = error.what();
    }
    return model;
}

struct CompiledPlanViewer::Impl {
    CompiledPlanModel model;
    bool initialized = false;
    int selected_program = 0;
    int selected_node = -1;
    char filter[128] = {};

    void refresh() {
        model = buildCompiledPlanModel();
        initialized = true;
        if (selected_program >= static_cast<int>(model.programs.size())) {
            selected_program = 0;
        }
        selected_node = -1;
    }

    const CompiledPlanProgram *program() const {
        if (selected_program < 0 ||
            selected_program >= static_cast<int>(model.programs.size())) {
            return nullptr;
        }
        return &model.programs[static_cast<std::size_t>(selected_program)];
    }

    void drawExecutionList(const CompiledPlanProgram &program) {
        ImGui::TextDisabled("execution order (%zu nodes)", program.nodes.size());
        ImGui::Separator();
        for (int index = 0; index < static_cast<int>(program.nodes.size()); ++index) {
            const auto &node = program.nodes[static_cast<std::size_t>(index)];
            const std::string label =
                std::to_string(node.index) + "  " + node.name;
            if (ImGui::Selectable(label.c_str(), selected_node == index)) {
                selected_node = index;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", node.kind.c_str());
            if (!node.incoming_barriers.empty()) {
                ImGui::SameLine();
                ImGui::TextColored({0.98f, 0.72f, 0.31f, 1.0f}, "%zu barriers",
                                   node.incoming_barriers.size());
            }
        }
    }

    void drawNodeDetail(const CompiledPlanProgram &program) {
        if (selected_node < 0 ||
            selected_node >= static_cast<int>(program.nodes.size())) {
            ImGui::TextDisabled("Select a compiled node to inspect barriers.");
            return;
        }
        const auto &node = program.nodes[static_cast<std::size_t>(selected_node)];
        ImGui::Text("%s", node.name.c_str());
        ImGui::TextDisabled("kind %s / execution index %zu", node.kind.c_str(),
                            node.index);
        ImGui::Separator();
        if (node.incoming_barriers.empty()) {
            ImGui::TextDisabled("no incoming barriers");
        } else if (ImGui::BeginTable("barriers", 4,
                                     ImGuiTableFlags_Borders |
                                         ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("resource");
            ImGui::TableSetupColumn("from node");
            ImGui::TableSetupColumn("from");
            ImGui::TableSetupColumn("to");
            ImGui::TableHeadersRow();
            for (const auto &barrier : node.incoming_barriers) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(barrier.resource.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(barrier.from_node.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(barrier.from_kind.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(barrier.to_kind.c_str());
            }
            ImGui::EndTable();
        }

        ImGui::Dummy({0.0f, 6.0f});
        if (ImGui::CollapsingHeader("resource bindings",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            if (program.bindings.empty()) {
                ImGui::TextDisabled("none");
            } else if (ImGui::BeginTable("bindings", 3,
                                         ImGuiTableFlags_Borders |
                                             ImGuiTableFlags_RowBg |
                                             ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("logical name");
                ImGui::TableSetupColumn("kind");
                ImGui::TableSetupColumn("id");
                ImGui::TableHeadersRow();
                for (const auto &binding : program.bindings) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(binding.logical_name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(binding.kind.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(binding.binding.c_str());
                }
                ImGui::EndTable();
            }
        }
        if (!program.routes.empty() && ImGui::CollapsingHeader("material routes")) {
            for (const auto &route : program.routes) {
                ImGui::BulletText("%s -> %s (pass %s)", route.route.c_str(),
                                  route.pass_contract.c_str(),
                                  route.pass_id.c_str());
            }
        }
    }

    void drawPlanFacts(const CompiledPlanProgram &program) {
        if (!program.has_target_plan) {
            ImGui::TextDisabled(
                "This variant was published without a Vulkan target plan.");
            return;
        }
        if (ImGui::BeginTable("facts", 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("key");
            ImGui::TableSetupColumn("value");
            ImGui::TableHeadersRow();
            for (const auto &fact : program.facts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(fact.key.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(fact.value.c_str());
            }
            ImGui::EndTable();
        }
        if (!program.planning_opportunities.empty() &&
            ImGui::CollapsingHeader("planning opportunities",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto &row : program.planning_opportunities) {
                ImGui::BulletText("%s", row.c_str());
            }
        }
        ImGui::Dummy({0.0f, 6.0f});
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##plan_filter", "filter raw plan", filter,
                                 sizeof(filter));
        if (ImGui::CollapsingHeader("raw pelican.vulkan_target_plan",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            drawJsonTree(program.target_plan_json, "target_plan", filter);
        }
    }
};

CompiledPlanViewer::CompiledPlanViewer() : impl{std::make_unique<Impl>()} {}
CompiledPlanViewer::~CompiledPlanViewer() = default;

void CompiledPlanViewer::draw(bool *open) {
    if (!impl->initialized) impl->refresh();
    ImGui::SetNextWindowPos({72.0f, 96.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({1320.0f, 760.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Pelican Compiled Passes", open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::Text("runtime generation %llu",
                static_cast<unsigned long long>(impl->model.generation));
    ImGui::SameLine();
    ImGui::TextDisabled("%zu published programs", impl->model.programs.size());
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) impl->refresh();
    if (!impl->model.error.empty()) {
        ImGui::TextColored({1.0f, 0.42f, 0.35f, 1.0f}, "%s",
                           impl->model.error.c_str());
    }
    if (!impl->model.enabled_features.empty()) {
        std::string features;
        for (const auto &name : impl->model.enabled_features) {
            if (!features.empty()) features += ", ";
            features += name;
        }
        ImGui::TextDisabled("features: %s", features.c_str());
    }

    if (impl->model.programs.empty()) {
        ImGui::TextDisabled(
            "No compiled render program is published yet. Start the renderer "
            "and press Refresh.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("variants")) {
        for (int index = 0;
             index < static_cast<int>(impl->model.programs.size()); ++index) {
            const auto &program =
                impl->model.programs[static_cast<std::size_t>(index)];
            if (ImGui::BeginTabItem(program.variant.c_str())) {
                if (impl->selected_program != index) {
                    impl->selected_program = index;
                    impl->selected_node = -1;
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    const auto *program = impl->program();
    if (program == nullptr) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("owner scope: %s", program->owner_scope.empty()
                                               ? "(none)"
                                               : program->owner_scope.c_str());

    const auto available = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("compiled_nodes", {320.0f, available.y},
                      ImGuiChildFlags_Borders);
    impl->drawExecutionList(*program);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("compiled_detail",
                      {std::max(360.0f, available.x - 800.0f), available.y},
                      ImGuiChildFlags_Borders);
    impl->drawNodeDetail(*program);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("compiled_plan", {0.0f, available.y},
                      ImGuiChildFlags_Borders);
    impl->drawPlanFacts(*program);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace Pelican
