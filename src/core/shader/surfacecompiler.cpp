#include "surfacecompiler.hpp"

#include "../loader/engineresources.hpp"
#include "../../project/materiallowering.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace Pelican {
namespace {

constexpr std::string_view userIncludeName = "__pelican_user_surface.glsl";
constexpr std::string_view paramsIncludeName = "__pelican_surface_params.glsl";

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
    bool clustered_lighting) {
    std::vector<ShaderResourceInterfaceBinding> result;
    result.reserve(
        surface.resource_ports.size() +
        (clustered_lighting ? 2u : 0u));
    for (std::size_t index = 0;
         index < surface.resource_ports.size(); ++index) {
        const auto &port = surface.resource_ports[index];
        const auto image =
            port.kind == SurfaceResourcePortKind::image;
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
                    },
                .binding =
                    first_binding +
                    static_cast<std::uint32_t>(index),
                .descriptor =
                    image
                        ? ShaderResourceDescriptorKind::
                              combined_image_sampler
                        : ShaderResourceDescriptorKind::
                              storage_buffer,
                .image_view_dimension =
                    image
                        ? ReflectedImageViewDimension::two_d
                        : ReflectedImageViewDimension::none,
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
        if (split_samplers) {
            const auto image_binding = materialCustomTextureFirstBinding +
                                       static_cast<std::uint32_t>(i * 2);
            const auto sampler_binding = image_binding + 1;
            source << "layout(set = PELICAN_SET_MATERIAL, binding = " << image_binding
                   << ") uniform texture2D pelican_texture_" << texture.name << "_image;\n";
            source << "layout(set = PELICAN_SET_MATERIAL, binding = " << sampler_binding
                   << ") uniform sampler pelican_texture_" << texture.name << "_sampler;\n";
            source << "vec4 pelican_sample_" << texture.name
                   << "(vec2 uv) { return texture(sampler2D(pelican_texture_" << texture.name
                   << "_image, pelican_texture_" << texture.name << "_sampler), uv); }\n";
        } else {
            const auto binding = materialCustomTextureFirstBinding + static_cast<std::uint32_t>(i);
            source << "layout(set = PELICAN_SET_MATERIAL, binding = " << binding
                   << ") uniform sampler2D pelican_texture_" << texture.name << ";\n";
            source << "vec4 pelican_sample_" << texture.name
                   << "(vec2 uv) { return texture(pelican_texture_" << texture.name << ", uv); }\n";
        }
    }
    for (std::size_t i = 0; i < surface.screen_inputs.size(); ++i) {
        const auto &input = surface.screen_inputs[i];
        source << "layout(set = PELICAN_SET_PASS_INPUT, binding = " << i
               << ") uniform sampler2D pelican_screen_" << input << "_texture;\n";
        if (input == "linear_view_depth") {
            source << "vec4 pelican_screen_linear_view_depth(vec2 uv) { "
                      "float device_depth = texture("
                      "pelican_screen_linear_view_depth_texture, uv).r; "
                      "vec4 view_position = inverse(pelicanFrame.projection) * "
                      "vec4(uv * 2.0 - 1.0, device_depth, 1.0); "
                      "float linear_depth = -view_position.z / view_position.w; "
                      "return vec4(linear_depth); }\n";
        } else {
            source << "vec4 pelican_screen_" << input
                   << "(vec2 uv) { return texture(pelican_screen_" << input
                   << "_texture, uv); }\n";
        }
    }
    if (!resource_interface.empty()) {
        source << generateShaderResourcePortInclude(
            resource_interface);
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

SurfaceShaderComposition composeSurfaceShadersImpl(const SurfaceFormatDocument &surface,
                                                    std::string_view source_name, SurfacePass pass,
                                                    std::vector<std::string> defines,
                                                    bool split_samplers) {
    if (surface.language != SurfaceLanguage::glsl) {
        throw std::runtime_error("surface '" + std::string{source_name} +
                                 "' uses a non-GLSL language; the M3a source backend accepts GLSL only");
    }

    if (surface.hooks.vertex_displace_v1) appendUnique(defines, "PELICAN_HAS_VERTEX_DISPLACE_V1");
    if (surface.hooks.surface_v1) appendUnique(defines, "PELICAN_HAS_SURFACE_V1");
    if (surface.hooks.brdf_v1) appendUnique(defines, "PELICAN_HAS_BRDF_V1");
    if (surface.hooks.ambient_v1) appendUnique(defines, "PELICAN_HAS_AMBIENT_V1");
    if (surface.hooks.lighting_v1) appendUnique(defines, "PELICAN_HAS_LIGHTING_V1");
    if (pass == SurfacePass::deferred_geometry)
        appendUnique(defines, "PELICAN_PASS_DEFERRED_GEOMETRY");
    if (pass == SurfacePass::forward) appendUnique(defines, "PELICAN_PASS_FORWARD");
    if (pass == SurfacePass::depth) appendUnique(defines, "PELICAN_PASS_DEPTH");
    if (pass == SurfacePass::velocity) appendUnique(defines, "PELICAN_PASS_VELOCITY");
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
            clustered_lighting);

    SurfaceShaderComposition composition;
    composition.vertex_source = engineResourceOrThrow("shaders/material/surface_v1.vert");
    composition.fragment_source = engineResourceOrThrow("shaders/material/surface_v1.frag");
    composition.virtual_includes.emplace_back(userIncludeName, makeUserInclude(surface, source_name));
    composition.virtual_includes.emplace_back(
        paramsIncludeName,
        makeParamsInclude(
            surface, resource_interface,
            split_samplers));
    composition.defines = std::move(defines);
    composition.resource_interface =
        std::move(resource_interface);
    return composition;
}

