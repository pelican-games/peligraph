#pragma once

#include "passdeclaration.hpp"
#include "rasterpass.hpp"
#include "renderpipeline.hpp"
#include "shaderreference.hpp"
#include "shaderresourceport.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Pelican {

struct FullscreenPassInfo {
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference frag_shader = ShaderReference{"", ShaderStage::fragment, ShaderReferenceKind::explicit_file, false};
    FullscreenPushConstantData push_constants = defaultFullscreenPushConstants;
    bool uses_light_data = defaultFullscreenUsesLightData;
    // Absence is observable compatibility state: legacy fullscreen pipelines
    // keep GraphicsPipelineDesc::color_attachment_states empty. Authored state
    // is expanded and lowered only when raster_state is present in JSON.
    std::optional<RasterFixedFunctionState> raster_state;
    // Empty preserves the legacy linear/repeat policy for every image input.
    // When authored, entries map one-to-one to PassDefinition::input_targets.
    std::vector<ShaderResourcePortSampling> input_sampling;
    // Optional typed annotations over input/input@history. Inputs without an
    // entry keep the existing raw set/binding shader ABI.
    std::vector<ShaderResourcePortDefinition> resource_ports;
    // VI-1: `shader.defines`, "NAME" / "NAME=VALUE" added to this pass's
    // shader compile after the root's shader_defines.
    std::vector<std::string> shader_defines;
};

// One extensible raster entry replaces the need for a new engine-owned pass
// kind for every custom raster technique. The portable contract describes
// draw/fixed state; ShaderReference is resolved only by the selected backend
// adapter.
struct GenericRasterPassInfo {
    RasterPassContract contract;
    std::string shader_implementation{
        defaultRasterShaderImplementation};
    ShaderReference vert_shader =
        ShaderReference{"", ShaderStage::vertex,
                        ShaderReferenceKind::explicit_file,
                        false};
    std::optional<ShaderReference> frag_shader;
    std::vector<ShaderResourcePortDefinition>
        resource_ports;
    // VI-1: `shader.defines`, as a fullscreen pass's.
    std::vector<std::string> shader_defines;
};

struct DebugDrawPassInfo {
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference frag_shader = ShaderReference{"", ShaderStage::fragment, ShaderReferenceKind::explicit_file, false};
};

struct GizmoPassInfo {
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference frag_shader = ShaderReference{"", ShaderStage::fragment, ShaderReferenceKind::explicit_file, false};
};

struct DebugTextPassInfo {
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference frag_shader = ShaderReference{"", ShaderStage::fragment, ShaderReferenceKind::explicit_file, false};
};

struct ShadowDepthPassInfo {
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
};

struct VelocityPassInfo {
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference skinned_vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference frag_shader = ShaderReference{"", ShaderStage::fragment, ShaderReferenceKind::explicit_file, false};
};

struct PickingPassInfo {
    ShaderReference vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference skinned_vert_shader = ShaderReference{"", ShaderStage::vertex, ShaderReferenceKind::explicit_file, false};
    ShaderReference frag_shader = ShaderReference{"", ShaderStage::fragment, ShaderReferenceKind::explicit_file, false};
};

struct UiPassInfo {};

} // namespace Pelican
