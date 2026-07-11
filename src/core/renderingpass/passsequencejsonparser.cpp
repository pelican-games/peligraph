#include "passsequencejsonparser.hpp"
#include "passdefinitionjsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "renderingpassvalidation.hpp"
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace Pelican {

std::vector<PassDefinition> parsePassSequenceFromJson(const nlohmann::json &pass_set_json,
                                                      const std::string &rendering_pass_name,
                                                      const RenderTargetNameResolver &rt_resolver,
                                                      const RenderTargetMetadataResolver &rt_metadata,
                                                      const std::unordered_set<std::string> &buffer_names) {
    if (!pass_set_json.contains("passes")) {
        throw std::runtime_error("Rendering pass requires passes array: " + rendering_pass_name);
    }
    const auto &passes_json = pass_set_json.at("passes");
    if (!passes_json.is_array()) {
        throw std::runtime_error("Rendering pass requires passes array: " + rendering_pass_name);
    }

    std::vector<PassDefinition> passes;
    std::unordered_set<std::string> pass_names;
    ProducedRenderTargetSet produced_targets;
    for (const auto &pass_json : passes_json) {
        const std::string pass_name = parseStringField(pass_json, "name", "pass");
        validateName(pass_name, "Pass");
        if (!pass_names.insert(pass_name).second) {
            throw std::runtime_error("Duplicate pass name: " + pass_name);
        }

        const auto type = pass_json.value("type", std::string{});
        if (type == "canonical_anchor") {
            continue;
        }

        auto parsed_pass = parsePassDefinitionFromJson(pass_json, rt_resolver, rt_metadata, buffer_names);
        validatePassInputsProduced(parsed_pass, produced_targets, rt_metadata);
        recordPassOutputs(parsed_pass, produced_targets);
        passes.push_back(std::move(parsed_pass));
    }

    return passes;
}

} // namespace Pelican
