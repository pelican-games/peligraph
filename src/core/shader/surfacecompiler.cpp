#include "surfacecompiler.hpp"

#include "../loader/engineresources.hpp"
#include "../../project/materiallowering.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace Pelican {

std::string makeSurfaceScreenInputLocalReadDefine(
    std::size_t input,
    std::uint32_t input_attachment_index) {
    return std::string{
               surfaceScreenInputLocalReadDefinePrefix} +
           std::to_string(input) + "_LOCAL_READ=" +
           std::to_string(input_attachment_index);
}

std::string makeSurfaceResourceLocalReadDefine(
    std::size_t resource,
    std::uint32_t input_attachment_index) {
    return std::string{
               surfaceResourceLocalReadDefinePrefix} +
           std::to_string(resource) + "_LOCAL_READ=" +
           std::to_string(input_attachment_index);
}

std::string makeSurfaceResourceLayeredDefine(
    std::size_t resource) {
    return std::string{
               surfaceResourceLocalReadDefinePrefix} +
           std::to_string(resource) + "_LAYERED=1";
}

std::string makeSurfaceResourceCubeDefine(
    std::size_t resource) {
    return std::string{
               surfaceResourceLocalReadDefinePrefix} +
           std::to_string(resource) + "_CUBE=1";
}

