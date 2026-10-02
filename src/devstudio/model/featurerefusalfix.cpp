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
        if (const auto needs = entry.find("needs_writer"); needs != entry.end() && needs->is_boolean())
            target.needs_writer = needs->get<bool>();
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
            auto ops = Json::array({Json{{"op", "replace_node"}, {"scope", scope}, {"node", std::move(edited)}}});
            // UX-12d, decision 3: what reads it through a typed port takes
            // the same type, in the same ops -- alone, either is refused.
            for (const auto &reader : nodes) {
                const auto inputs = reader.is_object() ? reader.find("inputs") : reader.end();
                if (!reader.is_object() || inputs == reader.end() || !inputs->is_object()) continue;
                auto parameters = reader.value("parameters", Json::object());
                if (!parameters.is_object() || !parameters.contains("ports") || !parameters.at("ports").is_object())
                    continue;
                bool changed = false;
                for (const auto &[port, endpoint] : inputs->items()) {
                    if (stringOf(endpoint, "node") != fix.node) continue;
                    auto &declared = parameters["ports"];
                    if (!declared.contains(port) || !declared.at(port).is_object() ||
                        !declared.at(port).contains("type") || declared.at(port).at("type") == fix.type)
                        continue;
                    declared[port]["type"] = fix.type;
                    changed = true;
                }
                if (changed)
                    ops.push_back(Json{{"op", "set_node_parameters"},
                                       {"scope", scope},
                                       {"node", stringOf(reader, "id")},
                                       {"parameters", std::move(parameters)}});
            }
            return ops;
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

// --- UX-12d (engine FR-3).

namespace {

const Json *nodeWithId(const Json &nodes, const std::string &id) {
    if (!nodes.is_array()) return nullptr;
    for (const auto &node : nodes)
        if (stringOf(node, "id") == id) return &node;
    return nullptr;
}

// The id of the render.target_ref of `name` among `nodes`, or among the
// add_node ops of `ops`; empty when there is none.
std::string targetRefOf(const std::string &name, const Json &nodes, const Json &ops) {
    const auto names = [&](const Json &node) {
        return stringOf(node, "op") == "render.target_ref" &&
               stringOf(node.value("parameters", Json::object()), "name") == name;
    };
    if (nodes.is_array())
        for (const auto &node : nodes)
            if (names(node)) return stringOf(node, "id");
    if (ops.is_array())
        for (const auto &op : ops)
            if (stringOf(op, "op") == "add_node" && op.contains("node") && names(op.at("node")))
                return stringOf(op.at("node"), "id");
    return {};
}

// A pass's input or output entry as the target it names.
std::string entryTarget(const Json &entry) {
    if (entry.is_string()) return entry.get<std::string>();
    return stringOf(entry, "target");
}

std::vector<std::string> entryTargets(const Json &list) {
    std::vector<std::string> names;
    if (list.is_string()) names.push_back(list.get<std::string>());
    if (list.is_array())
        for (const auto &entry : list)
            if (auto name = entryTarget(entry); !name.empty()) names.push_back(std::move(name));
    return names;
}

} // namespace

std::string writerCandidateLabel(const WriterCandidate &candidate) {
    return (candidate.node.empty() ? candidate.pass : candidate.node) + "." + candidate.port;
}

std::optional<RequiredTargetUnwritten> requiredTargetUnwritten(const Json &error) {
    if (stringOf(error, "code") != "feature.required_target_unwritten@1") return std::nullopt;
    const auto &fields = fieldsOf(error);
    const auto name = stringOf(fields, "target");
    if (name.empty() || !fields.contains("type") || !fields.at("type").is_object()) return std::nullopt;
    RequiredTargetUnwritten unwritten{stringOf(fields, "feature"), {name, fields.at("type"), {}}, {}};
    unwritten.target.needs_writer = true;
    if (const auto found = fields.find("writer_candidates"); found != fields.end() && found->is_array())
        for (const auto &candidate : *found) {
            WriterCandidate writer{stringOf(candidate, "node"), stringOf(candidate, "pass"),
                                   stringOf(candidate, "port"), stringOf(candidate, "resource")};
            if (writer.port.empty() || (writer.node.empty() && writer.pass.empty())) continue;
            unwritten.candidates.push_back(std::move(writer));
        }
    return unwritten;
}

