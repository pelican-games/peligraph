#include "featurerefusalfix.hpp"

#include <algorithm>
#include <set>

namespace PelicanStudio {

using Json = nlohmann::json;

namespace {

std::string stringOf(const Json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

const Json &fieldsOf(const Json &error) {
    // The engine merges a refusal's fields into the error object; a few
    // answers carry them under `detail` instead.
    if (error.is_object()) {
        const auto detail = error.find("detail");
        if (detail != error.end() && detail->is_object() && !error.contains("feature") &&
            !error.contains("node") && !error.contains("snapshot"))
            return *detail;
    }
    return error;
}

std::string kindWord(const Json &type) {
    if (requiredTargetIsDepth(type)) return "depth";
    return stringOf(type, "semantic") == "pelican.render.color_signal@1" ||
                   stringOf(type, "semantic") == "pelican.render.legacy_opaque_image@1"
               ? "color"
               : "image";
}

std::string joinNames(const std::vector<std::string> &names) {
    std::string joined;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index > 0) joined += index + 1 == names.size() ? " and " : ", ";
        joined += names[index];
    }
    return joined;
}

bool hasNode(const Json &nodes, const std::string &id) {
    if (!nodes.is_array()) return false;
    return std::ranges::any_of(nodes, [&](const Json &node) { return stringOf(node, "id") == id; });
}

} // namespace

std::vector<FeatureRequiredTarget> featureRequiredTargets(const Json &catalog_entry) {
    std::vector<FeatureRequiredTarget> targets;
    if (!catalog_entry.is_object()) return targets;
    const auto found = catalog_entry.find("required_targets");
    if (found == catalog_entry.end() || !found->is_array()) return targets;
    for (const auto &entry : *found) {
        const auto name = stringOf(entry, "name");
        if (name.empty() || !entry.contains("type") || !entry.at("type").is_object()) continue;
        FeatureRequiredTarget target{name, entry.at("type"), {}};
        if (const auto usage = entry.find("usage"); usage != entry.end() && usage->is_array())
            for (const auto &flag : *usage)
                if (flag.is_string()) target.usage.push_back(flag.get<std::string>());
        targets.push_back(std::move(target));
    }
    return targets;
}

bool requiredTargetIsDepth(const Json &type) {
    return stringOf(type, "semantic") == "pelican.render.depth@1";
}

std::vector<FeatureRequiredTarget> missingRequiredTargets(
    const std::vector<FeatureRequiredTarget> &required, const std::vector<std::string> &declared,
    const Json &graph_nodes) {
    std::set<std::string> referenced;
    if (graph_nodes.is_array())
        for (const auto &node : graph_nodes)
            if (stringOf(node, "op") == "render.target_ref")
                referenced.insert(stringOf(node.value("parameters", Json::object()), "name"));
    std::vector<FeatureRequiredTarget> missing;
    for (auto target : required) {
        // A declared target composes whether the Graph reads it or not; one
        // that is not is declared, and given a render.target_ref of its type
        // unless the Graph has one.
        target.declare = std::ranges::find(declared, target.name) == declared.end();
        target.reference = !referenced.contains(target.name);
        if (target.declare) missing.push_back(std::move(target));
    }
    return missing;
}

std::string requiredTargetsQuestion(const std::string &feature,
                                    const std::vector<FeatureRequiredTarget> &missing) {
    std::vector<std::string> named;
    for (const auto &target : missing) named.push_back(target.name + " (" + kindWord(target.type) + ")");
    return feature + " needs " + joinNames(named) + ". Add " + (missing.size() == 1 ? "it" : "them") +
           " with the feature?";
}

Json requiredTargetAddOps(const std::vector<FeatureRequiredTarget> &missing) {
    Json ops = Json::array();
    for (const auto &target : missing) {
        if (!target.declare) continue;
        const bool depth = requiredTargetIsDepth(target.type);
        // As the material pass form declares its outputs (G-1): drawn into,
        // then read by what the feature reads it for.
        std::vector<std::string> usage{depth ? "DEPTH_STENCIL_ATTACHMENT" : "COLOR_ATTACHMENT"};
        if (!depth) usage.emplace_back("SAMPLED");
        for (const auto &flag : target.usage)
            if (std::ranges::find(usage, flag) == usage.end()) usage.push_back(flag);
        ops.push_back(Json{{"op", "add"},
                           {"target",
                            {{"name", target.name},
                             {"extent_scale", 1.0},
                             {"format", depth ? "D32_SFLOAT" : "R16G16B16A16_SFLOAT"},
                             {"format_class", depth ? "data" : "scene"},
                             {"usage", usage}}}});
    }
    return ops;
}

