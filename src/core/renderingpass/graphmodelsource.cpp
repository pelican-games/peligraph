#include "../userpublic/userdataerror.hpp"
#include "graphmodelsource.hpp"
#include "framegraphruntime.hpp"
#include "computetask.hpp"
#include "../asset/model.hpp"
#include "../loader/scene.hpp"
#include "../loader/pathresolver.hpp"
#include "../ecs/core.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../userpublic/components/predefined.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../model/skeletalanimation.hpp"
#include <stdexcept>

namespace Pelican {
std::optional<GraphModelInstance> resolveGraphModelSource(
    const GraphInstanceOwner &owner, std::string_view asset, std::string_view resource) {
    const auto object = GET_MODULE(SceneLoader).graphOwnerObject(owner);
    if (!object) return std::nullopt;
    const auto *view = GET_MODULE(ECSCore).getTemplatePublicModule()
        .tryComponent<SimpleModelViewComponent>(*object);
    if (!view || !view->model_instance_id)
        throw UserDataError("graph attribute owner has no model: " + std::string{resource});
    const auto &model = GET_MODULE(ModelAssetContainer).getModelTemplateByName(view->model_name);
    const auto &paths = GET_MODULE(PathResolver);
    if (paths.normalizedReference(model.authored_asset_reference) != paths.normalizedReference(asset))
        throw UserDataError("graph attribute asset differs from its owner model: " + std::string{resource});
    return GraphModelInstance{*view->model_instance_id, &model};
}

void validateGraphPoseAttributes(const std::vector<GraphPoseAttribute> &attributes) {
    if (attributes.empty() || !FastModuleContainer::tryGet<SceneLoader>()) return;
    for (const auto &attribute : attributes) {
        const auto source = resolveGraphModelSource(attribute.owner, attribute.asset, attribute.resource);
        if (!source) continue;
        if (!source->model->skeletal || attribute.node >= source->model->skeletal->nodes.size())
            throw UserDataError("graph pose node is unavailable: " + attribute.resource);
        if (!GET_MODULE(PolygonInstanceContainer).animationNodeMatrix(source->instance, attribute.node))
            throw std::runtime_error("graph pose has no published node matrix: " + attribute.resource);
    }
}

void validateGraphPoseAttributesForModel(const std::vector<GraphPoseAttribute> &attributes,
    ModelAssetId asset, const ModelTemplate &candidate) {
    for (const auto &attribute : attributes) {
        const auto source = resolveGraphModelSource(attribute.owner, attribute.asset, attribute.resource);
        if (!source || source->model->asset_id != asset) continue;
        if (!candidate.skeletal || attribute.node >= candidate.skeletal->nodes.size())
            throw std::runtime_error("model candidate removes graph pose node: " + attribute.resource);
    }
}

void stageGraphPoseAttributes(const CompiledFrameGraphExecution &graph) {
    if (graph.pose_attributes.empty()) return;
    std::vector<std::pair<FrameGraphBufferId, glm::mat4>> values;
    for (const auto &attribute : graph.pose_attributes) {
        const auto source = resolveGraphModelSource(attribute.owner, attribute.asset, attribute.resource);
        if (!source) continue;
        const auto matrix = GET_MODULE(PolygonInstanceContainer).animationNodeMatrix(source->instance, attribute.node);
        if (!matrix) throw std::runtime_error("graph pose has no published node matrix: " + attribute.resource);
        values.emplace_back(graph.buffer_bindings.at(attribute.resource), *matrix);
    }
    auto &buffers = GET_MODULE(FrameGraphResourceContainer);
    for (const auto &[buffer, matrix] : values)
        if (!buffers.stageRuntimeValues(buffer, std::span<const float>{&matrix[0][0], 16}))
            throw std::runtime_error("graph pose output buffer is unavailable");
}
}
