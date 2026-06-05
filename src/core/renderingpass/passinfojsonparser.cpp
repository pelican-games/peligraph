#include "passinfojsonparser.hpp"
#include "fullscreenpassinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"

namespace Pelican {

void parsePassTypeFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    pass_def.pass_info = makePassInfo(parseStringField(pass_json, "type", "pass: " + pass_def.name));

    if (pass_def.isUi()) {
        pass_def.color_load_op = vk::AttachmentLoadOp::eLoad;
    }
}

void parseFullscreenPassInfoIntoDefinition(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isFullscreen()) {
        return;
    }

    pass_def.fullscreenInfo() = parseFullscreenPassInfoFromJson(pass_json, pass_def.name);
}

} // namespace Pelican