Json requiredTargetRemoveOps(const std::vector<FeatureRequiredTarget> &missing) {
    Json ops = Json::array();
    for (const auto &target : missing)
        if (target.declare) ops.push_back(Json{{"op", "remove"}, {"name", target.name}});
    return ops;
}

Json requiredTargetReferenceOps(const std::vector<FeatureRequiredTarget> &missing,
                                const Json &graph_nodes) {
    Json ops = Json::array();
    Json taken = graph_nodes.is_array() ? graph_nodes : Json::array();
    for (const auto &target : missing) {
        if (!target.reference) continue;
        std::string id = target.name + "_ref";
        for (int suffix = 2; hasNode(taken, id) && suffix < 10000; ++suffix)
            id = target.name + "_ref_" + std::to_string(suffix);
        Json node{{"id", id},
                  {"op", "render.target_ref"},
                  {"parameters", {{"name", target.name}, {"type", target.type}}},
                  {"outputs", Json::array({"resource"})}};
        taken.push_back(node);
        ops.push_back(Json{{"op", "add_node"}, {"scope", "root"}, {"node", std::move(node)}});
    }
    return ops;
}

Json removeAddedNodeOps(const Json &add_ops) {
    Json ops = Json::array();
    if (!add_ops.is_array()) return ops;
    for (auto entry = add_ops.rbegin(); entry != add_ops.rend(); ++entry)
        if (stringOf(*entry, "op") == "add_node" && entry->contains("node"))
            ops.push_back(Json{{"op", "remove_node"},
                               {"scope", entry->value("scope", std::string{"root"})},
                               {"node", stringOf(entry->at("node"), "id")}});
    return ops;
}

std::optional<FeatureRequiredTarget> requiredTargetFromRefusal(const Json &error) {
    if (stringOf(error, "code") != "feature.required_target_missing@1") return std::nullopt;
    const auto &fields = fieldsOf(error);
    const auto name = stringOf(fields, "target");
    if (name.empty() || !fields.contains("type") || !fields.at("type").is_object()) return std::nullopt;
    FeatureRequiredTarget target{name, fields.at("type"), {}};
    target.declare = true;
    target.reference = true;
    return target;
}

std::optional<FeatureInsertionAnchorMissing> insertionAnchorMissing(const Json &error) {
    if (!error.is_object()) return std::nullopt;
    if (stringOf(error, "code") == "feature.insertion_anchor_missing@1") {
        const auto &fields = fieldsOf(error);
        FeatureInsertionAnchorMissing missing{stringOf(fields, "feature"), stringOf(fields, "anchor"), {}};
        if (const auto candidates = fields.find("candidates");
            candidates != fields.end() && candidates->is_array())
            for (const auto &candidate : *candidates)
                if (candidate.is_string()) missing.candidates.push_back(candidate.get<std::string>());
        if (missing.anchor.empty()) return std::nullopt;
        return missing;
    }
    // An engine without FR-2 says it in a sentence only.
    static constexpr std::string_view sentence = "insert anchor was not found: ";
    const auto message = stringOf(error, "message");
    const auto at = message.find(sentence);
    if (at == std::string::npos) return std::nullopt;
    auto anchor = message.substr(at + sentence.size());
    const auto end = anchor.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
    if (end != std::string::npos) anchor.resize(end);
    if (anchor.empty()) return std::nullopt;
    return FeatureInsertionAnchorMissing{stringOf(error, "feature"), anchor, {}};
}

std::vector<std::string> missingInsertionAnchors(const Json &catalog_entry,
                                                 const std::vector<std::string> &passes) {
    std::vector<std::string> missing;
    if (!catalog_entry.is_object()) return missing;
    const auto insertion = catalog_entry.find("insertion");
    if (insertion == catalog_entry.end() || !insertion->is_array()) return missing;
    // The engine's canonical anchors (featurecompose.cpp) are in every
    // composed pipeline, whatever its passes are called.
    static const std::set<std::string> canonical{"sprite",     "post_main",  "tonemap",
                                                 "post_ldr",   "pelican_ui", "debug_draw",
                                                 "debug_text", "imgui"};
    for (const auto &entry : *insertion) {
        const auto anchor = stringOf(entry, "anchor");
        if (anchor.empty() || canonical.contains(anchor) ||
            std::ranges::find(passes, anchor) != passes.end() ||
            std::ranges::find(missing, anchor) != missing.end())
            continue;
        missing.push_back(anchor);
    }
    return missing;
}

