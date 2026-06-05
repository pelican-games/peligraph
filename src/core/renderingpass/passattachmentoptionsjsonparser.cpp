#include "passattachmentoptionsjsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"

namespace Pelican {

void parsePassAttachmentOptionsFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (pass_json.contains("clear_color")) {
        pass_def.clear_color = jsonToClearColor(pass_json.at("clear_color"));
    }
    if (pass_json.contains("color_load_op")) {
        pass_def.color_load_op =
            stringToLoadOp(parseStringField(pass_json, "color_load_op", "pass: " + pass_def.name));
    }
    if (pass_json.contains("color_store_op")) {
        pass_def.color_store_op =
            stringToStoreOp(parseStringField(pass_json, "color_store_op", "pass: " + pass_def.name));
    }
}

} // namespace Pelican
