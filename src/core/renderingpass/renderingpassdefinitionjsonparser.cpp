#include "renderingpassdefinitionjsonparser.hpp"
#include "passsequencejsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include <stdexcept>

namespace Pelican {

RenderingPassDefinition parseRenderingPassDefinitionFromJson(const nlohmann::json &pass_set_json,
                                                             const RenderTargetNameResolver &rt_resolver,
                                                             const RenderTargetMetadataResolver &rt_metadata) {
    if (!pass_set_json.is_object()) {
        throw std::runtime_error("rendering_passes entries must be objects");
    }

    RenderingPassDefinition pass_def;
    pass_def.name = parseStringField(pass_set_json, "name", "rendering pass");
    validateName(pass_def.name, "Rendering pass");
    pass_def.passes = parsePassSequenceFromJson(pass_set_json, pass_def.name, rt_resolver, rt_metadata);

    return pass_def;
}

} // namespace Pelican
