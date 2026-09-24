#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "graphcomposition.hpp"
#include "logicalrendertype.hpp"

namespace Pelican {

// Every failure below is a GraphRegistryError (graphcomposition.hpp).

// Contracts that depend on authored parameters (render resources declare their
// own ports) register a factory instead of a fixed contract. The factory
// supplies ports only; the declared classification below stays authoritative so
// the catalog cannot disagree with what a node resolves to.
using GraphOperationContractFactory = GraphOperationContractResolver;

struct GraphOperationRegistration {
    // Ports when there is no factory; the declared domain, evaluation and
    // parameter change classes in both cases.
    GraphOperationContract contract;
    GraphOperationContractFactory factory;
};

// What one module brings to the common graph: its own semantic types, its
// operations, and which of those operations are sinks. Registration order never
// changes what a document means.
struct GraphModuleManifest {
    std::string name_space;
    std::uint32_t version = 1;
    std::string description;
    std::vector<SemanticTypeSchema> types;
    std::map<std::string, GraphOperationRegistration> operations;
    std::vector<std::string> sinks;
};

class GraphModuleRegistry {
    std::map<std::string, GraphModuleManifest> modules_;
    std::map<std::string, std::string, std::less<>> operation_modules_;
    std::map<std::string, std::string, std::less<>> type_modules_;
    LogicalTypeRegistry types_;

  public:
    // graph.module_unnamed@1 / graph.module_duplicate@1 /
    // graph.operation_duplicate@1 / graph.type_duplicate@1 /
    // graph.sink_unregistered@1 on a bad manifest. A rejected manifest changes
    // nothing, so registration order never leaves a partial module.
    void registerModule(GraphModuleManifest manifest);
    bool contains(std::string_view operation) const;
    bool isSink(std::string_view operation) const;
    // graph.operation_unregistered@1 for an unknown op, graph.type_unregistered@1
    // when a resolved contract produces a type no module registered.
    GraphOperationContract resolveOperation(const nlohmann::json &node) const;
    const LogicalTypeRegistry &typeRegistry() const noexcept { return types_; }
    // Discovery. Modules, types and operations are emitted in name order so the
    // catalog does not depend on registration order.
    nlohmann::ordered_json catalog() const;
};

// pelican.value, pelican.audio, pelican.render and pelican.ui: the operations the engine
// itself brings. Nothing here is special-cased by the graph; a module added
// later registers the same way.
GraphModuleRegistry makeBuiltinGraphModuleRegistry();
// Shared immutable instance for the production entry points.
const GraphModuleRegistry &builtinGraphModuleRegistry();

} // namespace Pelican
