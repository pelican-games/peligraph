#include "outputmodule.hpp"

#include "../graphcomposition.hpp"
#include "../graphmodule.hpp"
#include "../logicalrendertype.hpp"

namespace Pelican {

void registerOutputGraphModule(GraphModuleRegistry &registry) {
    const auto types = std::make_shared<LogicalTypeRegistry>(makeBuiltinLogicalTypeRegistry());
    const auto color = sceneLinearHdrV1(*types);
    GraphOperationContract file;
    file.inputs.emplace("source", GraphInputContract{exactLogicalTypePattern(*types, color)});
    file.domain = GraphExecutionDomain::rendering;
    file.evaluation = GraphOperationEvaluation::direct;
    file.parameter_change = GraphParameterChange::rebuild;
    // VO-1a parameters: changing any of them rebuilds the sink. Lowering
    // validates them and then removes the sink from the rendered graph.
    file.parameter_changes = {{"name", GraphParameterChange::rebuild},
                              {"every_n", GraphParameterChange::rebuild},
                              {"enabled", GraphParameterChange::rebuild}};
    registry.registerModule(GraphModuleManifest{
        "pelican.output", 1,
        "Removable render-target sinks that write raw frames while the engine runs.",
        {},
        {{"output.file", GraphOperationRegistration{file, {}}}},
        {"output.file"}});
}

} // namespace Pelican
