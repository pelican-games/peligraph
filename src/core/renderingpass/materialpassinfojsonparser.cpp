#include "materialpassinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include <stdexcept>

namespace Pelican {

void parseMaterialPassInfoFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isMaterial() || !pass_json.contains("material_range")) {
        return;
    }

    const auto &mat_range = pass_json.at("material_range");
    if (!mat_range.is_object()) {
        throw std::runtime_error("material_range must be an object: " + pass_def.name);
    }

    auto &material_info = pass_def.materialInfo();
    material_info.material_start = parseUint32Field(mat_range, "start", "material_range in pass: " + pass_def.name);
    material_info.material_count = parseUint32Field(mat_range, "count", "material_range in pass: " + pass_def.name);
}

} // namespace Pelican
