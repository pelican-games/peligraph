#include "passdefinitionjsonparser.hpp"
#include "materialpassinfojsonparser.hpp"
#include "passattachmentoptionsjsonparser.hpp"
#include "passinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "renderingpasstargetjsonparser.hpp"
#include "renderingpassvalidation.hpp"
#include "../userpublic/render/pass_implementation_abi_v1.hpp"
#include <stdexcept>

namespace Pelican {

namespace {

void parsePassImplementationProvider(
    PassDefinition &pass_def,
    const nlohmann::json &pass_json) {
    if (!pass_json.contains("implementation")) {
        return;
    }
    if (!pass_def.isFullscreen()) {
        throw std::runtime_error(
            "Pass implementation providers currently support fullscreen "
            "passes only: " +
            pass_def.name);
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
                                           const std::unordered_set<std::string> &buffer_names) {
    if (!pass_json.is_object()) {
        throw std::runtime_error("passes entries must be objects");
    }

    PassDefinition pass_def;
    pass_def.name = parseStringField(pass_json, "name", "pass");
    validateName(pass_def.name, "Pass");
    parsePassTypeFromJson(pass_def, pass_json);
    parsePassImplementationProvider(pass_def, pass_json);

    parsePassOutputTargetsFromJson(pass_def, rt_resolver, pass_json);
    validatePassOutputs(pass_def);
    validatePassSpecificFields(pass_def, pass_json);

    parseMaterialPassInfoFromJson(pass_def, pass_json);

    parsePassInputTargetsFromJson(pass_def, rt_resolver, pass_json, buffer_names);
    parseMaterialPassScreenInputsFromJson(pass_def, pass_json, rt_resolver,
                                          rt_metadata);
    validatePassInputs(pass_def);
    validatePassTargetUsage(pass_def, rt_metadata);
    validateUniqueRenderTargets(pass_def.output_color, "color output", pass_def, rt_metadata);
    validateUniqueRenderTargets(pass_def.input_targets, "input", pass_def, rt_metadata);
    validatePassOutputExtents(pass_def, rt_metadata);
    pass_def.rasterization_samples =
        resolvePassOutputSamples(pass_def, rt_metadata);
    validateMaterialPassAttachments(pass_def, rt_metadata);

    parsePassAttachmentOptionsFromJson(pass_def, pass_json);
    parseFullscreenPassInfoIntoDefinition(pass_def, pass_json);
    parseDebugDrawPassInfoIntoDefinition(pass_def, pass_json);
    parseDebugTextPassInfoIntoDefinition(pass_def, pass_json);
    parseShadowDepthPassInfoIntoDefinition(pass_def, pass_json);
    parseVelocityPassInfoIntoDefinition(pass_def, pass_json);
    return pass_def;
}

} // namespace Pelican
