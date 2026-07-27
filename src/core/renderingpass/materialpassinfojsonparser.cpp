#include "materialpassinfojsonparser.hpp"
#include "computetask.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "rendertargetnameresolver.hpp"
#include "../../project/materialformat.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

vk::DeviceSize parseOptionalDeviceSize(
    const nlohmann::json &json,
    std::string_view field,
    std::string_view context) {
    const auto found =
        json.find(std::string{field});
    if (found == json.end()) return 0;
    if (found->is_number_unsigned()) {
        return found->get<vk::DeviceSize>();
    }
    if (found->is_number_integer()) {
        const auto value =
            found->get<std::int64_t>();
        if (value >= 0) {
            return static_cast<vk::DeviceSize>(
                value);
        }
    }
    {
        throw std::runtime_error(
            std::string{context} + " " +
            std::string{field} +
            " must be an unsigned integer");
    }
}

const FrameGraphBufferDefinition &
requireGpuDrawBuffer(
    std::span<const FrameGraphBufferDefinition>
        definitions,
    std::string_view name,
    std::string_view role,
    std::string_view pass_name) {
    const auto found = std::find_if(
        definitions.begin(), definitions.end(),
        [&](const auto &candidate) {
            return candidate.name == name;
        });
    if (found == definitions.end()) {
        throw std::runtime_error(
            "Material pass '" +
            std::string{pass_name} +
            "' gpu_draw_source " +
            std::string{role} +
            " references unknown buffer '" +
            std::string{name} + "'");
    }
    return *found;
}

void validateGpuDrawSourceBufferContract(
    const GpuDrawSourceDefinition &source,
    std::span<const FrameGraphBufferDefinition>
        definitions,
    std::string_view pass_name) {
    const auto &commands = requireGpuDrawBuffer(
        definitions, source.commands, "commands",
        pass_name);
    const auto &count = requireGpuDrawBuffer(
        definitions, source.count, "count",
        pass_name);
    if (commands.command_layout !=
        FrameGraphBufferCommandLayout::
            indexed_draw) {
        throw std::runtime_error(
            "Material pass '" +
            std::string{pass_name} +
            "' gpu_draw_source commands buffer '" +
            source.commands +
            "' requires command_layout 'indexed_draw'");
    }
    if (count.command_layout !=
        FrameGraphBufferCommandLayout::
            draw_count) {
        throw std::runtime_error(
            "Material pass '" +
            std::string{pass_name} +
            "' gpu_draw_source count buffer '" +
            source.count +
            "' requires command_layout 'draw_count'");
    }
    const auto command_bytes =
        static_cast<vk::DeviceSize>(
            source.max_draw_count) *
        frameGraphIndexedDrawCommandBytes;
    if (source.command_offset >
            commands.size ||
        commands.size - source.command_offset <
            command_bytes) {
        throw std::runtime_error(
            "Material pass '" +
            std::string{pass_name} +
            "' gpu_draw_source command range exceeds buffer '" +
            source.commands + "'");
    }
    if (source.count_offset > count.size ||
        count.size - source.count_offset <
            frameGraphDrawCountBytes) {
        throw std::runtime_error(
            "Material pass '" +
            std::string{pass_name} +
            "' gpu_draw_source count value exceeds buffer '" +
            source.count + "'");
    }
    if (source.layout ==
        GpuDrawSourceLayout::
            draw_queue_segments_v1) {
        const auto &segments =
            requireGpuDrawBuffer(
                definitions, source.segments,
                "segments", pass_name);
        if (segments.host_source !=
            FrameGraphHostBufferSource::
                scene_draw_segments_v1) {
            throw std::runtime_error(
                "Material pass '" +
                std::string{pass_name} +
                "' gpu_draw_source segments buffer '" +
                source.segments +
                "' requires host_source "
                "'scene_draw_segments_v1'");
        }
    }
}

} // namespace

