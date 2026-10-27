#pragma once
#include "graphcomposition.hpp"
#include "featurecompose.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <set>

namespace Pelican {
[[noreturn]] inline void refuseRemovedCanonicalAnchor(const nlohmann::json &name,
    const std::string &pointer, const std::string &document = {}) {
    throw GraphRegistryError("render_graph.canonical_anchor_removed@1",
        {{"anchor", name}, {"pointer", pointer}, {"document", document},
         {"suggestion", "Use after/before with a concrete pass, or frame_node_order."}});
}
inline void rejectCanonicalAnchorFields(const nlohmann::json &value,
    const std::string &pointer = {}, const std::string &document = {}) {
    if (value.is_object()) {
        if (const auto field = value.find("canonical_anchor"); field != value.end())
            refuseRemovedCanonicalAnchor(*field, pointer + "/canonical_anchor", document);
        if (value.value("type", nlohmann::json{}) == "canonical_anchor")
            refuseRemovedCanonicalAnchor("canonical_anchor", pointer + "/type", document);
        for (auto field = value.begin(); field != value.end(); ++field) {
            auto key = field.key();
            std::string escaped;
            for (char c : key) escaped += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1,c);
            rejectCanonicalAnchorFields(field.value(), pointer + "/" + escaped, document);
        }
    } else if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i)
            rejectCanonicalAnchorFields(value[i], pointer + "/" + std::to_string(i), document);
    }
}
inline void rejectRemovedCanonicalAuthoring(const nlohmann::json &value,
    const std::string &document, const nlohmann::json *context = nullptr) {
    rejectCanonicalAnchorFields(value, {}, document);
    std::set<std::string> passes;
    const auto pass_names = [&](const nlohmann::json &entries) {
        if (!entries.is_array()) return;
        for (const auto &entry : entries) {
            if (!entry.is_object()) continue;
            const auto &pass = entry.contains("pass") ? entry.at("pass") : entry;
            if (pass.is_object() && pass.contains("name") && pass.at("name").is_string())
                passes.insert(pass.at("name").get<std::string>());
        }
    };
    const auto collect = [&](const auto &self, const nlohmann::json &node) -> void {
        if (node.is_object()) {
            if (const auto entries = node.find("passes"); entries != node.end()) pass_names(*entries);
            for (const auto &child : node) self(self, child);
        } else if (node.is_array()) for (const auto &child : node) self(self, child);
    };
    if (value.is_array()) pass_names(value); // A subgraph provider returns a pass array.
    collect(collect, value);
    if (context != nullptr) collect(collect, *context);
    const auto scan = [&](const auto &self, const nlohmann::json &node, const std::string &pointer) -> void {
        if (node.is_object()) {
            const auto insert = node.find("insert");
            if (insert != node.end() && insert->is_string()) {
                const auto text = insert->get<std::string>();
                const auto at = text.find(':');
                if (at != std::string::npos && (text.substr(0, at) == "after" || text.substr(0, at) == "before")) {
                    const auto name = text.substr(at + 1);
                    if (isCanonicalRenderPassAnchor(name) && !passes.contains(name))
                        refuseRemovedCanonicalAnchor(name, pointer + "/insert", document);
                }
            }
            for (auto it = node.begin(); it != node.end(); ++it)
                self(self, it.value(), (nlohmann::json::json_pointer(pointer) / it.key()).to_string());
        } else if (node.is_array())
            for (std::size_t i = 0; i < node.size(); ++i) self(self, node[i], pointer + "/" + std::to_string(i));
    };
    scan(scan, value, {});
}
}