std::string makeTemplateHookStubs(const SurfaceFormatDocument &surface, vk::ShaderStageFlagBits stage) {
    std::ostringstream keep_alive;
    for (const auto &param : surface.params) keep_alive << "pelican_param_" << param.name << "();";
    for (const auto &texture : surface.textures) {
        keep_alive << "pelican_sample_" << texture.name << "(vec2(0.0));";
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

std::vector<std::string> generatedAccessorNames(const SurfaceFormatDocument &surface) {
    std::vector<std::string> names;
    for (const auto &param : surface.params) names.push_back("pelican_param_" + param.name);
    for (const auto &texture : surface.textures) names.push_back("pelican_sample_" + texture.name);
    for (const auto &input : surface.screen_inputs) names.push_back("pelican_screen_" + input);
    for (const auto &resource : surface.resource_ports) {
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
              "#include \"pelican_surface_v1.glsl\"\n";
    for (const auto &param : surface.params) {
        source << accessorType(param.type) << " pelican_param_" << param.name
               << "() { return " << defaultValueForAccessor(param.type) << "; }\n";
    }
    for (const auto &texture : surface.textures) {
        source << "vec4 pelican_sample_" << texture.name
               << "(vec2 uv) { return vec4(0.0); }\n";
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
        source << "pelican_sample_" << texture.name << "(vec2(0.0));\n";
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
    auto names = generatedAccessorNames(surface);
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
    user_options.virtual_includes.emplace_back(userIncludeName,
                                               makeUserInclude(surface, source_name));
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
                                                std::vector<std::string> defines) {
    return composeSurfaceShadersImpl(surface, source_name, pass, std::move(defines), false);
}

SurfaceCompileResult compileSurfaceShaders(ShaderCompiler &compiler,
                                           const SurfaceFormatDocument &surface,
                                           std::string_view source_name, SurfacePass pass,
                                           std::vector<std::string> defines) {
    if (surfaceSpvLinkExperimentalEnabled()) {
        const auto composition = composeSurfaceShadersImpl(surface, source_name, pass,
                                                           std::move(defines), true);
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
    const auto composition = composeSurfaceShaders(surface, source_name, pass, std::move(defines));
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