std::optional<MaterialDrawTagFilter>
parseMaterialDrawTagFilterFromJson(
    const nlohmann::json &pass_json,
    std::string_view context) {
    const auto found = pass_json.find("material_filter");
    if (found == pass_json.end()) return std::nullopt;
    if (!found->is_object()) {
        throw std::runtime_error(
            std::string{context} +
            " material_filter must be an object");
    }
    for (auto entry = found->begin();
         entry != found->end(); ++entry) {
        if (entry.key() != "include" &&
            entry.key() != "exclude") {
            throw std::runtime_error(
                std::string{context} +
                " material_filter has unknown field '" +
                entry.key() + "'");
        }
    }
    const auto parse = [&](std::string_view field) {
        const auto entry = found->find(std::string{field});
        if (entry == found->end()) {
            return std::vector<std::string>{};
        }
        if (!entry->is_array()) {
            throw std::runtime_error(
                std::string{context} +
                " material_filter." +
                std::string{field} +
                " must be an array of strings");
        }
        std::vector<std::string> tags;
        tags.reserve(entry->size());
        for (const auto &tag : *entry) {
            if (!tag.is_string()) {
                throw std::runtime_error(
                    std::string{context} +
                    " material_filter." +
                    std::string{field} +
                    " must be an array of strings");
            }
            tags.push_back(tag.get<std::string>());
        }
        return tags;
    };
    return makeMaterialDrawTagFilter(
        parse("include"), parse("exclude"),
        std::string{context} + " material_filter");
}

