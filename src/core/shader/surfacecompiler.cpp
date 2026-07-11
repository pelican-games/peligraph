#include "surfacecompiler.hpp"

#include "../loader/engineresources.hpp"
#include "../../project/materiallowering.hpp"

#include <algorithm>
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

std::string makeParamsInclude(const SurfaceFormatDocument &surface) {
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
        const auto binding = materialCustomTextureFirstBinding + static_cast<std::uint32_t>(i);
        source << "layout(set = PELICAN_SET_MATERIAL, binding = " << binding
               << ") uniform sampler2D pelican_texture_" << texture.name << ";\n";
        source << "vec4 pelican_sample_" << texture.name
               << "(vec2 uv) { return texture(pelican_texture_" << texture.name << ", uv); }\n";
    }
    return source.str();
}

void appendUnique(std::vector<std::string> &defines, std::string define) {
    if (std::find(defines.begin(), defines.end(), define) == defines.end()) {
        defines.push_back(std::move(define));
    }
}

} // namespace

std::string_view surfacePassName(SurfacePass pass) {
    switch (pass) {
    case SurfacePass::main: return "main";
    case SurfacePass::depth: return "depth";
    case SurfacePass::velocity: return "velocity";
    }
    return "unknown";
}

SurfaceShaderComposition composeSurfaceShaders(const SurfaceFormatDocument &surface,
                                                std::string_view source_name, SurfacePass pass,
                                                std::vector<std::string> defines) {
    if (surface.language != SurfaceLanguage::glsl) {
        throw std::runtime_error("surface '" + std::string{source_name} +
                                 "' uses a non-GLSL language; the M3a source backend accepts GLSL only");
    }

    if (surface.hooks.vertex_displace_v1) appendUnique(defines, "PELICAN_HAS_VERTEX_DISPLACE_V1");
    if (surface.hooks.surface_v1) appendUnique(defines, "PELICAN_HAS_SURFACE_V1");
    if (surface.hooks.brdf_v1) appendUnique(defines, "PELICAN_HAS_BRDF_V1");
    if (surface.hooks.ambient_v1) appendUnique(defines, "PELICAN_HAS_AMBIENT_V1");
    if (surface.hooks.lighting_v1) appendUnique(defines, "PELICAN_HAS_LIGHTING_V1");
    if (pass == SurfacePass::depth) appendUnique(defines, "PELICAN_PASS_DEPTH");
    if (pass == SurfacePass::velocity) appendUnique(defines, "PELICAN_PASS_VELOCITY");

    SurfaceShaderComposition composition;
    composition.vertex_source = engineResourceOrThrow("shaders/material/surface_v1.vert");
    composition.fragment_source = engineResourceOrThrow("shaders/material/surface_v1.frag");
    composition.virtual_includes.emplace_back(userIncludeName, makeUserInclude(surface, source_name));
    composition.virtual_includes.emplace_back(paramsIncludeName, makeParamsInclude(surface));
    composition.defines = std::move(defines);
    return composition;
}

SurfaceCompileResult compileSurfaceShaders(ShaderCompiler &compiler,
                                           const SurfaceFormatDocument &surface,
                                           std::string_view source_name, SurfacePass pass,
                                           std::vector<std::string> defines) {
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

} // namespace Pelican
