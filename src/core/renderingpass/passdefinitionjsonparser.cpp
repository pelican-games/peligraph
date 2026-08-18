#include "passdefinitionjsonparser.hpp"
#include "materialpassinfojsonparser.hpp"
#include "passattachmentoptionsjsonparser.hpp"
#include "passinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "renderingpasstargetjsonparser.hpp"
#include "renderingpassvalidation.hpp"
#include "../userpublic/render/pass_implementation_abi_v1.hpp"
#include "passshapepolicy.hpp"
#include <stdexcept>
#include <vector>

namespace Pelican {

namespace {

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
    const auto shape_observation = passShapeObservationFromJson(
        pass_type, pass_json, non_image_inputs);
    const auto shape_violations =
        evaluatePassShape(shape_policy, shape_observation);
    if (!shape_violations.empty()) {
        throw std::runtime_error(
            "Pass shape violation '" +
            std::string{passShapeViolationName(
                shape_violations.front().kind)} +
            "' in pass '" + pass_def.name + "': " +
            passShapeViolationDescription(shape_violations.front()));
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