std::optional<GpuDrawSourceDefinition>
parseGpuDrawSourceFromJson(
    const nlohmann::json &pass_json,
    std::string_view context) {
    const auto found =
        pass_json.find("gpu_draw_source");
    if (found == pass_json.end()) {
        return std::nullopt;
    }
    if (!found->is_object()) {
        throw std::runtime_error(
            std::string{context} +
            " gpu_draw_source must be an object");
    }
    for (auto field = found->begin();
         field != found->end(); ++field) {
        if (field.key() != "commands" &&
            field.key() != "count" &&
            field.key() != "max_draw_count" &&
            field.key() != "command_offset" &&
            field.key() != "count_offset" &&
            field.key() != "layout" &&
            field.key() != "segments" &&
            field.key() != "fallback" &&
            field.key() != "execution") {
            throw std::runtime_error(
                std::string{context} +
                " gpu_draw_source has unknown field '" +
                field.key() + "'");
        }
    }
    GpuDrawSourceDefinition result{
        .commands = parseStringField(
            *found, "commands",
            std::string{context} +
                " gpu_draw_source"),
        .count = parseStringField(
            *found, "count",
            std::string{context} +
                " gpu_draw_source"),
        .max_draw_count = parseUint32Field(
            *found, "max_draw_count",
            std::string{context} +
                " gpu_draw_source"),
        .command_offset = parseOptionalDeviceSize(
            *found, "command_offset",
            std::string{context} +
                " gpu_draw_source"),
        .count_offset = parseOptionalDeviceSize(
            *found, "count_offset",
            std::string{context} +
                " gpu_draw_source"),
    };
    if (found->contains("layout")) {
        const auto layout = parseStringField(
            *found, "layout",
            std::string{context} +
                " gpu_draw_source");
        if (layout == "fixed_state_v1") {
            result.layout =
                GpuDrawSourceLayout::
                    fixed_state_v1;
        } else if (
            layout ==
            "draw_queue_segments_v1") {
            result.layout =
                GpuDrawSourceLayout::
                    draw_queue_segments_v1;
        } else {
            throw std::runtime_error(
                std::string{context} +
                " gpu_draw_source has unknown layout '" +
                layout + "'");
        }
    }
    if (found->contains("segments")) {
        result.segments = parseStringField(
            *found, "segments",
            std::string{context} +
                " gpu_draw_source");
    }
    if (result.commands.empty() ||
        result.count.empty()) {
        throw std::runtime_error(
            std::string{context} +
            " gpu_draw_source buffer names must not be empty");
    }
    if (result.commands == result.count) {
        throw std::runtime_error(
            std::string{context} +
            " gpu_draw_source commands and count buffers must differ");
    }
    if (result.layout ==
            GpuDrawSourceLayout::
                fixed_state_v1 &&
        !result.segments.empty()) {
        throw std::runtime_error(
            std::string{context} +
            " gpu_draw_source fixed_state_v1 must not declare segments");
    }
    if (result.layout ==
            GpuDrawSourceLayout::
                draw_queue_segments_v1 &&
        result.segments.empty()) {
        throw std::runtime_error(
            std::string{context} +
            " gpu_draw_source draw_queue_segments_v1 requires segments");
    }
    if (!result.segments.empty() &&
        (result.segments == result.commands ||
         result.segments == result.count)) {
        throw std::runtime_error(
            std::string{context} +
            " gpu_draw_source buffer names must be distinct");
    }
    if (result.max_draw_count == 0) {
        throw std::runtime_error(
            std::string{context} +
            " gpu_draw_source max_draw_count must be greater than zero");
    }
    if (result.command_offset %
                frameGraphIndirectCommandAlignment !=
            0 ||
        result.count_offset %
                frameGraphIndirectCommandAlignment !=
            0) {
        throw std::runtime_error(
            std::string{context} +
            " gpu_draw_source offsets must be 4-byte aligned");
    }
    if (found->contains("fallback")) {
        const auto fallback = parseStringField(
            *found, "fallback",
            std::string{context} +
                " gpu_draw_source");
        if (fallback != "cpu_draw_queue") {
            throw std::runtime_error(
                std::string{context} +
                " gpu_draw_source has unknown fallback '" +
                fallback + "'");
        }
    }
    if (found->contains("execution")) {
        const auto execution = parseStringField(
            *found, "execution",
            std::string{context} +
                " gpu_draw_source");
        if (execution == "automatic") {
            result.execution =
                GpuDrawExecutionMode::
                    automatic;
        } else if (execution == "cpu") {
            result.execution =
                GpuDrawExecutionMode::cpu;
        } else {
            throw std::runtime_error(
                std::string{context} +
                " gpu_draw_source has unknown execution mode '" +
                execution + "'");
        }
    }
    const auto range =
        pass_json.find("material_range");
    const auto range_count =
        range != pass_json.end() &&
                range->is_object() &&
                range->contains("count")
            ? std::optional{
                  parseUint32Field(
                      *range, "count",
                      std::string{context} +
                          " material_range")}
            : std::nullopt;
    if (result.layout ==
            GpuDrawSourceLayout::
                fixed_state_v1 &&
        range_count !=
            std::optional<std::uint32_t>{1}) {
        throw std::runtime_error(
            std::string{context} +
            " gpu_draw_source fixed_state_v1 requires "
            "material_range.count = 1");
    }
    if (result.layout ==
            GpuDrawSourceLayout::
                draw_queue_segments_v1 &&
        (!range_count || *range_count == 0)) {
        throw std::runtime_error(
            std::string{context} +
            " gpu_draw_source draw_queue_segments_v1 requires "
            "a non-empty material_range");
    }
    return result;
}

void validateGpuDrawSourceBufferContracts(
    const nlohmann::json &config_json,
    std::span<const FrameGraphBufferDefinition>
        buffer_definitions) {
    const auto pass_sets =
        config_json.find("rendering_passes");
    if (pass_sets == config_json.end()) return;
    if (!pass_sets->is_array()) {
        throw std::runtime_error(
            "rendering_passes must be an array");
    }
    for (const auto &pass_set : *pass_sets) {
        const auto passes = pass_set.find("passes");
        if (passes == pass_set.end() ||
            !passes->is_array()) {
            continue;
        }
        for (const auto &pass : *passes) {
            if (!pass.is_object() ||
                !pass.contains("gpu_draw_source")) {
                continue;
            }
            const auto name = parseStringField(
                pass, "name", "material pass");
            if (pass.value(
                    "type", std::string{}) !=
                "material") {
                throw std::runtime_error(
                    "Only material passes support gpu_draw_source: " +
                    name);
            }
            const auto source =
                parseGpuDrawSourceFromJson(
                    pass,
                    "Material pass '" + name + "'");
            validateGpuDrawSourceBufferContract(
                *source, buffer_definitions, name);
        }
    }
}

void parseMaterialPassInfoFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isMaterial()) {
        return;
    }

    auto &material_info = pass_def.materialInfo();
    material_info.material_filter =
        parseMaterialDrawTagFilterFromJson(
            pass_json, "Material pass '" + pass_def.name + "'");
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

    if (pass_json.contains("material_variant")) {
        const auto &variant = pass_json.at("material_variant");
        if (!variant.is_string()) {
            throw std::runtime_error(
                "material_variant must be a string: " +
                pass_def.name);
        }
        auto name = variant.get<std::string>();
        validateMaterialVariantName(
            name, "Material pass '" + pass_def.name + "'");
        if (!pass_json.contains("material_contract")) {
            throw std::runtime_error(
                "Material pass '" + pass_def.name +
                "' material_variant requires explicit material_contract");
        }
        if (!material_info.material_filter ||
            material_info.material_filter->include.empty()) {
            throw std::runtime_error(
                "Material pass '" + pass_def.name +
                "' material_variant requires non-empty "
                "material_filter.include");
        }
        material_info.material_variant = std::move(name);
    }

    if (pass_json.contains("material_range")) {
        const auto &mat_range =
            pass_json.at("material_range");
        if (!mat_range.is_object()) {
            throw std::runtime_error(
                "material_range must be an object: " +
                pass_def.name);
        }

        material_info.material_start =
            parseUint32Field(
                mat_range, "start",
                "material_range in pass: " +
                    pass_def.name);
        material_info.material_count =
            parseUint32Field(
                mat_range, "count",
                "material_range in pass: " +
                    pass_def.name);
    }
    material_info.gpu_draw_source =
        parseGpuDrawSourceFromJson(
            pass_json,
            "Material pass '" + pass_def.name + "'");
}

