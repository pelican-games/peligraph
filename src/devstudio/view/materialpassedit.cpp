#include "materialpassedit.hpp"

#include <algorithm>
#include <set>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

FramePlanEditRequest refuse(std::string reason) {
    FramePlanEditRequest request;
    request.refusal = std::move(reason);
    return request;
}

bool forwardContract(const std::string &contract) {
    return contract == "forward_opaque_v1" ||
           contract == "forward_transparent_v1";
}

bool knownContract(const std::string &contract) {
    const auto &choices = materialPassContracts();
    return std::ranges::any_of(choices, [&](const auto &choice) {
        return choice.contract == contract;
    });
}

MaterialPassSlot colorSlot(std::string id, std::string role, std::string name,
                           std::string format, std::string format_class) {
    return MaterialPassSlot{
        .id = std::move(id),
        .role = std::move(role),
        .default_name = std::move(name),
        .format = std::move(format),
        .format_class = std::move(format_class),
        .usage = {"COLOR_ATTACHMENT", "SAMPLED"},
    };
}

std::string join(const std::vector<std::string> &values) {
    std::string text;
    for (const auto &value : values) {
        if (!text.empty()) text += ", ";
        text += value;
    }
    return text;
}

} // namespace

const std::vector<MaterialPassContractChoice> &materialPassContracts() {
    static const std::vector<MaterialPassContractChoice> choices{
        {"forward_opaque_v1", "Forward opaque"},
        {"forward_transparent_v1", "Forward transparent"},
        {"deferred_geometry_v1", "Deferred geometry (G-buffer)"},
        {"legacy_gbuffer_v1", "Legacy G-buffer"},
    };
    return choices;
}

std::vector<MaterialPassSlot> materialPassSlots(const std::string &contract,
                                                bool hdr) {
    std::vector<MaterialPassSlot> outputs;
    if (!knownContract(contract)) return outputs;
    if (forwardContract(contract)) {
        // A forward pass writes scene-linear color that is composed before the
        // one presentation curve: always R16G16B16A16_SFLOAT, HDR or not.
        outputs.push_back(colorSlot("color", "scene color", "scene_color",
                                  "R16G16B16A16_SFLOAT",
                                  "explicit(R16G16B16A16_SFLOAT)"));
    } else {
        // The G-buffer table (materialpassattachments.hpp): the SDR and HDR
        // columns differ only where the target is a scene-class one.
        const std::string scene = hdr ? "R16G16B16A16_SFLOAT" : "B8G8R8A8_SRGB";
        outputs.push_back(colorSlot("albedo", "albedo", "gbuffer_albedo", scene, "scene"));
        outputs.push_back(colorSlot("normal", "normal", "gbuffer_normal",
                                  "R16G16B16A16_SFLOAT", "data"));
        outputs.push_back(colorSlot("material", "material", "gbuffer_material",
                                  "R8G8B8A8_UNORM", "data"));
        outputs.push_back(colorSlot("world_position", "world position",
                                  "gbuffer_worldpos", "R16G16B16A16_SFLOAT",
                                  "data"));
        outputs.push_back(colorSlot("emissive", "emissive", "gbuffer_emissive",
                                  scene, "scene"));
    }
    outputs.push_back(MaterialPassSlot{
        .id = "depth",
        .role = "depth",
        .default_name = "scene_depth",
        .format = "D32_SFLOAT",
        .format_class = "data",
        .usage = {"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"},
        .depth = true,
    });
    return outputs;
}

bool materialPassHdr(const FramePlanAuthoring &authoring) {
    if (!authoring.enabled_features_known) return true;
    return std::ranges::find(authoring.enabled_features, "hdr") !=
           authoring.enabled_features.end();
}

