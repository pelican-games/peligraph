#include "graphinputs.hpp"

#include "../../project/graphmodule.hpp"
#include "../loader/scene.hpp"
#include "../ecs/core.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../userpublic/components/predefined.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

#include <stdexcept>

namespace Pelican {
void validateGraphInputs(const ValueGraph::Graph &graph) {
    auto names = graph.inputNames();
    // CT-1: the table the catalog lists under value.input (input_names).
    for (const auto &input : graphFrameInputNames()) names.erase(std::string{input.name});
    for (const auto &[node, source] : graph.morphSources()) names.erase("$morph/" + node);
    if (!names.empty()) throw std::runtime_error("unbound frame graph input: " + *names.begin());
}

ValueGraph::Inputs collectGraphInputs(const ValueGraph::Graph &graph, const SceneLoader &scene,
    const ValueGraph::InactiveScopes &inactive, double time, double delta_time) {
    ValueGraph::Inputs inputs{{"time", time}, {"delta_time", delta_time},
        {"scene_time", time - scene.sceneStartTime()}};
    for (const auto &[node, source] : graph.morphSources()) {
        if (!graph.nodeActive(node, inactive)) continue;
        const auto object = scene.graphOwnerObject(source.owner);
        if (!object) throw std::runtime_error("graph morph owner is unavailable: " + node);
        const auto *model = GET_MODULE(ECSCore).getTemplatePublicModule()
            .tryComponent<SimpleModelViewComponent>(*object);
        const auto weight = model && model->model_instance_id
            ? GET_MODULE(PolygonInstanceContainer).morphWeight(*model->model_instance_id, source.node, source.target)
            : std::nullopt;
        if (!weight) throw std::runtime_error("graph morph target is unavailable: " + node);
        inputs["$morph/" + node] = *weight;
    }
    return inputs;
}
}
