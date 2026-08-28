#include "passdefinitionjsonparser.hpp"
#include "materialpassinfojsonparser.hpp"
#include "passattachmentoptionsjsonparser.hpp"
#include "passfieldownershipcapabilities.hpp"
#include "passinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "renderingpasstargetjsonparser.hpp"
#include "renderingpassvalidation.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "../userpublic/render/pass_implementation_abi_v1.hpp"
#include "passshapepolicy.hpp"
#include <stdexcept>
#include <vector>

namespace Pelican {

namespace {

std::string passShapeTargetName(
    GlobalRenderTargetId target,
    const RenderTargetMetadataResolver &rt_metadata) {
    if (isSwapchainRenderTarget(target)) {
        return "swapchain";
    }
    return rt_metadata.get(target).name;
}

PassShapeObservation passShapeObservationFromDefinition(
    RenderPassType type,
    const PassDefinition &pass_def,
    const RenderTargetMetadataResolver &rt_metadata) {
    PassShapeObservation observation{.type = type};
    observation.color_outputs.reserve(
        pass_def.output_color.size());
    for (const auto &attachment : pass_def.output_color) {
        observation.color_outputs.push_back(
            passShapeTargetName(
                attachment.target, rt_metadata));
    }
    if (isSwapchainRenderTarget(
            pass_def.output_depth.target) ||
        isConcreteRenderTarget(
            pass_def.output_depth.target)) {
        observation.depth_output =
            passShapeTargetName(
                pass_def.output_depth.target,
                rt_metadata);
    }
    observation.inputs.reserve(
        pass_def.input_targets.size() +
        pass_def.input_buffers.size());
    for (std::size_t index = 0;
         index < pass_def.input_targets.size();
         ++index) {
        observation.inputs.push_back(
            PassShapeInputObservation{
                .name = passShapeTargetName(
                    pass_def.input_targets[index],
                    rt_metadata),
                .history =
                    pass_def.input_target_history.at(index),
                .image = true,
            });
    }
    for (const auto &buffer : pass_def.input_buffers) {
        observation.inputs.push_back(
            PassShapeInputObservation{
                .name = buffer,
                .history = false,
                .image = false,
            });
    }
    return observation;
}

void parsePassImplementationProvider(
    PassDefinition &pass_def,
    const nlohmann::json &pass_json) {
    if (!pass_json.contains("implementation")) {
        return;
    }
    const auto &implementation =
        pass_json.at("implementation");
    if (!implementation.is_object() ||
        implementation.size() != 1 ||
        !implementation.contains("provider") ||
        !implementation.at("provider").is_string()) {
        throw std::runtime_error(
            "Pass implementation must be an object containing only a "
            "provider string: " +
            pass_def.name);
    }
    auto provider =
        implementation.at("provider")
            .get<std::string>();
    if (provider.empty() ||
        provider.size() >
            RenderPass::maximumProviderNameBytesV1) {
        throw std::runtime_error(
            "Pass implementation provider name is empty or too long: " +
            pass_def.name);
    }
    pass_def.requested_implementation_provider =
        std::move(provider);
}

void captureDeclaredShaderReferences(
    PassDefinition &pass_def,
    const nlohmann::json &pass_json) {
    const auto shader = pass_json.find("shader");
    if (shader == pass_json.end()) {
        return;
    }
    // Every pass-specific typed parser has already validated the shader
    // object and its stage value types before this capture runs.
    if (!shader->is_object()) {
        throw std::logic_error(
            "typed pass parser accepted a non-object shader: " +
            pass_def.name);
    }
    const auto append = [&](std::string_view field,
                            DeclaredShaderStage stage) {
        const auto found = shader->find(field);
        if (found == shader->end()) {
            return;
        }
        if (!found->is_string()) {
            throw std::logic_error(
                "typed pass parser accepted a non-string shader stage: " +
                pass_def.name + "/" + std::string{field});
        }
        pass_def.declared_shader_refs.push_back(
            DeclaredShaderReference{
                .stage = stage,
                .ref = found->get<std::string>(),
            });
    };
    append("vertex", DeclaredShaderStage::vertex);
    append("skinned_vertex",
           DeclaredShaderStage::skinned_vertex);
    append("fragment", DeclaredShaderStage::fragment);
}

} // namespace

PassDefinition parsePassDefinitionFromJson(const nlohmann::json &pass_json,
                                           const RenderTargetNameResolver &rt_resolver,
                                           const RenderTargetMetadataResolver &rt_metadata,
                                           const PassShapePolicy &shape_policy,
                                           const std::unordered_set<std::string> &buffer_names) {
    if (!pass_json.is_object()) {
        throw std::runtime_error("passes entries must be objects");
    }

    PassDefinition pass_def;
    pass_def.name = parseStringField(pass_json, "name", "pass");
    validateName(pass_def.name, "Pass");
    validatePassAttachmentOptionsHaveOutputs(
        pass_json, pass_def.name);
    const auto pass_type = parsePassTypeFromJson(pass_def, pass_json);
    pass_def.region_tags =
        parseOptionalRegionTags(
            pass_json, "Pass '" + pass_def.name + "'");
    pass_def.view_family =
        parseRenderViewFamilyId(
            pass_json,
            "Pass '" + pass_def.name + "'");
    parsePassImplementationProvider(pass_def, pass_json);

    const std::vector<std::string> non_image_inputs{
        buffer_names.begin(), buffer_names.end()};
    const auto authored_type = validateAuthoredPassShape(
        shape_policy, pass_json,
        buildPassFieldOwnershipCapabilities(),
        pass_def.name, non_image_inputs);
    if (authored_type != pass_type) {
        throw std::runtime_error(
            "Pass type changed across authored shape validation: " +
            pass_def.name);
    }

    parsePassOutputTargetsFromJson(pass_def, rt_resolver, pass_json);

    parseMaterialPassInfoFromJson(pass_def, pass_json);

    parsePassInputTargetsFromJson(pass_def, rt_resolver, pass_json, buffer_names);
    parseMaterialPassScreenInputsFromJson(pass_def, pass_json, rt_resolver,
                                          rt_metadata);
    parseMaterialPassSurfaceResourcesFromJson(
        pass_def, pass_json, rt_resolver, rt_metadata);
    parseMaterialPassResourcesFromJson(
        pass_def, pass_json, rt_resolver,
        buffer_names);
    validatePassInputs(pass_def);
    validatePassShapeObservation(
        shape_policy,
        passShapeObservationFromDefinition(
            pass_type, pass_def, rt_metadata),
        pass_def.name);
    validatePassTargetUsage(pass_def, rt_metadata);
    validatePassOutputExtents(pass_def, rt_metadata);
    pass_def.rasterization_samples =
        resolvePassOutputSamples(pass_def, rt_metadata);
    validateMaterialPassAttachments(pass_def, rt_metadata);

    parsePassAttachmentOptionsFromJson(pass_def, pass_json);
    parseFullscreenPassInfoIntoDefinition(pass_def, pass_json);
    if (pass_def.isFullscreen() &&
        pass_json.contains("input_sampling") &&
        pass_def.fullscreenInfo().input_sampling.size() !=
            pass_def.input_targets.size()) {
        throw std::runtime_error(
            "Fullscreen pass input_sampling count must match image input count: " +
            pass_def.name);
    }
    parseGenericRasterPassInfoIntoDefinition(
        pass_def, pass_json);
    parseDebugDrawPassInfoIntoDefinition(pass_def, pass_json);
    parseGizmoPassInfoIntoDefinition(pass_def, pass_json);
    parseDebugTextPassInfoIntoDefinition(pass_def, pass_json);
    parseShadowDepthPassInfoIntoDefinition(pass_def, pass_json);
    parseVelocityPassInfoIntoDefinition(pass_def, pass_json);
    parsePickingPassInfoIntoDefinition(pass_def, pass_json);
    pass_def.shader_declaration_parsed = true;
    captureDeclaredShaderReferences(pass_def, pass_json);
    return pass_def;
}

PassDefinition parsePassDefinitionFromJson(
    const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata,
    const std::unordered_set<std::string> &buffer_names) {
    return parsePassDefinitionFromJson(
        pass_json, rt_resolver, rt_metadata,
        defaultPassShapePolicy(), buffer_names);
}

} // namespace Pelican
