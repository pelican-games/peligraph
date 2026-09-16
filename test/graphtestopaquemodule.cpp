#include "graphtestopaquemodule.hpp"

namespace Pelican {

void registerTestOpaqueGraphModule(GraphModuleRegistry &registry, TestOpaqueModuleTypes types) {
    const auto id = parseSemanticTypeId("pelican.test.token@1");
    const SemanticTypeSchema schema{id, LogicalTypeConstructor::value, {}, {"opaque"}};
    LogicalTypeRegistry local;
    local.registerSchema(schema);
    const auto token = local.canonicalize(id);
    GraphOperationContract emit;
    emit.outputs.emplace("token", token);
    GraphOperationContract consume;
    consume.inputs.emplace("token", GraphInputContract{exactLogicalTypePattern(local, token)});
    // A contract in the rendering domain that no render backend can lower.
    GraphOperationContract draw;
    draw.domain = GraphExecutionDomain::rendering;
    GraphModuleManifest manifest{
        "pelican.test.opaque", 1,
        "Test-only opaque handle module for the registration and purge gate.",
        types == TestOpaqueModuleTypes::registered ? std::vector<SemanticTypeSchema>{schema}
                                                   : std::vector<SemanticTypeSchema>{},
        {{"test.emit", GraphOperationRegistration{std::move(emit), {}}},
         {"test.consume", GraphOperationRegistration{std::move(consume), {}}},
         {"test.draw", GraphOperationRegistration{std::move(draw), {}}}},
        {"test.consume"}};
    registry.registerModule(std::move(manifest));
}

} // namespace Pelican