std::string requiredTargetUnwrittenLine(const RequiredTargetUnwritten &unwritten) {
    std::string line = (unwritten.feature.empty() ? std::string{"The feature"} : unwritten.feature) +
                       " reads " + unwritten.target.name + ", which no pass writes before it.";
    if (unwritten.candidates.size() == 1)
        return line + " Write it from " + writerCandidateLabel(unwritten.candidates.front()) + ".";
    if (unwritten.candidates.size() > 1) return line + " Write it from one of the passes below.";
    return line;
}

std::vector<WriterCandidate> graphWriterCandidates(const Json &type, const Json &graph_nodes) {
    std::vector<WriterCandidate> candidates;
    if (!graph_nodes.is_array()) return candidates;
    const bool depth = requiredTargetIsDepth(type);
    for (const auto &node : graph_nodes) {
        const auto id = stringOf(node, "id");
        const auto inputs = node.is_object() ? node.find("inputs") : node.end();
        if (id.empty() || inputs == node.end() || !inputs->is_object()) continue;
        const auto ports = node.value("parameters", Json::object()).value("ports", Json::object());
        for (const auto &[port, endpoint] : inputs->items()) {
            const auto *image = nodeWithId(graph_nodes, stringOf(endpoint, "node"));
            if (image == nullptr || stringOf(*image, "op") != "render.image") continue;
            if (ports.is_object() && stringOf(ports.value(port, Json::object()), "access") == "read") continue;
            if (requiredTargetIsDepth(image->value("parameters", Json::object()).value("type", Json::object())) !=
                depth)
                continue;
            candidates.push_back(WriterCandidate{id, id, port, stringOf(*image, "id")});
        }
    }
    return candidates;
}

std::string requiredTargetsQuestion(const std::string &feature,
                                    const std::vector<FeatureRequiredTarget> &missing,
                                    const std::string &written, const WriterCandidate &writer) {
    auto line = requiredTargetsQuestion(feature, missing);
    line.pop_back(); // the question mark
    return line + ", and write " + (missing.size() == 1 ? std::string{"it"} : written) + " from " +
           writerCandidateLabel(writer) + "?";
}

Json writerConnectOps(const FeatureRequiredTarget &target, const WriterCandidate &writer, const Json &graph_nodes,
                      const Json &added_ops) {
    Json ops = Json::array();
    if (writer.node.empty()) return ops;
    auto reference = targetRefOf(target.name, graph_nodes, added_ops);
    if (reference.empty()) {
        auto missing = target;
        missing.declare = false;
        missing.reference = true;
        Json taken = graph_nodes.is_array() ? graph_nodes : Json::array();
        if (added_ops.is_array())
            for (const auto &op : added_ops)
                if (op.contains("node") && op.at("node").is_object()) taken.push_back(op.at("node"));
        ops = requiredTargetReferenceOps({missing}, taken);
        reference = stringOf(ops.at(0).at("node"), "id");
    }
    // A writer whose port declares its type (a fullscreen pass's colour) is
    // connected only from a source of exactly that type. UX-13d (engine
    // FR-4): the catalog gives a colour target the colour signal the Graph's
    // ports declare, so the target_ref already has it; only one an older
    // engine typed as the untyped legacy image takes the port's type.
    const auto *writer_node = nodeWithId(graph_nodes, writer.node);
    const auto port_type = writer_node == nullptr ? Json{}
                                                  : writer_node->value("parameters", Json::object())
                                                        .value("ports", Json::object())
                                                        .value(writer.port, Json::object())
                                                        .value("type", Json{});
    if (port_type.is_object()) {
        const Json *declared = nodeWithId(graph_nodes, reference);
        for (const auto *list : std::initializer_list<const Json *>{&added_ops, &ops})
            if (declared == nullptr && list->is_array())
                for (const auto &op : *list)
                    if (op.contains("node") && op.at("node").is_object() && stringOf(op.at("node"), "id") == reference)
                        declared = &op.at("node");
        auto parameters = declared != nullptr ? declared->value("parameters", Json::object()) : Json::object();
        const auto declared_type = parameters.value("type", Json{});
        if (stringOf(declared_type, "semantic") == "pelican.render.legacy_opaque_image@1" &&
            declared_type != port_type) {
            parameters["name"] = target.name;
            parameters["type"] = port_type;
            ops.push_back(Json{{"op", "set_node_parameters"},
                               {"scope", "root"},
                               {"node", reference},
                               {"parameters", std::move(parameters)}});
        }
    }
    ops.push_back(Json{{"op", "set_input"},
                       {"node", writer.node},
                       {"port", writer.port},
                       {"source", {{"node", reference}, {"port", "resource"}}}});
    return ops;
}