namespace {

constexpr std::string_view userIncludeName = "__pelican_user_surface.glsl";
constexpr std::string_view paramsIncludeName = "__pelican_surface_params.glsl";
constexpr std::string_view materialOutputsIncludeName =
    "__pelican_material_outputs.glsl";

std::optional<std::uint32_t>
physicalLocalReadIndex(
    std::span<const std::string> defines,
    std::string_view prefix, std::size_t index) {
    const auto key =
        std::string{prefix} +
        std::to_string(index) + "_LOCAL_READ=";
    std::optional<std::uint32_t> result;
    for (const auto &define : defines) {
        if (!define.starts_with(key)) {
            continue;
        }
        if (result) {
            throw std::runtime_error(
                "surface physical local-read define is duplicated: " +
                key);
        }
        const auto encoded =
            std::string_view{define}.substr(key.size());
        std::uint32_t value = 0;
        const auto [end, error] = std::from_chars(
            encoded.data(),
            encoded.data() + encoded.size(), value);
        if (error != std::errc{} ||
            end != encoded.data() + encoded.size()) {
            throw std::runtime_error(
                "surface physical local-read define has an invalid "
                "input-attachment index: " +
                define);
        }
        result = value;
    }
    return result;
}

bool physicalLayeredResource(
    std::span<const std::string> defines,
    std::size_t index) {
    const auto key =
        std::string{
            surfaceResourceLocalReadDefinePrefix} +
        std::to_string(index) + "_LAYERED=";
    std::optional<bool> result;
    for (const auto &define : defines) {
        if (!define.starts_with(key)) {
            continue;
        }
        if (result) {
            throw std::runtime_error(
                "surface physical layered-view define is duplicated: " +
                key);
        }
        const auto encoded =
            std::string_view{define}.substr(key.size());
        if (encoded == "0") {
            result = false;
        } else if (encoded == "1") {
            result = true;
        } else {
            throw std::runtime_error(
                "surface physical layered-view define must be 0 or 1: " +
                define);
        }
    }
    return result.value_or(false);
}

bool physicalCubeResource(
    std::span<const std::string> defines,
    std::size_t index) {
    const auto key =
        std::string{
            surfaceResourceLocalReadDefinePrefix} +
        std::to_string(index) + "_CUBE=";
    std::optional<bool> result;
    for (const auto &define : defines) {
        if (!define.starts_with(key)) {
            continue;
        }
        if (result) {
            throw std::runtime_error(
                "surface physical cube-view define is duplicated: " +
                key);
        }
        const auto encoded =
            std::string_view{define}.substr(key.size());
        if (encoded == "0") {
            result = false;
        } else if (encoded == "1") {
            result = true;
        } else {
            throw std::runtime_error(
                "surface physical cube-view define must be 0 or 1: " +
                define);
        }
    }
    return result.value_or(false);
}

std::string diagnosticSourceName(std::string_view source_name) {
    std::string result{source_name};
    std::replace(result.begin(), result.end(), '\\', '/');
    std::replace(result.begin(), result.end(), '"', '\'');
    return result;
}

std::string makeUserInclude(const SurfaceFormatDocument &surface, std::string_view source_name) {
    std::ostringstream source;
    source << "#line " << surface.code_line << " \"" << diagnosticSourceName(source_name)
           << "\"\n" << surface.code;
    return source.str();
}

std::string accessorType(SurfaceParamType type) {
    switch (type) {
    case SurfaceParamType::floating: return "float";
    case SurfaceParamType::vec2: return "vec2";
    case SurfaceParamType::vec3: return "vec3";
    case SurfaceParamType::vec4:
    case SurfaceParamType::color: return "vec4";
    case SurfaceParamType::integer: return "int";
    }
    throw std::runtime_error("unknown surface parameter type while generating GLSL shim");
}

std::string accessorFunction(SurfaceParamType type) {
    switch (type) {
    case SurfaceParamType::floating: return "pelican_material_float";
    case SurfaceParamType::vec2: return "pelican_material_vec2";
    case SurfaceParamType::vec3: return "pelican_material_vec3";
    case SurfaceParamType::vec4:
    case SurfaceParamType::color: return "pelican_material_vec4";
    case SurfaceParamType::integer: return "pelican_material_int";
    }
    throw std::runtime_error("unknown surface parameter type while generating GLSL shim");
}

std::string textureCoordinateType(
    SurfaceTextureDimension dimension) {
    return dimension == SurfaceTextureDimension::two_d
               ? "vec2"
               : "vec3";
}

std::string textureObjectType(
    SurfaceTextureDimension dimension) {
    switch (dimension) {
    case SurfaceTextureDimension::two_d:
        return "texture2D";
    case SurfaceTextureDimension::cube:
        return "textureCube";
    case SurfaceTextureDimension::two_d_array:
        return "texture2DArray";
    case SurfaceTextureDimension::three_d:
        return "texture3D";
    }
    throw std::runtime_error(
        "unknown surface texture dimension");
}

std::string textureSamplerType(
    const SurfaceTextureDefinition &texture) {
    const auto comparison =
        texture.sampler.compare !=
        SurfaceTextureCompare::none;
    switch (texture.dimension) {
    case SurfaceTextureDimension::two_d:
        return comparison ? "sampler2DShadow"
                          : "sampler2D";
    case SurfaceTextureDimension::cube:
        return comparison ? "samplerCubeShadow"
                          : "samplerCube";
    case SurfaceTextureDimension::two_d_array:
        return comparison ? "sampler2DArrayShadow"
                          : "sampler2DArray";
    case SurfaceTextureDimension::three_d:
        if (comparison) {
            throw std::runtime_error(
                "comparison sampling is unavailable for a 3d "
                "surface texture");
        }
        return "sampler3D";
    }
    throw std::runtime_error(
        "unknown surface texture dimension");
}

std::string textureAccessorReturnType(
    const SurfaceTextureDefinition &texture) {
    return texture.sampler.compare ==
                   SurfaceTextureCompare::none
               ? "vec4"
               : "float";
}

std::string textureAccessorArguments(
    const SurfaceTextureDefinition &texture) {
    auto result =
        textureCoordinateType(texture.dimension) +
        " coordinates";
    if (texture.sampler.compare !=
        SurfaceTextureCompare::none) {
        result += ", float reference";
    }
    return result;
}

std::string textureSampleCoordinates(
    const SurfaceTextureDefinition &texture) {
    if (texture.sampler.compare ==
        SurfaceTextureCompare::none) {
        return "coordinates";
    }
    return texture.dimension ==
                   SurfaceTextureDimension::two_d
               ? "vec3(coordinates, reference)"
               : "vec4(coordinates, reference)";
}

std::string textureAccessorCall(
    const SurfaceTextureDefinition &texture) {
    auto result =
        "pelican_sample_" + texture.name + "(" +
        textureCoordinateType(texture.dimension) +
        "(0.0)";
    if (texture.sampler.compare !=
        SurfaceTextureCompare::none) {
        result += ", 0.0";
    }
    result += ")";
    return result;
}

vk::ShaderStageFlags resourceStages(
    SurfaceResourcePortStage stage) {
    switch (stage) {
    case SurfaceResourcePortStage::vertex:
        return vk::ShaderStageFlagBits::eVertex;
    case SurfaceResourcePortStage::fragment:
        return vk::ShaderStageFlagBits::eFragment;
    case SurfaceResourcePortStage::vertex_fragment:
        return vk::ShaderStageFlagBits::eVertex |
               vk::ShaderStageFlagBits::eFragment;
    }
    throw std::runtime_error(
        "unknown surface resource port stage");
}

std::vector<ShaderResourceInterfaceBinding>
makeSurfaceResourceInterface(
    const SurfaceFormatDocument &surface,
    std::uint32_t first_binding,
    bool clustered_lighting,
    std::span<const std::string> defines) {
    std::vector<ShaderResourceInterfaceBinding> result;
    result.reserve(
        surface.resource_ports.size() +
        (clustered_lighting ? 2u : 0u));
    for (std::size_t index = 0;
         index < surface.resource_ports.size(); ++index) {
        const auto &port = surface.resource_ports[index];
        const auto image =
            port.kind == SurfaceResourcePortKind::image;
        const auto local_read =
            image
                ? physicalLocalReadIndex(
                      defines,
                      surfaceResourceLocalReadDefinePrefix,
                      index)
                : std::nullopt;
        const auto layered =
            image &&
            physicalLayeredResource(
                defines, index);
        const auto cube =
            image &&
            physicalCubeResource(
                defines, index);
        if (local_read && layered) {
            throw std::runtime_error(
                "surface resource port '" + port.name +
                "' cannot select input-attachment and layered sampled "
                "ABIs simultaneously");
        }
        if (local_read && cube) {
            throw std::runtime_error(
                "surface resource port '" + port.name +
                "' cannot select input-attachment and cube sampled "
                "ABIs simultaneously");
        }
        if (layered && cube) {
            throw std::runtime_error(
                "surface resource port '" + port.name +
                "' cannot select layered-array and cube sampled "
                "ABIs simultaneously");
        }
        if (local_read &&
            port.stage !=
                SurfaceResourcePortStage::fragment) {
            throw std::runtime_error(
                "surface resource port '" + port.name +
                "' selected an input attachment but is not "
                "fragment-only");
        }
        result.push_back(
            ShaderResourceInterfaceBinding{
                .port =
                    ShaderResourcePortDefinition{
                        .name = port.name,
                        .resource = port.name,
                        .kind =
                            image
                                ? ShaderResourcePortKind::image
                                : ShaderResourcePortKind::buffer,
                        .buffer_element =
                            image
                                ? std::nullopt
                                : std::optional{
                                      port.element},
                        .access =
                            image
                                ? ShaderResourcePortAccess::sampled
                                : ShaderResourcePortAccess::storage,
                        .view =
                            cube
                                ? ShaderResourcePortView::
                                      cube
                            : layered
                                ? ShaderResourcePortView::
                                      family_array
                                : ShaderResourcePortView::
                                      shared_2d,
                    },
                .binding =
                    first_binding +
                    static_cast<std::uint32_t>(index),
                .descriptor =
                    image
                        ? local_read
                              ? ShaderResourceDescriptorKind::
                                    input_attachment
                              : ShaderResourceDescriptorKind::
                                    combined_image_sampler
                        : ShaderResourceDescriptorKind::
                              storage_buffer,
                .image_view_dimension =
                    image
                                ? cube
                                      ? ReflectedImageViewDimension::
                                            cube
                                  : layered
                                      ? ReflectedImageViewDimension::
                                            two_d_array
                                      : ReflectedImageViewDimension::
                                            two_d
                                : ReflectedImageViewDimension::none,
                .input_attachment_index =
                    local_read,
                .buffer_element = port.element,
                .expected_stages =
                    resourceStages(port.stage),
                .readable = true,
                .writable = false,
            });
    }
    if (clustered_lighting) {
        const auto append =
            [&](std::string name,
                ShaderResourceBufferElement element) {
                if (std::find_if(
                        result.begin(), result.end(),
                        [&](const auto &binding) {
                            return binding.port.name ==
                                   name;
                        }) != result.end()) {
                    throw std::runtime_error(
                        "surface resource port '" +
                        name +
                        "' collides with the standard clustered "
                        "lighting contract");
                }
                const auto binding =
                    first_binding +
                    static_cast<std::uint32_t>(
                        result.size());
                result.push_back(
                    ShaderResourceInterfaceBinding{
                        .port =
                            ShaderResourcePortDefinition{
                                .name = name,
                                .resource = name,
                                .kind =
                                    ShaderResourcePortKind::
                                        buffer,
                                .buffer_element =
                                    element,
                                .access =
                                    ShaderResourcePortAccess::
                                        storage,
                            },
                        .binding = binding,
                        .descriptor =
                            ShaderResourceDescriptorKind::
                                storage_buffer,
                        .image_view_dimension =
                            ReflectedImageViewDimension::none,
                        .buffer_element = element,
                        .expected_stages =
                            vk::ShaderStageFlagBits::
                                eFragment,
                        .readable = true,
                        .writable = false,
                    });
            };
        append(
            "light_inventory",
            ShaderResourceBufferElement::uvec4);
        append(
            "light_selection",
            ShaderResourceBufferElement::
                unsigned_integer);
    }
    return result;
}

std::string makeParamsInclude(
    const SurfaceFormatDocument &surface,
    std::span<const ShaderResourceInterfaceBinding>
        resource_interface,
    std::span<const std::string> defines,
    bool split_samplers = false) {
    const auto layout = makeSurfaceStd140Layout(surface);
    std::ostringstream source;
    source << "// Generated public C-layer accessors for this .surface.\n";
    for (std::size_t i = 0; i < surface.params.size(); ++i) {
        const auto &param = surface.params[i];
        source << accessorType(param.type) << " pelican_param_" << param.name << "() { return "
               << accessorFunction(param.type) << "(pelicanPush.materialIndex, "
               << layout.members[i].offset << "u); }\n";
    }
    for (std::size_t i = 0; i < surface.textures.size(); ++i) {
        const auto &texture = surface.textures[i];
        const auto sampler_type =
            textureSamplerType(texture);
        const auto return_type =
            textureAccessorReturnType(texture);
        const auto arguments =
            textureAccessorArguments(texture);
        const auto coordinates =
            textureSampleCoordinates(texture);
        if (split_samplers) {
            const auto image_binding = materialCustomTextureFirstBinding +
                                       static_cast<std::uint32_t>(i * 2);
            const auto sampler_binding = image_binding + 1;
            source << "layout(set = PELICAN_SET_MATERIAL, binding = " << image_binding
                   << ") uniform "
                   << textureObjectType(texture.dimension)
                   << " pelican_texture_" << texture.name
                   << "_image;\n";
            source << "layout(set = PELICAN_SET_MATERIAL, binding = " << sampler_binding
                   << ") uniform "
                   << (texture.sampler.compare ==
                               SurfaceTextureCompare::none
                           ? "sampler"
                           : "samplerShadow")
                   << " pelican_texture_" << texture.name
                   << "_sampler;\n";
            source << return_type << " pelican_sample_"
                   << texture.name << "(" << arguments
                   << ") { return texture(" << sampler_type
                   << "(pelican_texture_" << texture.name
                   << "_image, pelican_texture_"
                   << texture.name << "_sampler), "
                   << coordinates << "); }\n";
        } else {
            const auto binding = materialCustomTextureFirstBinding + static_cast<std::uint32_t>(i);
            source << "layout(set = PELICAN_SET_MATERIAL, binding = " << binding
                   << ") uniform " << sampler_type
                   << " pelican_texture_" << texture.name
                   << ";\n";
            source << return_type << " pelican_sample_"
                   << texture.name << "(" << arguments
                   << ") { return texture(pelican_texture_"
                   << texture.name << ", " << coordinates
                   << "); }\n";
        }
    }
    for (std::size_t i = 0; i < surface.screen_inputs.size(); ++i) {
        const auto &input = surface.screen_inputs[i];
        const auto local_read =
            physicalLocalReadIndex(
                defines,
                surfaceScreenInputLocalReadDefinePrefix,
                i);
        if (local_read) {
            source
                << "#if defined(PELICAN_SURFACE_STAGE_FRAGMENT)\n"
                << "layout(input_attachment_index = "
                << *local_read
                << ", set = PELICAN_SET_PASS_INPUT, binding = "
                << i
                << ") uniform subpassInput pelican_screen_"
                << input << "_texture;\n";
        } else {
            source
                << "layout(set = PELICAN_SET_PASS_INPUT, binding = "
                << i
                << ") uniform sampler2D pelican_screen_"
                << input << "_texture;\n";
        }
        const auto sample =
            local_read
                ? "subpassLoad(pelican_screen_" +
                      input + "_texture)"
                : "texture(pelican_screen_" +
                      input + "_texture, uv)";
        if (input == "linear_view_depth") {
            source << "vec4 pelican_screen_linear_view_depth(vec2 uv) { "
                      "float device_depth = "
                   << sample
                   << ".r; "
                      "vec4 view_position = inverse(pelicanFrame.projection) * "
                      "vec4(uv * 2.0 - 1.0, device_depth, 1.0); "
                      "float linear_depth = -view_position.z / view_position.w; "
                      "return vec4(linear_depth); }\n";
        } else {
            source << "vec4 pelican_screen_" << input
                   << "(vec2 uv) { return "
                   << sample << "; }\n";
        }
        if (local_read) {
            source
                << "#else\n"
                << "vec4 pelican_screen_" << input
                << "(vec2 uv) { return vec4(0.0); }\n"
                << "#endif\n";
        }
    }
    if (!resource_interface.empty()) {
        source << generateShaderResourcePortInclude(
            resource_interface);
        // A .surface owns the sampling algorithm, while the active material
        // pass owns the physical view shape. Preserve the scalar accessor
        // across that lowering boundary: array-backed variants select the
        // current logical view automatically, and the generated indexed
        // overload remains available for algorithms that intentionally
        // address another family member.
        for (const auto &resource :
             resource_interface) {
            if (resource.descriptor !=
                    ShaderResourceDescriptorKind::
                        combined_image_sampler ||
                resource.image_view_dimension !=
                    ReflectedImageViewDimension::
                        two_d_array) {
                continue;
            }
            source << "vec4 pelican_sample_"
                   << resource.port.name
                   << "(vec2 uv) { return pelican_sample_"
                   << resource.port.name
                   << "(uv, pelican_view_index()); }\n"
                   << "vec4 pelican_sample_lod_"
                   << resource.port.name
                   << "(vec2 uv, float lod) { return pelican_sample_lod_"
                   << resource.port.name
                   << "(uv, pelican_view_index(), lod); }\n";
        }
    }
    return source.str();
}

void appendUnique(std::vector<std::string> &defines, std::string define) {
    if (std::find(defines.begin(), defines.end(), define) == defines.end()) {
        defines.push_back(std::move(define));
    }
}

bool hasDefine(
    const std::vector<std::string> &defines,
    std::string_view name) {
    return std::any_of(
        defines.begin(), defines.end(),
        [name](const auto &define) {
            return define == name ||
                   (define.size() > name.size() &&
                    define.starts_with(name) &&
                    define[name.size()] == '=');
        });
}

std::string materialOutputZero(
    MaterialOutputType type) {
    switch (type) {
    case MaterialOutputType::floating: return "0.0";
    case MaterialOutputType::vec2: return "vec2(0.0)";
    case MaterialOutputType::vec3: return "vec3(0.0)";
    case MaterialOutputType::vec4: return "vec4(0.0)";
    case MaterialOutputType::integer: return "0";
    case MaterialOutputType::ivec2: return "ivec2(0)";
    case MaterialOutputType::ivec3: return "ivec3(0)";
    case MaterialOutputType::ivec4: return "ivec4(0)";
    case MaterialOutputType::unsigned_integer: return "0u";
    case MaterialOutputType::uvec2: return "uvec2(0u)";
    case MaterialOutputType::uvec3: return "uvec3(0u)";
    case MaterialOutputType::uvec4: return "uvec4(0u)";
    }
    throw std::runtime_error(
        "unknown material output type while generating GLSL");
}

std::string materialOutputFloatingValue(
    std::string expression, MaterialOutputType type) {
    switch (type) {
    case MaterialOutputType::floating:
        return "(" + expression + ").x";
    case MaterialOutputType::vec2:
        return "(" + expression + ").xy";
    case MaterialOutputType::vec3:
        return "(" + expression + ").xyz";
    case MaterialOutputType::vec4:
        return expression;
    default:
        throw std::runtime_error(
            "built-in material output source requires a "
            "floating output type");
    }
}

std::string materialOutputDefaultValue(
    const MaterialOutputField &field) {
    std::string expression;
    switch (field.source) {
    case MaterialOutputSource::custom:
        return materialOutputZero(field.type);
    case MaterialOutputSource::surface_base_color:
        expression = "surface.base_color";
        break;
    case MaterialOutputSource::surface_normal:
        expression = "vec4(surface.normal, 1.0)";
        break;
    case MaterialOutputSource::surface_normal_encoded:
        expression =
            "vec4(surface.normal * 0.5 + 0.5, 1.0)";
        break;
    case MaterialOutputSource::surface_material:
        expression =
            "vec4(surface.roughness, surface.metallic, "
            "surface.occlusion, shading_model)";
        break;
    case MaterialOutputSource::input_world_position:
        expression =
            "vec4(input_data.world_position, 1.0)";
        break;
    case MaterialOutputSource::surface_emissive:
        expression = "vec4(surface.emissive, 1.0)";
        break;
    case MaterialOutputSource::lighting_scene_color:
        expression = "scene_color";
        break;
    }
    return materialOutputFloatingValue(
        std::move(expression), field.type);
}

std::string makeMaterialOutputsInclude(
    const MaterialOutputSchema &schema) {
    validateMaterialOutputSchema(schema);
    std::ostringstream source;
    source << "struct PelicanMaterialOutputsV1 {\n";
    for (const auto &output : schema.outputs) {
        source << "    " << materialOutputTypeName(output.type)
               << ' ' << output.name << ";\n";
    }
    source << "};\n"
              "#ifndef PELICAN_MATERIAL_OUTPUT_TYPES_ONLY\n";
    for (std::size_t location = 0;
         location < schema.outputs.size(); ++location) {
        const auto &output = schema.outputs[location];
        source << "layout(location = " << location << ") out "
               << materialOutputTypeName(output.type)
               << " pelican_material_output_" << location
               << ";\n";
    }
    source << "void pelican_initialize_material_outputs_v1(\n"
              "    out PelicanMaterialOutputsV1 outputs,\n"
              "    in PelicanSurfaceInputV1 input_data,\n"
              "    in PelicanSurfaceV1 surface,\n"
              "    in vec4 scene_color,\n"
              "    in float shading_model) {\n";
    for (const auto &output : schema.outputs) {
        source << "    outputs." << output.name << " = "
               << materialOutputDefaultValue(output) << ";\n";
    }
    source << "}\n"
              "void pelican_store_material_outputs_v1(\n"
              "    in PelicanMaterialOutputsV1 outputs) {\n";
    for (std::size_t location = 0;
         location < schema.outputs.size(); ++location) {
        source << "    pelican_material_output_" << location
               << " = outputs."
               << schema.outputs[location].name << ";\n";
    }
    source << "}\n"
              "#endif\n";
    return source.str();
}

SurfaceShaderComposition composeSurfaceShadersImpl(const SurfaceFormatDocument &surface,
                                                    std::string_view source_name, SurfacePass pass,
                                                    std::vector<std::string> defines,
                                                    bool split_samplers,
                                                    std::optional<MaterialOutputSchema>
                                                        material_output_schema) {
    if (surface.language != SurfaceLanguage::glsl) {
        throw std::runtime_error("surface '" + std::string{source_name} +
                                 "' uses a non-GLSL language; the M3a source backend accepts GLSL only");
    }

    if (pass != SurfacePass::forward) {
        constexpr std::string_view
            clustered_define =
                "PELICAN_FEATURE_CLUSTERED_LIGHTING";
        std::erase_if(
            defines,
            [clustered_define](
                const std::string &define) {
                return define ==
                           clustered_define ||
                       (define.size() >
                            clustered_define.size() &&
                        define.starts_with(
                            clustered_define) &&
                        define[
                            clustered_define.size()] ==
                            '=');
            });
    }

    if (surface.hooks.vertex_displace_v1) appendUnique(defines, "PELICAN_HAS_VERTEX_DISPLACE_V1");
    if (surface.hooks.surface_v1) appendUnique(defines, "PELICAN_HAS_SURFACE_V1");
    if (surface.hooks.material_outputs_v1) {
        if (!material_output_schema) {
            throw std::runtime_error(
                "surface '" + std::string{source_name} +
                "' defines pelican_material_outputs_v1 but the "
                "selected material pass has no material_outputs "
                "schema");
        }
        appendUnique(
            defines, "PELICAN_HAS_MATERIAL_OUTPUTS_V1");
    }
    if (surface.hooks.brdf_v1) appendUnique(defines, "PELICAN_HAS_BRDF_V1");
    if (surface.hooks.ambient_v1) appendUnique(defines, "PELICAN_HAS_AMBIENT_V1");
    if (surface.hooks.lighting_v1) appendUnique(defines, "PELICAN_HAS_LIGHTING_V1");
    if (pass == SurfacePass::deferred_geometry)
        appendUnique(defines, "PELICAN_PASS_DEFERRED_GEOMETRY");
    if (pass == SurfacePass::forward) appendUnique(defines, "PELICAN_PASS_FORWARD");
    if (pass == SurfacePass::depth) appendUnique(defines, "PELICAN_PASS_DEPTH");
    if (pass == SurfacePass::velocity) appendUnique(defines, "PELICAN_PASS_VELOCITY");
    if (material_output_schema) {
        if (pass == SurfacePass::depth ||
            pass == SurfacePass::velocity) {
            throw std::runtime_error(
                "material_outputs is unavailable for surface pass '" +
                std::string{surfacePassName(pass)} + "'");
        }
        validateMaterialOutputSchema(
            *material_output_schema,
            "surface '" + std::string{source_name} +
                "' material_outputs");
        if (pass == SurfacePass::deferred_geometry &&
            std::any_of(
                material_output_schema->outputs.begin(),
                material_output_schema->outputs.end(),
                [](const auto &output) {
                    return output.source ==
                           MaterialOutputSource::
                               lighting_scene_color;
                })) {
            throw std::runtime_error(
                "surface '" + std::string{source_name} +
                "' deferred material_outputs cannot use "
                "lighting.scene_color");
        }
        appendUnique(
            defines, "PELICAN_CUSTOM_MATERIAL_OUTPUTS_V1");
    }
    if (pass == SurfacePass::forward &&
        hasDefine(defines, "PELICAN_FEATURE_SHADOW")) {
        appendUnique(
            defines,
            "PELICAN_DIRECTIONAL_SHADOW_BINDING=" +
                std::to_string(surface.screen_inputs.size()));
    }
    const auto feature_input_count =
        pass == SurfacePass::forward &&
                hasDefine(
                    defines, "PELICAN_FEATURE_SHADOW")
            ? 1u
            : 0u;
    const auto clustered_lighting =
        pass == SurfacePass::forward &&
        hasDefine(
            defines,
            "PELICAN_FEATURE_CLUSTERED_LIGHTING");
    auto resource_interface =
        makeSurfaceResourceInterface(
            surface,
            static_cast<std::uint32_t>(
                surface.screen_inputs.size()) +
                feature_input_count,
            clustered_lighting, defines);

    SurfaceShaderComposition composition;
    composition.vertex_source = engineResourceOrThrow("shaders/material/surface_v1.vert");
    composition.fragment_source = engineResourceOrThrow("shaders/material/surface_v1.frag");
    composition.virtual_includes.emplace_back(userIncludeName, makeUserInclude(surface, source_name));
    composition.virtual_includes.emplace_back(
        paramsIncludeName,
        makeParamsInclude(
            surface, resource_interface,
            defines,
            split_samplers));
    if (material_output_schema) {
        composition.virtual_includes.emplace_back(
            materialOutputsIncludeName,
            makeMaterialOutputsInclude(
                *material_output_schema));
    }
    composition.defines = std::move(defines);
    composition.resource_interface =
        std::move(resource_interface);
    composition.material_output_schema =
        std::move(material_output_schema);
    return composition;
}

std::string makeTemplateHookStubs(const SurfaceFormatDocument &surface, vk::ShaderStageFlagBits stage) {
    std::ostringstream keep_alive;
    for (const auto &param : surface.params) keep_alive << "pelican_param_" << param.name << "();";
    for (const auto &texture : surface.textures) {
        keep_alive << textureAccessorCall(texture) << ";";
    }
    for (const auto &resource : surface.resource_ports) {
        const auto stages =
            resourceStages(resource.stage);
        if (!(stages & stage)) continue;
        if (resource.kind ==
            SurfaceResourcePortKind::image) {
            keep_alive << "pelican_sample_"
                       << resource.name
                       << "(vec2(0.0));pelican_size_"
                       << resource.name << "();";
        } else {
            keep_alive << "pelican_load_"
                       << resource.name
                       << "(0u);pelican_count_"
                       << resource.name << "();";
        }
    }
    if (stage == vk::ShaderStageFlagBits::eFragment) {
        for (const auto &input : surface.screen_inputs) {
            keep_alive << "pelican_screen_" << input << "(vec2(0.0));";
        }
        keep_alive << "pelican_light_count();pelican_light(0u, vec3(0.0));"
                      "pelican_shadow(0u, vec3(0.0));pelican_env_ambient(vec3(0.0));";
    }
    std::ostringstream source;
    if (stage == vk::ShaderStageFlagBits::eVertex && surface.hooks.vertex_displace_v1) {
        source << "void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) {"
               << keep_alive.str() << "}\n";
    }
    if (stage == vk::ShaderStageFlagBits::eFragment) {
        if (surface.hooks.surface_v1) {
            source << "void pelican_surface_v1(in PelicanSurfaceInputV1 input_data, "
                      "inout PelicanSurfaceV1 surface) {" << keep_alive.str() << "}\n";
        }
        if (surface.hooks.material_outputs_v1) {
            source << "void pelican_material_outputs_v1("
                      "in PelicanSurfaceInputV1 input_data, "
                      "in PelicanSurfaceV1 surface, "
                      "inout PelicanMaterialOutputsV1 outputs) {"
                   << keep_alive.str() << "}\n";
        }
        if (surface.hooks.brdf_v1) {
            source << "vec3 pelican_brdf_v1(in PelicanSurfaceV1 surface, vec3 light_dir, "
                      "vec3 view_dir, vec3 radiance) {" << keep_alive.str()
                   << "return vec3(0.0); }\n";
        }
        if (surface.hooks.ambient_v1) {
            source << "vec3 pelican_ambient_v1(in PelicanSurfaceV1 surface, vec3 view_dir, "
                      "vec3 ambient) {" << keep_alive.str() << "return vec3(0.0); }\n";
        }
        if (surface.hooks.lighting_v1) {
            source << "vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, "
                      "in PelicanSurfaceInputV1 input_data) {" << keep_alive.str()
                   << "return vec3(0.0); }\n";
        }
    }
    return source.str();
}

std::vector<std::string> stageHookNames(const SurfaceFormatDocument &surface,
                                        vk::ShaderStageFlagBits stage) {
    std::vector<std::string> names;
    if (stage == vk::ShaderStageFlagBits::eVertex) {
        if (surface.hooks.vertex_displace_v1) names.emplace_back("pelican_vertex_displace_v1");
        return names;
    }
    if (surface.hooks.surface_v1) names.emplace_back("pelican_surface_v1");
    if (surface.hooks.material_outputs_v1) {
        names.emplace_back("pelican_material_outputs_v1");
    }
    if (surface.hooks.brdf_v1) names.emplace_back("pelican_brdf_v1");
    if (surface.hooks.ambient_v1) names.emplace_back("pelican_ambient_v1");
    if (surface.hooks.lighting_v1) names.emplace_back("pelican_lighting_v1");
    return names;
}

std::string defaultValueForAccessor(SurfaceParamType type) {
    switch (type) {
    case SurfaceParamType::floating: return "0.0";
    case SurfaceParamType::vec2: return "vec2(0.0)";
    case SurfaceParamType::vec3: return "vec3(0.0)";
    case SurfaceParamType::vec4:
    case SurfaceParamType::color: return "vec4(0.0)";
    case SurfaceParamType::integer: return "0";
    }
    throw std::runtime_error("unknown surface parameter type while generating SPIR-V stub");
}

std::string defaultValueForResourceAccessor(
    ShaderResourceBufferElement element) {
    switch (element) {
    case ShaderResourceBufferElement::floating:
        return "0.0";
    case ShaderResourceBufferElement::vec2:
        return "vec2(0.0)";
    case ShaderResourceBufferElement::vec3:
        return "vec3(0.0)";
    case ShaderResourceBufferElement::vec4:
        return "vec4(0.0)";
    case ShaderResourceBufferElement::integer:
        return "0";
    case ShaderResourceBufferElement::ivec2:
        return "ivec2(0)";
    case ShaderResourceBufferElement::ivec3:
        return "ivec3(0)";
    case ShaderResourceBufferElement::ivec4:
        return "ivec4(0)";
    case ShaderResourceBufferElement::unsigned_integer:
        return "0u";
    case ShaderResourceBufferElement::uvec2:
        return "uvec2(0u)";
    case ShaderResourceBufferElement::uvec3:
        return "uvec3(0u)";
    case ShaderResourceBufferElement::uvec4:
        return "uvec4(0u)";
    case ShaderResourceBufferElement::mat4:
        return "mat4(0.0)";
    }
    throw std::runtime_error(
        "unknown surface resource element while generating SPIR-V stub");
}

std::vector<std::string> generatedAccessorNames(
    const SurfaceFormatDocument &surface,
    vk::ShaderStageFlagBits stage) {
    std::vector<std::string> names;
    for (const auto &param : surface.params) names.push_back("pelican_param_" + param.name);
    for (const auto &texture : surface.textures) names.push_back("pelican_sample_" + texture.name);
    for (const auto &input : surface.screen_inputs) names.push_back("pelican_screen_" + input);
    for (const auto &resource : surface.resource_ports) {
        if (!(resourceStages(resource.stage) &
              stage)) {
            continue;
        }
        if (resource.kind ==
            SurfaceResourcePortKind::image) {
            names.push_back(
                "pelican_sample_" +
                resource.name);
            names.push_back(
                "pelican_size_" +
                resource.name);
        } else {
            names.push_back(
                "pelican_load_" +
                resource.name);
            names.push_back(
                "pelican_count_" +
                resource.name);
        }
    }
    return names;
}

std::string makeUserLibrarySource(const SurfaceFormatDocument &surface, std::string_view source_name,
                                  vk::ShaderStageFlagBits stage) {
    std::ostringstream source;
    source << "#version 460\n"
              "#extension GL_GOOGLE_include_directive : enable\n"
              "#extension GL_GOOGLE_cpp_style_line_directive : enable\n"
              "#include \"pelican_surface_v1.glsl\"\n"
              "#ifdef PELICAN_CUSTOM_MATERIAL_OUTPUTS_V1\n"
              "#define PELICAN_MATERIAL_OUTPUT_TYPES_ONLY 1\n"
              "#include \"__pelican_material_outputs.glsl\"\n"
              "#undef PELICAN_MATERIAL_OUTPUT_TYPES_ONLY\n"
              "#endif\n";
    for (const auto &param : surface.params) {
        source << accessorType(param.type) << " pelican_param_" << param.name
               << "() { return " << defaultValueForAccessor(param.type) << "; }\n";
    }
    for (const auto &texture : surface.textures) {
        source << textureAccessorReturnType(texture)
               << " pelican_sample_" << texture.name
               << "(" << textureAccessorArguments(texture)
               << ") { return "
               << (texture.sampler.compare ==
                           SurfaceTextureCompare::none
                       ? "vec4(0.0)"
                       : "0.0")
               << "; }\n";
    }
    for (const auto &input : surface.screen_inputs) {
        source << "vec4 pelican_screen_" << input
               << "(vec2 uv) { return vec4(0.0); }\n";
    }
    for (const auto &resource : surface.resource_ports) {
        if (resource.kind ==
            SurfaceResourcePortKind::image) {
            source << "vec4 pelican_sample_"
                   << resource.name
                   << "(vec2 uv) { return vec4(0.0); }\n"
                   << "ivec2 pelican_size_"
                   << resource.name
                   << "() { return ivec2(0); }\n";
        } else {
            const auto type =
                shaderResourceBufferElementName(
                    resource.element);
            source << type << " pelican_load_"
                   << resource.name
                   << "(uint index) { return "
                   << defaultValueForResourceAccessor(
                          resource.element)
                   << "; }\n"
                   << "uint pelican_count_"
                   << resource.name
                   << "() { return 0u; }\n";
        }
    }
    if (stage == vk::ShaderStageFlagBits::eFragment) {
        source << "uint pelican_light_count() { return 0u; }\n"
                  "PelicanLightV1 pelican_light(uint index, vec3 world_position) { "
                  "PelicanLightV1 value; value.direction = vec3(0.0); "
                  "value.radiance = vec3(0.0); value.attenuation = 0.0; return value; }\n"
                  "float pelican_shadow(uint index, vec3 world_position) { return 1.0; }\n"
                  "vec3 pelican_env_ambient(vec3 normal) { return vec3(0.0); }\n";
    }
    source << "#include \"" << userIncludeName << "\"\nvoid main() {\n";
    if (stage == vk::ShaderStageFlagBits::eVertex) {
        if (surface.hooks.vertex_displace_v1) {
            source << "PelicanVertexV1 vertex; pelican_vertex_displace_v1(vertex);\n";
        }
        // A stage without a user hook is compiled by the unchanged source
        // backend: no link is needed and no empty export set is manufactured.
    } else {
        source << "PelicanSurfaceInputV1 input_data; PelicanSurfaceV1 surface; vec3 sink;\n";
        if (surface.hooks.surface_v1) source << "pelican_surface_v1(input_data, surface);\n";
        if (surface.hooks.material_outputs_v1) {
            source << "PelicanMaterialOutputsV1 outputs; "
                      "pelican_material_outputs_v1(input_data, surface, outputs);\n";
        }
        if (surface.hooks.brdf_v1) {
            source << "sink = pelican_brdf_v1(surface, vec3(0.0), vec3(0.0), vec3(0.0));\n";
        }
        if (surface.hooks.ambient_v1) {
            source << "sink = pelican_ambient_v1(surface, vec3(0.0), vec3(0.0));\n";
        }
        if (surface.hooks.lighting_v1) source << "sink = pelican_lighting_v1(surface, input_data);\n";
    }
    for (const auto &param : surface.params) source << "pelican_param_" << param.name << "();\n";
    for (const auto &texture : surface.textures) {
        source << textureAccessorCall(texture) << ";\n";
    }
    for (const auto &input : surface.screen_inputs) {
        source << "pelican_screen_" << input << "(vec2(0.0));\n";
    }
    for (const auto &resource : surface.resource_ports) {
        if (resource.kind ==
            SurfaceResourcePortKind::image) {
            source << "pelican_sample_"
                   << resource.name
                   << "(vec2(0.0));pelican_size_"
                   << resource.name << "();\n";
        } else {
            source << "pelican_load_"
                   << resource.name
                   << "(0u);pelican_count_"
                   << resource.name << "();\n";
        }
    }
    if (stage == vk::ShaderStageFlagBits::eFragment) {
        source << "pelican_light_count(); pelican_light(0u, vec3(0.0)); "
                  "pelican_shadow(0u, vec3(0.0)); pelican_env_ambient(vec3(0.0));\n";
    }
    source << "}\n";
    (void)source_name;
    return source.str();
}

std::vector<std::string> engineExports(const SurfaceFormatDocument &surface,
                                       vk::ShaderStageFlagBits stage) {
    auto names =
        generatedAccessorNames(surface, stage);
    if (stage == vk::ShaderStageFlagBits::eFragment) {
        names.emplace_back("pelican_light_count");
        names.emplace_back("pelican_light");
        names.emplace_back("pelican_shadow");
        names.emplace_back("pelican_env_ambient");
    }
    return names;
}

ShaderCompileResult compileExperimentalStage(ShaderCompiler &compiler,
                                             const SurfaceFormatDocument &surface,
                                             std::string_view source_name,
                                             const SurfaceShaderComposition &composition,
                                             vk::ShaderStageFlagBits stage,
                                             std::vector<SpvLinkBinding> &bindings,
                                             std::string &cache_key) {
    auto hooks = stageHookNames(surface, stage);
    if (stage == vk::ShaderStageFlagBits::eFragment &&
        std::find(composition.defines.begin(), composition.defines.end(), "PELICAN_PASS_DEPTH") !=
            composition.defines.end()) {
        hooks.clear();
    }
    if (hooks.empty()) {
        ShaderCompileOptions options;
        options.defines = composition.defines;
        options.virtual_includes = composition.virtual_includes;
        const auto &template_source = stage == vk::ShaderStageFlagBits::eVertex
                                          ? composition.vertex_source : composition.fragment_source;
        const auto template_name = stage == vk::ShaderStageFlagBits::eVertex
                                       ? "engine://shaders/material/surface_v1.vert"
                                       : "engine://shaders/material/surface_v1.frag";
        return compiler.compileSource(template_source, stage, template_name, options);
    }

    ShaderCompileOptions template_options;
    template_options.defines = composition.defines;
    template_options.virtual_includes = composition.virtual_includes;
    for (auto &[name, contents] : template_options.virtual_includes) {
        if (name == userIncludeName) contents = makeTemplateHookStubs(surface, stage);
    }
    const auto &template_source = stage == vk::ShaderStageFlagBits::eVertex
                                      ? composition.vertex_source : composition.fragment_source;
    const auto template_name = stage == vk::ShaderStageFlagBits::eVertex
                                   ? "engine://spvlink/surface_v1.vert"
                                   : "engine://spvlink/surface_v1.frag";
    auto template_result = compiler.compileSource(template_source, stage, template_name,
                                                  template_options);
    if (!template_result.ok) return template_result;

    ShaderCompileOptions user_options;
    user_options.defines = composition.defines;
    user_options.virtual_includes =
        composition.virtual_includes;
    auto user_result = compiler.compileSource(makeUserLibrarySource(surface, source_name, stage),
                                              stage, std::string{source_name} + "#spvlink-user",
                                              user_options);
    if (!user_result.ok) return user_result;

    try {
        SpvLinkRequest request;
        request.template_module = template_result.spirv;
        request.user_module = user_result.spirv;
        request.user_exports = hooks;
        request.template_exports = engineExports(surface, stage);
        request.cache_salts = composition.defines;
        request.cache_salts.emplace_back("surface-abi=v1");
        request.cache_salts.emplace_back(stage == vk::ShaderStageFlagBits::eVertex
                                             ? "stage=vertex" : "stage=fragment");
        request.preserved_descriptor_names = {
            "pelicanMaterials",
            "pelican_directional_shadow_texture",
        };
        for (const auto &resource :
             composition.resource_interface) {
            if (resource.expected_stages &&
                !(resource.expected_stages & stage)) {
                continue;
            }
            request.preserved_descriptor_names.push_back(
                shaderResourcePortVariableName(
                    resource.port.name));
        }
        auto linked = linkSpirvModules(request);
        bindings = std::move(linked.bindings);
        cache_key = std::move(linked.cache_key);
        ShaderCompileResult result;
        result.spirv = std::move(linked.spirv);
        result.log = template_result.log + user_result.log;
        result.ok = true;
        result.cache_hit = template_result.cache_hit && user_result.cache_hit;
        result.dependencies = std::move(template_result.dependencies);
        result.dependencies.insert(result.dependencies.end(),
                                   user_result.dependencies.begin(),
                                   user_result.dependencies.end());
        std::sort(result.dependencies.begin(), result.dependencies.end());
        result.dependencies.erase(std::unique(result.dependencies.begin(),
                                              result.dependencies.end()),
                                  result.dependencies.end());
        return result;
    } catch (const std::exception &error) {
        return {{}, error.what(), false};
    }
}

} // namespace

