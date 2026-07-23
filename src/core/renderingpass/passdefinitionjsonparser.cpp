#include "passdefinitionjsonparser.hpp"
#include "materialpassinfojsonparser.hpp"
#include "passattachmentoptionsjsonparser.hpp"
#include "passinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "renderingpasstargetjsonparser.hpp"
#include "renderingpassvalidation.hpp"
#include <stdexcept>

namespace Pelican {

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