std::optional<std::size_t> batchIndexOf(const Json &error) {
    const auto &fields = fieldsOf(error);
    for (const auto *object : {&error, &fields}) {
        if (!object->is_object()) continue;
        const auto found = object->find("batch_index");
        if (found != object->end() && found->is_number_unsigned()) return found->get<std::size_t>();
        if (found != object->end() && found->is_number_integer() && found->get<std::int64_t>() >= 0)
            return static_cast<std::size_t>(found->get<std::int64_t>());
    }
    return std::nullopt;
}

std::optional<ShowTargetPlan> showTargetPlan(const Json &catalog_entry, const std::string &feature_reference,
                                             const Json &context, const Json &graph_nodes) {
    if (!context.is_object() || !context.contains("graphs") || !context.at("graphs").is_array())
        return std::nullopt;
    const auto feature = stringOf(catalog_entry, "name");
    for (const auto &graph : context.at("graphs")) {
        const auto passes = graph.is_object() ? graph.find("passes") : graph.end();
        if (!graph.is_object() || passes == graph.end() || !passes->is_array()) continue;
        const auto declarationOf = [](const Json &pass) { return pass.value("declaration", Json::object()); };
        const auto isFeatures = [&](const Json &pass) {
            const auto provenance = pass.value("provenance", Json::object());
            return stringOf(provenance, "source") == "feature" &&
                   (stringOf(provenance, "provider_reference") == feature_reference ||
                    (!feature.empty() && stringOf(provenance, "provider_feature") == feature));
        };
        // Every target a pass of this graph reads.
        std::set<std::string> read;
        for (const auto &pass : *passes)
            for (const auto &name : entryTargets(declarationOf(pass).value("input", Json::array())))
                read.insert(name);
        // present: the pass that draws to the display, and whose inputs the
        // root (or the forked preset) lets an edit replace.
        const Json *present = nullptr;
        for (const auto &pass : *passes) {
            const auto outputs = entryTargets(declarationOf(pass).value("output", Json::object()).value("color", Json{}));
            if ((std::ranges::find(outputs, "display") != outputs.end() ||
                 std::ranges::find(outputs, "swapchain") != outputs.end()) &&
                (pass.value("editable_root_pass", false) || pass.value("editable_preset_pass", false)))
                present = &pass;
        }
        if (present == nullptr) continue;
        for (const auto &pass : *passes) {
            if (!isFeatures(pass)) continue;
            const auto drawn = entryTargets(declarationOf(pass).value("output", Json::object()).value("color", Json{}));
            for (const auto &target : drawn) {
                if (target == "display" || target == "swapchain" || read.contains(target)) continue;
                ShowTargetPlan plan{feature.empty() ? stringOf(pass.value("provenance", Json::object()), "provider_feature")
                                                    : feature,
                                    target, stringOf(graph, "name"), stringOf(*present, "name"), {}, Json::array()};
                plan.present_inputs = entryTargets(declarationOf(*present).value("input", Json::array()));
                if (plan.present_inputs.empty()) plan.present_inputs.push_back(target);
                else plan.present_inputs.front() = target;
                // The pass the feature's goes after draws its colour into the
                // target first, when a root graph node made it.
                const auto pass_name = stringOf(pass, "name");
                std::string anchor;
                for (const auto &entry : catalog_entry.value("insertion", Json::array()))
                    if (stringOf(entry, "pass") == pass_name && stringOf(entry, "position") != "before")
                        anchor = stringOf(entry, "anchor");
                const auto *node = nodeWithId(graph_nodes, anchor);
                if (node != nullptr && !targetRefOf(target, graph_nodes, Json::array()).empty()) {
                    FeatureRequiredTarget written{target, Json::object(), {}};
                    for (const auto &candidate : graphWriterCandidates(written.type, graph_nodes)) {
                        if (candidate.node != anchor) continue;
                        // Its first colour write: the port the parameters
                        // declare the colour attachment, else the first.
                        const auto ports = node->value("parameters", Json::object()).value("ports", Json::object());
                        const auto attachment =
                            ports.is_object() ? stringOf(ports.value(candidate.port, Json::object()), "attachment")
                                              : std::string{};
                        if (!plan.graph_ops.empty() && attachment != "color") continue;
                        plan.graph_ops = writerConnectOps(written, candidate, graph_nodes, Json::array());
                        if (attachment == "color") break;
                    }
                }
                return plan;
            }
        }
    }
    return std::nullopt;
}

std::string showTargetLine(const ShowTargetPlan &plan) {
    return plan.feature + " draws into " + plan.target + ", which nothing reads. Connect it to " + plan.present +
           " to see it.";
}

} // namespace PelicanStudio