std::string_view surfacePassName(SurfacePass pass) {
    switch (pass) {
    case SurfacePass::main: return "main";
    case SurfacePass::deferred_geometry: return "deferred_geometry";
    case SurfacePass::forward: return "forward";
    case SurfacePass::depth: return "depth";
    case SurfacePass::velocity: return "velocity";
    }
    return "unknown";
}

SurfacePass surfacePassForMaterialRoute(MaterialRouteClass route) {
    return route == MaterialRouteClass::deferred_geometry
               ? SurfacePass::deferred_geometry
               : SurfacePass::forward;
}

SurfaceShaderComposition composeSurfaceShaders(const SurfaceFormatDocument &surface,
                                                std::string_view source_name, SurfacePass pass,
                                                std::vector<std::string> defines,
                                                std::optional<MaterialOutputSchema>
                                                    material_output_schema) {
    return composeSurfaceShadersImpl(
        surface, source_name, pass, std::move(defines), false,
        std::move(material_output_schema));
}

SurfaceCompileResult compileSurfaceShaders(ShaderCompiler &compiler,
                                           const SurfaceFormatDocument &surface,
                                           std::string_view source_name, SurfacePass pass,
                                           std::vector<std::string> defines,
                                           std::optional<MaterialOutputSchema>
                                               material_output_schema) {
    if (surfaceSpvLinkExperimentalEnabled()) {
        const auto composition = composeSurfaceShadersImpl(
            surface, source_name, pass, std::move(defines), true,
            std::move(material_output_schema));
        SurfaceCompileResult result;
        result.experimental_spv_link = true;
        result.vertex = compileExperimentalStage(compiler, surface, source_name, composition,
                                                 vk::ShaderStageFlagBits::eVertex,
                                                 result.vertex_bindings,
                                                 result.vertex_cache_key);
        if (!result.vertex.ok) return result;
        result.fragment = compileExperimentalStage(compiler, surface, source_name, composition,
                                                   vk::ShaderStageFlagBits::eFragment,
                                                   result.fragment_bindings,
                                                   result.fragment_cache_key);
        return result;
    }
    const auto composition = composeSurfaceShaders(
        surface, source_name, pass, std::move(defines),
        std::move(material_output_schema));
    ShaderCompileOptions options;
    options.defines = composition.defines;
    options.virtual_includes = composition.virtual_includes;
    SurfaceCompileResult result;
    result.vertex = compiler.compileSource(composition.vertex_source,
                                           vk::ShaderStageFlagBits::eVertex,
                                           "engine://shaders/material/surface_v1.vert", options);
    result.fragment = compiler.compileSource(composition.fragment_source,
                                             vk::ShaderStageFlagBits::eFragment,
                                             "engine://shaders/material/surface_v1.frag", options);
    return result;
}

bool surfaceSpvLinkExperimentalEnabled() {
    const auto *value = std::getenv("PELICAN_SPV_LINK");
    return value != nullptr && std::string_view{value} == "experimental";
}

} // namespace Pelican
