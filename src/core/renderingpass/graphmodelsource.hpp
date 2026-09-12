#pragma once
#include "../../project/graphinstanceowner.hpp"
#include "../renderer/modelinstance.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {
struct ModelTemplate;
struct ModelAssetId;
struct CompiledFrameGraphExecution;
struct GraphModelInstance {
    ModelInstanceId instance;
    const ModelTemplate *model;
};
struct GraphPoseAttribute {
    std::string resource;
    GraphInstanceOwner owner;
    std::string asset;
    std::uint32_t node;
};
std::optional<GraphModelInstance> resolveGraphModelSource(
    const GraphInstanceOwner &owner, std::string_view asset, std::string_view resource);
void validateGraphPoseAttributes(const std::vector<GraphPoseAttribute> &attributes);
void validateGraphPoseAttributesForModel(const std::vector<GraphPoseAttribute> &attributes,
    ModelAssetId asset, const ModelTemplate &candidate);
void stageGraphPoseAttributes(const CompiledFrameGraphExecution &graph);
}
