#include "materialpassinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include <stdexcept>

namespace Pelican {

void parseMaterialPassInfoFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isMaterial()) {
        return;
    }

    auto &material_info = pass_def.materialInfo();
    if (pass_json.contains("material_contract")) {
        const auto &contract = pass_json.at("material_contract");
        if (!contract.is_string()) {
            throw std::runtime_error("material_contract must be a string: " + pass_def.name);
        }
        const auto parsed = materialPassContractFromName(contract.get<std::string>());
        if (!parsed) {
            throw std::runtime_error("Unknown material_contract '" +
                                     contract.get<std::string>() + "': " + pass_def.name);
        }
        material_info.contract = *parsed;
    }

    if (!pass_json.contains("material_range")) return;

    const auto &mat_range = pass_json.at("material_range");
    if (!mat_range.is_object()) {
        throw std::runtime_error("material_range must be an object: " + pass_def.name);
    }

    material_info.material_start = parseUint32Field(mat_range, "start", "material_range in pass: " + pass_def.name);
    material_info.material_count = parseUint32Field(mat_range, "count", "material_range in pass: " + pass_def.name);
}

} // namespace Pelican
