#include "passsequencejsonparser.hpp"
#include "passdefinitionjsonparser.hpp"
#include "passfieldownershipcapabilities.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "renderingpassvalidation.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "rendertargetnameresolver.hpp"
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

        const auto type = validatePassFieldOwnership(
            pass_json, buildPassFieldOwnershipCapabilities());
        if (type == RenderPassType::canonical_anchor) {
            continue;
        }
        if (type == RenderPassType::snapshot_copy) {
            const auto source_name = parseStringField(pass_json, "source", "snapshot copy: " + pass_name);
            const auto destination_name = parseStringField(pass_json, "destination",
                                                           "snapshot copy: " + pass_name);
            const auto source = rt_resolver.resolve(source_name);
            const auto destination = rt_resolver.resolve(destination_name);
            if (!isConcreteRenderTarget(source) || !isConcreteRenderTarget(destination)) {
                throw std::runtime_error("Snapshot copy references an unknown render target: " + pass_name);
            }
            if (!produced_targets.contains(source)) {
                throw std::runtime_error("Snapshot source is not produced before copy: " + source_name +
                                         " in snapshot: " + pass_name);
            }
            const auto source_meta = rt_metadata.get(source);
            const auto destination_meta = rt_metadata.get(destination);
            if (!(source_meta.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
                !(destination_meta.usage & vk::ImageUsageFlagBits::eTransferDst) ||
                !(destination_meta.usage & vk::ImageUsageFlagBits::eSampled)) {
                throw std::runtime_error("Snapshot copy usage mismatch: " + pass_name);
            }
            if (source_meta.format != destination_meta.format ||
                source_meta.extent != destination_meta.extent) {
                throw std::runtime_error("Snapshot copy source/destination mismatch: " + pass_name);
            }
            produced_targets.insert(destination);
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
