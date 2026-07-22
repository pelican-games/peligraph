#pragma once

#include "../../project/logicalrendergraph.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Pelican {

struct FrameGraphDefinition;

struct LogicalShadowResourceType {
    std::string resource;
    LogicalType type;
    std::optional<LogicalMaterializationRequirement> materialization;
};

struct LogicalFrameGraphShadowOptions {
    std::vector<LogicalShadowResourceType> resource_types;
    bool allow_legacy_type_fallback = true;
};

CompiledLogicalRenderGraph compileLogicalFrameGraphShadow(
    const FrameGraphDefinition &definition,
    const LogicalTypeRegistry &types,
    const LogicalFrameGraphShadowOptions &options = {});

} // namespace Pelican
