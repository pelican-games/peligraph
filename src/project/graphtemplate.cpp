#include "graphtemplate.hpp"

#include "graphcomposition.hpp"

#include <algorithm>
#include <set>

namespace Pelican {

using OrderedJson = nlohmann::ordered_json;

namespace {

[[noreturn]] void invalidTemplate(const std::string &reason) {
    throw GraphTemplateError(std::string{graphTemplateInvalidCode}, {{"reason", reason}},
                             "The graph template's decomposes is not usable: " + reason);
}

} // namespace

GraphTemplateDecomposes readGraphTemplateDecomposes(const OrderedJson &graph_template) {
    if (!graph_template.is_object() || graph_template.value("schema", std::string{}) != "pelican.graph")
        invalidTemplate("the template is not a pelican.graph document");
    const auto found = graph_template.find("decomposes");
    if (found == graph_template.end() || !found->is_object()) invalidTemplate("it has no decomposes object");
    const auto &decomposes = *found;
    GraphTemplateDecomposes result;
    const auto feature = decomposes.find("feature");
    if (feature == decomposes.end() || !feature->is_string() || feature->get_ref<const std::string &>().empty())
        invalidTemplate("decomposes.feature is not a feature reference");
    result.feature = feature->get<std::string>();
    if (const auto remove = decomposes.find("remove_feature"); remove != decomposes.end()) {
        if (!remove->is_boolean()) invalidTemplate("decomposes.remove_feature is not a boolean");
        result.remove_feature = remove->get<bool>();
    }
    if (const auto replacements = decomposes.find("pass_replacements"); replacements != decomposes.end()) {
        if (!replacements->is_object()) invalidTemplate("decomposes.pass_replacements is not an object");
        for (const auto &[pass, node] : replacements->items()) {
            if (!node.is_string()) invalidTemplate("decomposes.pass_replacements." + pass + " is not a node id");
            result.pass_replacements.emplace_back(pass, node.get<std::string>());
        }
    }
    if (const auto defines = decomposes.find("shader_defines"); defines != decomposes.end()) {
        if (!defines->is_array()) invalidTemplate("decomposes.shader_defines is not an array");
        for (const auto &define : *defines) {
            if (!define.is_string()) invalidTemplate("decomposes.shader_defines holds a value that is not a string");
            result.shader_defines.push_back(define.get<std::string>());
        }
    }
    for (const auto *member : {"definitions", "nodes"}) {
        const auto body = graph_template.find(member);
        if (body == graph_template.end()) continue;
        if (std::string_view{member} == "definitions" ? !body->is_object() : !body->is_array())
            invalidTemplate(std::string{"the template's "} + member + " has the wrong type");
    }
    return result;
}

std::string graphTemplateFeatureRef(const OrderedJson &entry) {
    if (entry.is_string()) return entry.get<std::string>();
    if (entry.is_object()) {
        const auto ref = entry.find("ref");
        if (ref != entry.end() && ref->is_string()) return ref->get<std::string>();
    }
    return {};
}

std::size_t countGraphTemplateFeature(const OrderedJson &features, std::string_view feature) {
    if (!features.is_array()) return 0;
    return static_cast<std::size_t>(std::count_if(features.begin(), features.end(), [&](const OrderedJson &entry) {
        return graphTemplateFeatureRef(entry) == feature;
    }));
}

GraphTemplateBody copyGraphTemplateBody(OrderedJson &graph, const OrderedJson &graph_template) {
    auto edited = graph;
    GraphTemplateBody body;
    auto &definitions = edited["definitions"];
    if (definitions.is_null()) definitions = OrderedJson::object();
    if (const auto added = graph_template.find("definitions"); added != graph_template.end()) {
        for (const auto &[name, definition] : added->items()) {
            if (definitions.contains(name))
                throw GraphRegistryError("graph.definition_duplicate@1", {{"definition", name}});
            definitions[name] = definition;
            body.definitions.push_back(name);
        }
    }
    auto &nodes = edited["nodes"];
    if (nodes.is_null()) nodes = OrderedJson::array();
    std::set<std::string> taken;
    for (const auto &node : nodes)
        if (node.is_object() && node.contains("id") && node.at("id").is_string())
            taken.insert(node.at("id").get<std::string>());
    if (const auto added = graph_template.find("nodes"); added != graph_template.end()) {
        for (const auto &node : *added) {
            const auto id = node.at("id").get<std::string>();
            if (!taken.insert(id).second)
                throw GraphRegistryError("graph.node_duplicate@1", {{"node", id}, {"scope", "root"}});
            nodes.push_back(node);
            body.nodes.push_back(id);
        }
    }
    if (!edited.contains("outputs")) edited["outputs"] = OrderedJson::object();
    graph = std::move(edited);
    return body;
}

std::size_t applyGraphTemplateToFeatures(OrderedJson &features, const GraphTemplateDecomposes &decomposes) {
    if (!features.is_array()) return 0;
    auto edited = features;
    std::size_t instances = 0;
    for (auto entry = edited.begin(); entry != edited.end();) {
        if (graphTemplateFeatureRef(*entry) != decomposes.feature) {
            ++entry;
            continue;
        }
        ++instances;
        if (decomposes.remove_feature) {
            entry = edited.erase(entry);
            continue;
        }
        if (!decomposes.pass_replacements.empty()) {
            if (entry->is_string()) *entry = OrderedJson{{"ref", decomposes.feature}};
            auto &replacements = (*entry)["pass_replacements"];
            if (replacements.is_null()) replacements = OrderedJson::object();
            if (!replacements.is_object())
                throw std::runtime_error("The instance of " + decomposes.feature +
                                         " has pass_replacements that are not an object");
            for (const auto &[pass, node] : decomposes.pass_replacements) {
                const auto current = replacements.find(pass);
                if (current == replacements.end()) {
                    replacements[pass] = node;
                } else if (*current != OrderedJson(node)) {
                    throw GraphTemplateError(
                        std::string{graphTemplatePassReplacementConflictCode},
                        {{"feature", decomposes.feature}, {"pass", pass}, {"current", *current}, {"requested", node}},
                        "The instance of " + decomposes.feature + " already replaces " + pass + " with " +
                            current->dump() + ", not " + node);
                }
            }
        }
        ++entry;
    }
    features = std::move(edited);
    return instances;
}

std::vector<std::string> addGraphTemplateShaderDefines(OrderedJson &section, const GraphTemplateDecomposes &decomposes) {
    std::vector<std::string> added;
    if (decomposes.shader_defines.empty()) return added;
    auto list = section.value("shader_defines", OrderedJson::array());
    if (!list.is_array()) throw std::runtime_error("The rendering section's shader_defines is not an array");
    for (const auto &define : decomposes.shader_defines) {
        if (std::find(list.begin(), list.end(), OrderedJson(define)) != list.end()) continue;
        list.push_back(define);
        added.push_back(define);
    }
    if (!added.empty()) section["shader_defines"] = std::move(list);
    return added;
}

OrderedJson applyGraphTemplate(OrderedJson document, const OrderedJson &graph_template) {
    const auto decomposes = readGraphTemplateDecomposes(graph_template);
    if (countGraphTemplateFeature(document.at("rendering").value("features", OrderedJson::array()),
                                  decomposes.feature) == 0)
        throw GraphTemplateError(std::string{graphTemplateFeatureAbsentCode}, {{"feature", decomposes.feature}},
                                 "The rendering section has no instance of " + decomposes.feature);
    (void)copyGraphTemplateBody(document, graph_template);
    auto &section = document.at("rendering");
    (void)applyGraphTemplateToFeatures(section.at("features"), decomposes);
    (void)addGraphTemplateShaderDefines(section, decomposes);
    return document;
}

nlohmann::json applyGraphTemplate(const nlohmann::json &document, const nlohmann::json &graph_template) {
    return nlohmann::json(applyGraphTemplate(OrderedJson(document), OrderedJson(graph_template)));
}

} // namespace Pelican
