#pragma once

#include "../model/selectionhub.hpp"

#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

class QWidget;

namespace PelicanStudio {

struct InspectorGraphInput {
    std::string port;
    std::string type;
    bool connected = false;
    std::string source;
    bool has_parameter = false;
    nlohmann::json parameter;
};

struct InspectorGraphOutput {
    std::string port;
    std::string type;
    bool has_value = false;
    double value = 0.0;
    bool bundle = false;
};

// What the Inspector's read-only Graph page shows of one node: the data a
// Graph pick names, read from the model at the time the page is built, not
// from whatever the Graph tab happens to have selected now.
struct InspectorGraphNodeView {
    GraphNodeRef ref;
    bool found = false;
    // A pass the rendering setup supplies: the Graph draws it folded, and the
    // Graph document has no node for it. It isn't gone.
    bool from_rendering_setup = false;
    bool scope_read = false;
    std::string heading;
    std::string op;
    std::string domain;
    std::string evaluation;
    std::string definition;
    bool sink = false;
    nlohmann::json parameters = nlohmann::json::object();
    std::vector<InspectorGraphInput> inputs;
    std::vector<InspectorGraphOutput> outputs;
};

// The operation-specific part of the page. The page owns the heading, the
// parameters and the ports; a section adds what its operations need beside
// them. The empty prefix is the default, and has nothing to add.
struct InspectorGraphSection {
    std::string_view op_prefix;
    void (*fill)(QWidget &page, const InspectorGraphNodeView &node);
};

// Longest prefix wins; the empty prefix is always last.
[[nodiscard]] std::span<const InspectorGraphSection> inspectorGraphSections();
[[nodiscard]] const InspectorGraphSection &
inspectorGraphSectionFor(std::string_view op);

} // namespace PelicanStudio