std::string materialPassDefaultName(const FramePlanAuthoring &authoring,
                                    const std::string &graph,
                                    const std::string &contract) {
    std::string stem = contract;
    if (stem.ends_with("_v1")) stem.resize(stem.size() - 3);
    if (stem.empty()) stem = "material_pass";
    const auto order = authoring.order.find(graph);
    const auto taken = [&](const std::string &name) {
        return order != authoring.order.end() &&
               std::ranges::find(order->second, name) != order->second.end();
    };
    if (!taken(stem)) return stem;
    for (int suffix = 2;; ++suffix) {
        const auto candidate = stem + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
}

std::vector<std::string> materialPassFillMissing(
    const std::vector<std::string> &declared, const std::string &contract,
    std::vector<std::string> targets) {
    const auto outputs = materialPassSlots(contract, true);
    targets.resize(outputs.size());
    std::set<std::string, std::less<>> used{declared.begin(), declared.end()};
    for (const auto &target : targets)
        if (!target.empty()) used.insert(target);
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        if (!targets[index].empty()) continue;
        auto name = outputs[index].default_name;
        for (int suffix = 2; used.contains(name); ++suffix)
            name = outputs[index].default_name + "_" + std::to_string(suffix);
        used.insert(name);
        targets[index] = std::move(name);
    }
    return targets;
}

std::vector<std::string> materialPassInitialTargets(
    const FramePlanAuthoring &authoring, const std::string &contract) {
    const auto slots = materialPassSlots(contract, materialPassHdr(authoring));
    std::vector<std::string> declared;
    for (const auto &[name, row] : authoring.render_targets) {
        (void)row;
        declared.push_back(name);
    }
    std::vector<std::string> targets(slots.size());
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const auto *row = authoring.findRenderTarget(slots[index].default_name);
        if (row == nullptr || !row->declaration.is_object()) continue;
        const auto format = row->declaration.value("format", std::string{});
        const auto format_class = row->declaration.value("format_class", std::string{});
        const bool fits = !format.empty() ? format == slots[index].format
                                          : !format_class.empty() &&
                                                format_class == slots[index].format_class;
        if (fits) targets[index] = slots[index].default_name;
    }
    return materialPassFillMissing(declared, contract, std::move(targets));
}

std::vector<std::size_t> materialPassNewSlots(
    const FramePlanAuthoring &authoring, const MaterialPassFormValues &values) {
    std::vector<std::size_t> outputs;
    std::set<std::string, std::less<>> seen;
    for (std::size_t index = 0; index < values.targets.size(); ++index) {
        const auto &name = values.targets[index];
        if (name.empty() || authoring.findRenderTarget(name) != nullptr) continue;
        // One name in two outputs is made once; the pass then names it twice,
        // and the engine says what it thinks of that.
        if (!seen.insert(name).second) continue;
        outputs.push_back(index);
    }
    return outputs;
}

std::string materialPassFormRefusal(const FramePlanAuthoring &authoring,
                                    const MaterialPassFormValues &values) {
    if (!authoring.received)
        return "The render authoring context has not arrived yet.";
    if (!knownContract(values.contract))
        return "Choose a material contract for the pass.";
    if (values.name.empty()) return "A material pass needs a name.";
    if (authoring.find(values.graph, values.name) != nullptr)
        return values.graph + " already has a pass named " + values.name + ".";
    const auto anchors = authoring.anchors.find(values.graph);
    if (anchors == authoring.anchors.end() ||
        std::ranges::none_of(anchors->second, [&](const auto &anchor) {
            return anchor.second == values.insert;
        }))
        return "There is no place in " + values.graph +
               " to insert the pass; refresh the Frame Plan.";
    const auto outputs = materialPassSlots(values.contract, true);
    if (values.targets.size() != outputs.size())
        return values.name + ": the outputs do not match the contract.";
    std::vector<std::string> empty;
    for (std::size_t index = 0; index < outputs.size(); ++index)
        if (values.targets[index].empty()) empty.push_back(outputs[index].role);
    if (!empty.empty())
        return values.name + ": choose a target for " + join(empty) +
               ", or press Create missing targets.";
    return {};
}

