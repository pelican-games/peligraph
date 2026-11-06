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
    // VO-1a/VO-1b parameters: changing any of them rebuilds the sink.
    // Lowering validates them and then removes the sink from the rendered
    // graph.
    file.parameter_changes = {{"name", GraphParameterChange::rebuild},
                              {"every_n", GraphParameterChange::rebuild},
                              {"enabled", GraphParameterChange::rebuild},
                              {"format", GraphParameterChange::rebuild},
                              {"when_full", GraphParameterChange::rebuild}};
    GraphOperationContract video = file;
    video.parameter_changes = {{"name", GraphParameterChange::rebuild},
                               {"enabled", GraphParameterChange::rebuild},
                               {"fps_num", GraphParameterChange::rebuild},
                               {"fps_den", GraphParameterChange::rebuild},
                               {"codec", GraphParameterChange::rebuild},
                               {"mode", GraphParameterChange::rebuild}};
    registry.registerModule(GraphModuleManifest{
        "pelican.output", 1,
        "Removable render-target sinks that write image or video frames while the engine runs.",
        {},
        {{"output.file", GraphOperationRegistration{file, {}}},
         {"output.video", GraphOperationRegistration{video, {}}}},
        {"output.file", "output.video"}});
}

} // namespace Pelican
