#pragma once

#include "framegraphbufferdefinition.hpp"
#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace Pelican {

struct RayTracingProjectConfig;

std::vector<FrameGraphBufferDefinition> parseFrameGraphBufferDefinitionsFromJson(const nlohmann::json &config_json);
std::unordered_set<std::string> frameGraphBufferNameSet(const std::vector<FrameGraphBufferDefinition> &definitions);
std::vector<ComputeTaskDefinition> parseComputeTaskDefinitionsFromConfigJson(
    const nlohmann::json &config_json,
    const RayTracingProjectConfig *ray_tracing_project = nullptr);
void validateComputeTaskBufferContracts(
    std::span<const FrameGraphBufferDefinition>
        buffer_definitions,
    std::span<const ComputeTaskDefinition>
        task_definitions);

} // namespace Pelican