FramePlanEditRequest materialPassTargetsRequest(
    const FramePlanAuthoring &authoring, const MaterialPassFormValues &values) {
    if (auto reason = materialPassFormRefusal(authoring, values); !reason.empty())
        return refuse(std::move(reason));
    const auto outputs = materialPassSlots(values.contract, materialPassHdr(authoring));
    auto ops = Json::array();
    for (const auto index : materialPassNewSlots(authoring, values)) {
        const auto &slot = outputs[index];
        ops.push_back(Json{{"op", "add"},
                           {"target",
                            {{"name", values.targets[index]},
                             {"extent_scale", 1.0},
                             {"format", slot.format},
                             {"format_class", slot.format_class},
                             {"usage", slot.usage}}}});
    }
    if (ops.empty()) return {};
    FramePlanEditRequest request;
    request.method = "edit_render_targets";
    request.params = Json{{"base_source_digest", authoring.source_digest},
                          {"ops", std::move(ops)}};
    return request;
}

FramePlanEditRequest materialPassAddRequest(const FramePlanAuthoring &authoring,
                                            const MaterialPassFormValues &values) {
    if (auto reason = materialPassFormRefusal(authoring, values); !reason.empty())
        return refuse(std::move(reason));
    std::vector<std::string> missing;
    for (const auto index : materialPassNewSlots(authoring, values))
        missing.push_back(values.targets[index]);
    if (!missing.empty())
        return refuse(values.name + " names render targets that are not declared yet: " +
                      join(missing) + ".");
    auto colors = Json::array();
    for (std::size_t index = 0; index + 1 < values.targets.size(); ++index)
        colors.push_back(values.targets[index]);
    FramePlanEditRequest request;
    request.method = "add_authored_pass";
    request.params = Json{
        {"base_source_digest", authoring.source_digest},
        {"graph", values.graph},
        {"insert", values.insert},
        {"pass",
         {{"name", values.name},
          {"type", "material"},
          {"material_contract", values.contract},
          {"output",
           {{"color", std::move(colors)}, {"depth", values.targets.back()}}}}}};
    // G-4: one ticket carries the pass and the route default together, so
    // the engine judges the pass with the routes the new default gives.
    if (values.forward_default &&
        materialPassOffersForwardDefault(authoring, values.contract))
        request.params["material_route_default"] = "forward";
    return request;
}

bool materialPassPipelineHasDeferredPass(const FramePlanAuthoring &authoring) {
    return std::ranges::any_of(authoring.passes, [](const auto &entry) {
        const auto &declaration = entry.second.declaration;
        if (!declaration.is_object() ||
            declaration.value("type", std::string{}) != "material")
            return false;
        const auto contract =
            declaration.value("material_contract", std::string{"legacy_gbuffer_v1"});
        return contract == "deferred_geometry_v1" || contract == "legacy_gbuffer_v1";
    });
}

bool materialPassOffersForwardDefault(const FramePlanAuthoring &authoring,
                                      const std::string &contract) {
    return contract == "forward_opaque_v1" &&
           !materialPassPipelineHasDeferredPass(authoring);
}

std::string materialRouteUnservedNotice(const Json &error) {
    if (!error.is_object() ||
        error.value("code", std::string{}) != "material_route_unserved@1")
        return {};
    std::string notice;
    for (const auto &route : error.value("unserved", Json::array())) {
        std::vector<std::string> names;
        for (const auto &material : route.value("materials", Json::array())) {
            auto name = material.value("name", std::string{});
            if (material.value("origin", std::string{}) == "builtin")
                name += " (engine built-in)";
            names.push_back(std::move(name));
        }
        if (!notice.empty()) notice += " ";
        notice += "No pass draws the " + route.value("route", std::string{}) +
                  " route, which " + join(names) +
                  (names.size() == 1 ? " takes" : " take") + ". To fix: " +
                  route.value("hint", std::string{}) + ".";
    }
    return notice.empty() ? error.value("message", std::string{}) : notice;
}

} // namespace PelicanStudio