namespace {

LogicalType logicalSourceTypeForTarget(
    const LogicalTypeRegistry &types, const RenderTargetMetadata &metadata) {
    switch (metadata.format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD32SfloatS8Uint:
        return deviceDepthV1(types);
    case vk::Format::eR16G16B16A16Sfloat:
        return sceneLinearHdrV1(types);
    default:
        throw std::runtime_error(
            "render target '" + metadata.name + "' format " +
            vk::to_string(metadata.format) +
            " has no hybrid material screen-input logical type");
    }
}

using MaterialInputContractFactory =
    MaterialPassInputContract (*)(
        const LogicalTypeRegistry &, std::string_view);

struct MaterialResourceReference {
    std::string authored;
    std::string name;
    bool history = false;
};

MaterialResourceReference parseMaterialResourceReference(
    std::string authored, std::string_view context) {
    constexpr std::string_view suffix = "@history";
    if (authored.ends_with(suffix)) {
        auto name = authored.substr(
            0, authored.size() - suffix.size());
        if (name.empty() ||
            name.find('@') != std::string::npos) {
            throw std::runtime_error(
                std::string{context} +
                " has invalid history resource '" +
                authored + "'");
        }
        return {
            std::move(authored),
            std::move(name),
            true,
        };
    }
    if (authored.empty() ||
        authored.find('@') != std::string::npos) {
        throw std::runtime_error(
            std::string{context} +
            " has unknown resource qualifier '" +
            authored + "'");
    }
    return {
        authored,
        std::move(authored),
        false,
    };
}

LogicalReadFootprintKind parseMaterialResourceFootprintKind(
    std::string_view value, std::string_view context) {
    if (value == "same_pixel") {
        return LogicalReadFootprintKind::same_pixel;
    }
    if (value == "neighborhood") {
        return LogicalReadFootprintKind::neighborhood;
    }
    if (value == "arbitrary") {
        return LogicalReadFootprintKind::arbitrary;
    }
    throw std::runtime_error(
        std::string{context} +
        " has unknown footprint '" +
        std::string{value} +
        "'; expected same_pixel, neighborhood, or arbitrary");
}

LogicalReadFootprint parseMaterialResourceFootprint(
    const nlohmann::json &encoded,
    std::string_view context) {
    LogicalReadFootprint result{
        LogicalReadFootprintKind::arbitrary,
        std::nullopt,
    };
    if (encoded.is_string()) {
        result.kind =
            parseMaterialResourceFootprintKind(
                encoded.get_ref<const std::string &>(),
                context);
        return result;
    }
    if (!encoded.is_object()) {
        throw std::runtime_error(
            std::string{context} +
            " must be a footprint string or object");
    }
    for (auto field = encoded.begin();
         field != encoded.end(); ++field) {
        if (field.key() != "kind" &&
            field.key() != "radius") {
            throw std::runtime_error(
                std::string{context} +
                " has unknown field '" +
                field.key() + "'");
        }
    }
    if (!encoded.contains("kind") ||
        !encoded.at("kind").is_string()) {
        throw std::runtime_error(
            std::string{context} +
            " requires string field 'kind'");
    }
    result.kind =
        parseMaterialResourceFootprintKind(
            encoded.at("kind")
                .get_ref<const std::string &>(),
            context);
    if (!encoded.contains("radius")) {
        return result;
    }
    const auto &radius = encoded.at("radius");
    if (!radius.is_number_unsigned()) {
        throw std::runtime_error(
            std::string{context} +
            " radius must be a positive unsigned integer");
    }
    const auto value = radius.get<std::uint64_t>();
    if (value == 0 ||
        value >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            std::string{context} +
            " radius is outside the uint32 range");
    }
    if (result.kind !=
        LogicalReadFootprintKind::neighborhood) {
        throw std::runtime_error(
            std::string{context} +
            " radius is valid only for neighborhood");
    }
    result.radius =
        static_cast<std::uint32_t>(value);
    return result;
}

void parseNamedMaterialPassInputsFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    std::string_view field_name, std::string_view display_name,
    MaterialInputContractFactory make_contract,
    std::vector<MaterialPassInputBinding> &material_inputs,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata) {
    const auto field = std::string{field_name};
    if (!pass_json.contains(field)) return;
    if (!pass_def.isMaterial()) {
        throw std::runtime_error(
            "Only material passes support " + field + ": " +
            pass_def.name);
    }
    if (pass_json.contains("input")) {
        throw std::runtime_error(
            "Material pass " + field +
            " replace positional input: " + pass_def.name);
    }

    const auto &declaration = pass_json.at(field);
    if (!declaration.is_object()) {
        throw std::runtime_error(
            "Material pass " + field +
            " must be an object: " + pass_def.name);
    }

    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto conversions =
        makeBuiltinLogicalTypeConversionRegistry(types);
    material_inputs.reserve(
        material_inputs.size() + declaration.size());
    for (auto entry = declaration.begin();
         entry != declaration.end(); ++entry) {
        validateName(
            entry.key(), std::string{display_name});
        if (!entry.value().is_string()) {
            throw std::runtime_error(
                std::string{display_name} + " '" + entry.key() +
                "' must name a render target: " +
                pass_def.name);
        }
        const auto target_name =
            entry.value().get<std::string>();
        validateName(
            target_name,
            std::string{display_name} + " target");
        const auto target = rt_resolver.resolve(target_name);
        if (!isConcreteRenderTarget(target)) {
            throw std::runtime_error(
                std::string{display_name} +
                " target not found: " + target_name +
                " in pass: " + pass_def.name);
        }

        auto contract = make_contract(types, entry.key());
        const auto metadata = rt_metadata.get(target);
        auto resolved = resolveMaterialScreenInputContract(
            types, conversions, std::move(contract),
            logicalSourceTypeForTarget(types, metadata));
        material_inputs.push_back(MaterialPassInputBinding{
            std::move(resolved.contract), target, false});

        if (std::find(
                pass_def.input_targets.begin(),
                pass_def.input_targets.end(),
                target) == pass_def.input_targets.end()) {
            pass_def.input_targets.push_back(target);
            pass_def.input_target_history.push_back(false);
        }
    }
}

} // namespace

void parseMaterialPassScreenInputsFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata) {
    if (!pass_json.contains("screen_inputs")) {
        return;
    }
    if (!pass_def.isMaterial()) {
        throw std::runtime_error(
            "Only material passes support screen_inputs: " +
            pass_def.name);
    }
    parseNamedMaterialPassInputsFromJson(
        pass_def, pass_json, "screen_inputs",
        "Material screen input",
        makeBuiltinMaterialScreenInputContract,
        pass_def.materialInfo().screen_inputs,
        rt_resolver, rt_metadata);
}

void parseMaterialPassSurfaceResourcesFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata) {
    if (!pass_json.contains("surface_resources")) {
        return;
    }
    if (!pass_def.isMaterial()) {
        throw std::runtime_error(
            "Only material passes support surface_resources: " +
            pass_def.name);
    }
    parseNamedMaterialPassInputsFromJson(
        pass_def, pass_json, "surface_resources",
        "Material surface resource",
        makeBuiltinMaterialPassInputContract,
        pass_def.materialInfo().surface_resources,
        rt_resolver, rt_metadata);
    for (const auto &binding :
         pass_def.materialInfo().surface_resources) {
        if (binding.contract.name !=
            directionalShadowInputContractName) {
            throw std::runtime_error(
                "material surface resource '" +
                binding.contract.name +
                "' is not feature-owned");
        }
    }
}

void parseMaterialPassResourcesFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const std::unordered_set<std::string> &buffer_names) {
    if (!pass_json.contains("material_resources")) {
        return;
    }
    if (!pass_def.isMaterial()) {
        throw std::runtime_error(
            "Only material passes support material_resources: " +
            pass_def.name);
    }
    if (pass_json.contains("input")) {
        throw std::runtime_error(
            "Material pass material_resources replace positional input: " +
            pass_def.name);
    }
    const auto &encoded =
        pass_json.at("material_resources");
    if (!encoded.is_object()) {
        throw std::runtime_error(
            "Material pass material_resources must be an object: " +
            pass_def.name);
    }

    nlohmann::json sanitized =
        nlohmann::json::object();
    std::vector<std::string> reads;
    reads.reserve(encoded.size());
    for (auto entry = encoded.begin();
         entry != encoded.end(); ++entry) {
        if (entry.value().is_string()) {
            sanitized[entry.key()] = entry.value();
            reads.push_back(
                entry.value().get<std::string>());
            continue;
        }
        if (!entry.value().is_object()) {
            throw std::runtime_error(
                "Material resource '" + entry.key() +
                "' must be a resource string or object: " +
                pass_def.name);
        }
        if (entry.value().contains("subresource")) {
            throw std::runtime_error(
                "Material resource '" + entry.key() +
                "' does not yet support image subresource views: " +
                pass_def.name);
        }
        auto object = entry.value();
        object.erase("footprint");
        sanitized[entry.key()] = std::move(object);
        if (entry.value().contains("resource")) {
            if (!entry.value().at("resource").is_string()) {
                throw std::runtime_error(
                    "Material resource '" + entry.key() +
                    "'.resource must be a string: " +
                    pass_def.name);
            }
            reads.push_back(
                entry.value().at("resource")
                    .get<std::string>());
        } else {
            reads.push_back(entry.key());
        }
    }
    const nlohmann::json owner{
        {"resource_ports", std::move(sanitized)}};
    auto ports = parseShaderResourcePortDefinitions(
        owner, reads, std::span<const std::string>{},
        "material pass '" + pass_def.name + "'");

    pass_def.materialInfo().material_resources.reserve(
        pass_def.materialInfo().material_resources.size() +
        ports.size());
    for (auto &port : ports) {
        const auto context =
            "Material resource '" + port.name +
            "' in pass '" + pass_def.name + "'";
        const auto reference =
            parseMaterialResourceReference(
                port.resource, context);
        const auto &source = encoded.at(port.name);
        const auto *footprint_json =
            source.is_object() &&
                    source.contains("footprint")
                ? &source.at("footprint")
                : nullptr;
        const auto footprint =
            reference.history
                ? LogicalReadFootprint{
                      LogicalReadFootprintKind::temporal,
                      std::nullopt}
                : footprint_json != nullptr
                    ? parseMaterialResourceFootprint(
                          *footprint_json,
                          context + ".footprint")
                    : LogicalReadFootprint{
                          LogicalReadFootprintKind::arbitrary,
                          std::nullopt};
        if (reference.history &&
            footprint_json != nullptr) {
            throw std::runtime_error(
                context +
                " @history derives temporal footprint and cannot override it");
        }

        if (reference.history &&
            buffer_names.contains(reference.name)) {
            throw std::runtime_error(
                context +
                " buffer history is not supported");
        }
        const auto buffer =
            buffer_names.contains(reference.name);
        if (buffer) {
            if (effectiveShaderResourcePortAccess(
                    port, false, false) !=
                ShaderResourcePortAccess::storage) {
                throw std::runtime_error(
                    context +
                    " buffer requires storage access");
            }
            if (port.view !=
                ShaderResourcePortView::shared_2d) {
                throw std::runtime_error(
                    context +
                    " buffer does not support per_view");
            }
            if (source.is_object() &&
                source.contains("sampling")) {
                throw std::runtime_error(
                    context +
                    " buffer does not support sampling");
            }
            if (std::find(
                    pass_def.input_buffers.begin(),
                    pass_def.input_buffers.end(),
                    reference.name) ==
                pass_def.input_buffers.end()) {
                pass_def.input_buffers.push_back(
                    reference.name);
            }
            pass_def.materialInfo()
                .material_resources.push_back(
                    MaterialPassResourceBinding{
                        .port = std::move(port),
                        .buffer = reference.name,
                        .history = false,
                        .footprint = footprint,
                    });
            continue;
        }

        if (effectiveShaderResourcePortAccess(
                port, true, false) !=
            ShaderResourcePortAccess::sampled) {
            throw std::runtime_error(
                context +
                " image currently requires sampled access");
        }
        const auto target =
            rt_resolver.resolve(reference.name);
        if (!isConcreteRenderTarget(target)) {
            throw std::runtime_error(
                context + " resource not found: " +
                reference.authored);
        }
        const auto existing_target = std::find(
            pass_def.input_targets.begin(),
            pass_def.input_targets.end(), target);
        if (existing_target ==
            pass_def.input_targets.end()) {
            pass_def.input_targets.push_back(target);
            pass_def.input_target_history.push_back(
                reference.history);
        } else {
            const auto index = static_cast<std::size_t>(
                std::distance(
                    pass_def.input_targets.begin(),
                    existing_target));
            if (index >=
                pass_def.input_target_history.size() ||
                pass_def.input_target_history[index] !=
                    reference.history) {
                throw std::runtime_error(
                    context +
                    " cannot bind the same target as both current and history");
            }
        }
        pass_def.materialInfo()
            .material_resources.push_back(
                MaterialPassResourceBinding{
                    .port = std::move(port),
                    .target = target,
                    .history = reference.history,
                    .footprint = footprint,
                });
    }
}

} // namespace Pelican