std::string insertionAnchorLine(const std::string &feature, const std::string &anchor,
                                const std::string &label) {
    return (feature.empty() ? std::string{"The feature"} : feature) + " goes after " + anchor +
           ", which this pipeline does not have. Place " + (label.empty() ? anchor : label) +
           " first.";
}

std::optional<TargetRefTypeFix> targetRefTypeFix(const Json &error) {
    if (stringOf(error, "code") != "graph.target_ref_type_mismatch@1") return std::nullopt;
    const auto &fields = fieldsOf(error);
    const auto expected = fields.find("expected");
    if (expected == fields.end() || !expected->is_object() || !expected->contains("type") ||
        !expected->at("type").is_object())
        return std::nullopt;
    TargetRefTypeFix fix{stringOf(fields, "node"), stringOf(fields, "name"), expected->at("type"), {}};
    if (fix.node.empty()) return std::nullopt;
    if (const auto layers = expected->find("layers");
        layers != expected->end() && layers->is_number_integer())
        fix.layers = layers->get<std::int64_t>();
    return fix;
}

std::string targetRefTypeFixLabel(const TargetRefTypeFix &fix) {
    const auto arguments = fix.type.value("arguments", Json::object());
    const auto topology = stringOf(arguments, "topology");
    const std::string shape = topology == "two_d_array" ? "2D array "
                              : topology == "cube"      ? "cube "
                              : topology.empty()        ? "2D "
                                                        : topology + " ";
    std::string label = "Fix: make " + fix.node + " " + shape + kindWord(fix.type);
    if (fix.layers) label += " (" + std::to_string(*fix.layers) + " layers)";
    return label;
}

Json targetRefTypeFixOps(const TargetRefTypeFix &fix, const Json &instances) {
    const auto replace = [&](const Json &nodes, const std::string &scope) -> Json {
        if (!nodes.is_array()) return nullptr;
        for (const auto &node : nodes) {
            if (stringOf(node, "id") != fix.node || stringOf(node, "op") != "render.target_ref") continue;
            Json edited = node;
            edited["parameters"]["type"] = fix.type;
            return Json::array({Json{{"op", "replace_node"}, {"scope", scope}, {"node", std::move(edited)}}});
        }
        return nullptr;
    };
    if (!instances.is_object()) return Json::array();
    if (auto ops = replace(instances.value("nodes", Json::array()), "root"); !ops.is_null()) return ops;
    if (const auto definitions = instances.find("definitions");
        definitions != instances.end() && definitions->is_object())
        for (const auto &[name, body] : definitions->items())
            if (body.is_object())
                if (auto ops = replace(body.value("nodes", Json::array()), name); !ops.is_null())
                    return ops;
    return Json::array();
}

std::optional<std::string> snapshotAfterUnknownLine(const Json &error) {
    if (stringOf(error, "code") != "snapshot.after_unknown@1") return std::nullopt;
    const auto &fields = fieldsOf(error);
    std::vector<std::string> candidates;
    if (const auto found = fields.find("candidates"); found != fields.end() && found->is_array())
        for (const auto &candidate : *found)
            if (candidate.is_string()) candidates.push_back(candidate.get<std::string>());
    std::string line = "Snapshot " + stringOf(fields, "snapshot") + " cannot be taken after " +
                       stringOf(fields, "after") + ": ";
    if (candidates.empty()) return line + "this pipeline has no pass to take it after.";
    std::string listed;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (index > 0) listed += index + 1 == candidates.size() ? " or " : ", ";
        listed += candidates[index];
    }
    return line + "this pipeline takes it after " + listed + ".";
}

std::optional<std::string> materialRouteUnservedLine(const Json &error) {
    // The code, or (from a ticket refused at compile) the composer's sentence
    // that names it.
    if (stringOf(error, "code").find("material_route_unserved") == std::string::npos &&
        stringOf(error, "message").find("material_route_unserved") == std::string::npos)
        return std::nullopt;
    return std::string{"No forward material pass serves the route, so it stays on auto — "
                       "add a forward material pass first."};
}

} // namespace PelicanStudio
