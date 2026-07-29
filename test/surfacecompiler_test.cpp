#include "../src/core/shader/surfacecompiler.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/project/materialformat.hpp"
#include "../src/project/materiallowering.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" /
           "surface_format";
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) throw std::runtime_error("failed to open " + path.string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

SurfaceFormatDocument engineLightingSurface(std::string_view resource_name) {
    const auto source = std::string{"//! pelican.surface v1\n//! language: glsl\n\n"} +
                        engineResourceOrThrow(resource_name);
    return parseSurfaceFormat(source, std::string{"engine://"} + std::string{resource_name});
}

struct ScopedSpvLinkEnvironment {
    std::optional<std::string> previous;
    explicit ScopedSpvLinkEnvironment(const char *value) {
        if (const auto *current = std::getenv("PELICAN_SPV_LINK")) previous = current;
#ifdef _WIN32
        _putenv_s("PELICAN_SPV_LINK", value == nullptr ? "" : value);
#else
        if (value == nullptr) unsetenv("PELICAN_SPV_LINK");
        else setenv("PELICAN_SPV_LINK", value, 1);
#endif
    }
    ~ScopedSpvLinkEnvironment() {
#ifdef _WIN32
        _putenv_s("PELICAN_SPV_LINK", previous ? previous->c_str() : "");
#else
        if (previous) setenv("PELICAN_SPV_LINK", previous->c_str(), 1);
        else unsetenv("PELICAN_SPV_LINK");
#endif
    }
};

void requireCompiled(const SurfaceCompileResult &result) {
    INFO("vertex log: " << result.vertex.log);
    INFO("fragment log: " << result.fragment.log);
    REQUIRE(result.vertex.ok);
    REQUIRE(result.fragment.ok);
    REQUIRE_FALSE(result.vertex.spirv.empty());
    REQUIRE_FALSE(result.fragment.spirv.empty());
}

} // namespace

TEST_CASE("SPV link backend is opt-in and keeps source composition as the default",
          "[surface-compiler][spv-link]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ScopedSpvLinkEnvironment environment{nullptr};
    REQUIRE_FALSE(surfaceSpvLinkExperimentalEnabled());
    const auto surface = parseSurfaceFormat(readText(fixtureRoot() / "valid" / "wp78.surface"),
                                            "wp78.surface");
    ShaderCompiler compiler;
    const auto result = compileSurfaceShaders(compiler, surface, "wp78.surface");
    requireCompiled(result);
    REQUIRE_FALSE(result.experimental_spv_link);
    REQUIRE(result.vertex_cache_key.empty());
    REQUIRE(result.fragment_cache_key.empty());
#endif
}

TEST_CASE("experimental SPV link compiles B hooks with split descriptor types and stable keys",
          "[surface-compiler][spv-link]") {
#if PELICAN_RUNTIME_SHADER_COMPILER && PELICAN_WITH_SPIRV_LINK
    ScopedSpvLinkEnvironment environment{"experimental"};
    REQUIRE(surfaceSpvLinkExperimentalEnabled());
    const auto surface = parseSurfaceFormat(readText(fixtureRoot() / "valid" / "wp78.surface"),
                                            "wp78.surface");
    ShaderCompiler compiler;
    const auto first = compileSurfaceShaders(compiler, surface, "wp78.surface");
    const auto second = compileSurfaceShaders(compiler, surface, "wp78.surface");
    requireCompiled(first);
    requireCompiled(second);
    REQUIRE(first.experimental_spv_link);
    REQUIRE(first.vertex_cache_key == second.vertex_cache_key);
    REQUIRE(first.fragment_cache_key == second.fragment_cache_key);
    REQUIRE(first.fragment_cache_key.find("spirv-headers=09913f") != std::string::npos);
    REQUIRE(first.fragment_cache_key.find("spirv-tools=f289d0") != std::string::npos);
    REQUIRE(first.fragment_cache_key.find("spirv-reflect=c63785") != std::string::npos);
    REQUIRE(first.fragment_cache_key.find("template-sha256=") != std::string::npos);
    REQUIRE(first.fragment_cache_key.find("user-sha256=") != std::string::npos);

    const auto has_binding = [&](std::uint32_t binding, std::string_view type) {
        return std::any_of(first.fragment_bindings.begin(), first.fragment_bindings.end(),
                           [&](const auto &item) {
                               return item.set == 2 && item.binding == binding &&
                                      item.descriptor_type == type;
                           });
    };
    REQUIRE(has_binding(7, "sampled_image"));
    REQUIRE(has_binding(8, "sampler"));

    const auto variant = compileSurfaceShaders(compiler, surface, "wp78.surface",
                                               SurfacePass::main, {"PELICAN_VARIANT_WARM"});
    requireCompiled(variant);
    REQUIRE(variant.fragment_cache_key != first.fragment_cache_key);

    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto bundles = library.loadFromSurface(surface, "wp78.surface");
    REQUIRE_FALSE(library.get(bundles.fragment).binding_table.empty());
    REQUIRE(library.get(bundles.fragment).cache_key == first.fragment_cache_key);
#elif PELICAN_RUNTIME_SHADER_COMPILER
    ScopedSpvLinkEnvironment environment{"experimental"};
    const auto surface = parseSurfaceFormat(readText(fixtureRoot() / "valid" / "wp78.surface"),
                                            "wp78.surface");
    ShaderCompiler compiler;
    const auto result = compileSurfaceShaders(compiler, surface, "wp78.surface");
    REQUIRE(result.experimental_spv_link);
    REQUIRE_FALSE(result.vertex.ok);
    REQUIRE(result.vertex.log.find("PELICAN_WITH_SPIRV_LINK=OFF") != std::string::npos);
#endif
}

TEST_CASE("surface source composition compiles routed main depth and velocity variants",
          "[surface-compiler]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto source = readText(fixtureRoot() / "valid" / "wp78.surface");
    const auto surface = parseSurfaceFormat(source, "wp78.surface");
    ShaderCompiler compiler;
    requireCompiled(compileSurfaceShaders(compiler, surface, "wp78.surface", SurfacePass::main));
    requireCompiled(compileSurfaceShaders(compiler, surface, "wp78.surface",
                                          SurfacePass::deferred_geometry));
    requireCompiled(compileSurfaceShaders(compiler, surface, "wp78.surface",
                                          SurfacePass::forward));
    requireCompiled(compileSurfaceShaders(compiler, surface, "wp78.surface", SurfacePass::depth));
    requireCompiled(compileSurfaceShaders(compiler, surface, "wp78.surface", SurfacePass::velocity));
    requireCompiled(compileSurfaceShaders(compiler, surface, "wp78.surface", SurfacePass::depth,
                                          {"PELICAN_FEATURE_SHADOW"}));

    const auto depth = composeSurfaceShaders(surface, "wp78.surface", SurfacePass::depth);
    REQUIRE(std::find(depth.defines.begin(), depth.defines.end(), "PELICAN_PASS_DEPTH") !=
            depth.defines.end());
    REQUIRE(std::find(depth.defines.begin(), depth.defines.end(),
                      "PELICAN_HAS_VERTEX_DISPLACE_V1") != depth.defines.end());
    const auto deferred = composeSurfaceShaders(surface, "wp78.surface",
                                                SurfacePass::deferred_geometry);
    REQUIRE(std::find(deferred.defines.begin(), deferred.defines.end(),
                      "PELICAN_PASS_DEFERRED_GEOMETRY") != deferred.defines.end());
    const auto forward = composeSurfaceShaders(surface, "wp78.surface", SurfacePass::forward);
    REQUIRE(std::find(forward.defines.begin(), forward.defines.end(),
                      "PELICAN_PASS_FORWARD") != forward.defines.end());
    REQUIRE(surfacePassForMaterialRoute(MaterialRouteClass::deferred_geometry) ==
            SurfacePass::deferred_geometry);
    REQUIRE(surfacePassForMaterialRoute(MaterialRouteClass::forward_transparent) ==
            SurfacePass::forward);
    const auto shadow_depth = composeSurfaceShaders(surface, "wp78.surface", SurfacePass::depth,
                                                    {"PELICAN_FEATURE_SHADOW"});
    REQUIRE(std::find(shadow_depth.defines.begin(), shadow_depth.defines.end(),
                      "PELICAN_FEATURE_SHADOW") != shadow_depth.defines.end());
#endif
}

TEST_CASE(
    "surface compiler emits and reflects an arbitrary material output schema",
    "[surface-compiler][material-output][wp218]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ScopedSpvLinkEnvironment environment{nullptr};
    const auto surface = parseSurfaceFormat(
        R"surface(//! pelican.surface v1
//! language: glsl

void pelican_surface_v1(
    in PelicanSurfaceInputV1 input_data,
    inout PelicanSurfaceV1 surface) {
    surface.roughness = 0.25;
}

void pelican_material_outputs_v1(
    in PelicanSurfaceInputV1 input_data,
    in PelicanSurfaceV1 surface,
    inout PelicanMaterialOutputsV1 outputs) {
    outputs.object_id = 73u;
    outputs.reactive_mask = surface.base_color.a;
}
)surface",
        "extended_gbuffer.surface");
    const MaterialOutputSchema schema{
        .name = "project.extended_gbuffer",
        .outputs = {
            {"base_color", MaterialOutputType::vec4,
             MaterialOutputSource::surface_base_color},
            {"normal", MaterialOutputType::vec4,
             MaterialOutputSource::
                 surface_normal_encoded},
            {"material", MaterialOutputType::vec4,
             MaterialOutputSource::surface_material},
            {"world_position", MaterialOutputType::vec4,
             MaterialOutputSource::input_world_position},
            {"emissive", MaterialOutputType::vec4,
             MaterialOutputSource::surface_emissive},
            {"object_id",
             MaterialOutputType::unsigned_integer,
             MaterialOutputSource::custom},
            {"reactive_mask",
             MaterialOutputType::floating,
             MaterialOutputSource::custom},
        },
    };

    ShaderCompiler compiler;
    const auto result = compileSurfaceShaders(
        compiler, surface, "extended_gbuffer.surface",
        SurfacePass::deferred_geometry, {}, schema);
    requireCompiled(result);
    const auto reflection =
        reflect(result.fragment.spirv);
    REQUIRE(reflection.fragment_outputs.size() == 7);
    REQUIRE(reflection.fragment_outputs[5].format ==
            vk::Format::eR32Uint);
    REQUIRE(reflection.fragment_outputs[6].format ==
            vk::Format::eR32Sfloat);
    REQUIRE_NOTHROW(validateFragmentOutputSchema(
        reflection, schema, "extended G-buffer"));

    const auto composition = composeSurfaceShaders(
        surface, "extended_gbuffer.surface",
        SurfacePass::deferred_geometry, {}, schema);
    REQUIRE(
        std::find(
            composition.defines.begin(),
            composition.defines.end(),
            "PELICAN_CUSTOM_MATERIAL_OUTPUTS_V1") !=
        composition.defines.end());
    REQUIRE(
        std::find(
            composition.defines.begin(),
            composition.defines.end(),
            "PELICAN_HAS_MATERIAL_OUTPUTS_V1") !=
        composition.defines.end());
#endif
}

TEST_CASE("surface resource ports generate vertex buffer and fragment image accessors",
          "[surface-compiler][resource-port][wp207b]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    constexpr std::string_view source = R"surface(//! pelican.surface v1
//! language: glsl
//! resource_ports:
//!   - { name: displacement, kind: buffer, element: vec4, stage: vertex }
//!   - { name: simulation_color, kind: image, stage: fragment }

void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) {
    vertex.position += pelican_load_displacement(0u).xyz;
}

void pelican_surface_v1(in PelicanSurfaceInputV1 input_data,
                        inout PelicanSurfaceV1 surface) {
    surface.base_color *= pelican_sample_simulation_color(input_data.uv);
}
)surface";
    const auto surface =
        parseSurfaceFormat(source, "typed_material.surface");
    const auto composition = composeSurfaceShaders(
        surface, "typed_material.surface",
        SurfacePass::deferred_geometry);
    REQUIRE(composition.resource_interface.size() == 2);
    REQUIRE(composition.resource_interface[0].binding == 0);
    REQUIRE(composition.resource_interface[0].descriptor ==
            ShaderResourceDescriptorKind::storage_buffer);
    REQUIRE(composition.resource_interface[0].buffer_element ==
            ShaderResourceBufferElement::vec4);
    REQUIRE(composition.resource_interface[0].expected_stages ==
            vk::ShaderStageFlagBits::eVertex);
    REQUIRE(composition.resource_interface[1].binding == 1);
    REQUIRE(composition.resource_interface[1].descriptor ==
            ShaderResourceDescriptorKind::combined_image_sampler);
    REQUIRE(composition.resource_interface[1].expected_stages ==
            vk::ShaderStageFlagBits::eFragment);

    ShaderCompiler compiler;
    const auto compiled = compileSurfaceShaders(
        compiler, surface, "typed_material.surface",
        SurfacePass::deferred_geometry);
    requireCompiled(compiled);
    const auto reflection = merge(std::array{
        reflect(compiled.vertex.spirv),
        reflect(compiled.fragment.spirv),
    });
    REQUIRE_NOTHROW(
        validateShaderResourceInterfaceReflection(
            composition.resource_interface, reflection));
#if PELICAN_WITH_SPIRV_LINK
    {
        ScopedSpvLinkEnvironment environment{
            "experimental"};
        const auto linked = compileSurfaceShaders(
            compiler, surface,
            "typed_material.surface",
            SurfacePass::deferred_geometry);
        requireCompiled(linked);
        REQUIRE(linked.experimental_spv_link);
        const auto linked_reflection = merge(
            std::array{
                reflect(linked.vertex.spirv),
                reflect(linked.fragment.spirv),
            });
        REQUIRE_NOTHROW(
            validateShaderResourceInterfaceReflection(
                composition.resource_interface,
                linked_reflection));
    }
#endif
#endif
}

TEST_CASE("surface texture dimensions compile to matching GLSL and SPIR-V reflection",
          "[surface-compiler][texture][wp209a]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ScopedSpvLinkEnvironment environment{nullptr};
    constexpr std::string_view source = R"surface(//! pelican.surface v1
//! language: glsl
//! textures:
//!   - { name: color_map, default: "project://color.ktx2", color_space: linear, dimension: 2d }
//!   - { name: environment, default: "project://environment.ktx2", color_space: linear, dimension: cube, sampler: { address: clamp_to_edge } }
//!   - { name: layers, default: "project://layers.ktx2", color_space: linear, dimension: 2d_array }
//!   - { name: volume, default: "project://volume.ktx2", color_space: linear, dimension: 3d }

void pelican_surface_v1(in PelicanSurfaceInputV1 input_data,
                        inout PelicanSurfaceV1 surface) {
    surface.base_color *= pelican_sample_color_map(input_data.uv);
    surface.emissive += pelican_sample_environment(vec3(1.0, 0.0, 0.0)).rgb;
    surface.emissive += pelican_sample_layers(vec3(input_data.uv, 0.0)).rgb;
    surface.emissive += pelican_sample_volume(vec3(input_data.uv, 0.5)).rgb;
}
)surface";
    const auto surface = parseSurfaceFormat(
        source, "texture_dimensions.surface");
    const auto composition = composeSurfaceShaders(
        surface, "texture_dimensions.surface",
        SurfacePass::forward);
    const auto params = std::find_if(
        composition.virtual_includes.begin(),
        composition.virtual_includes.end(),
        [](const auto &include) {
            return include.first ==
                   "__pelican_surface_params.glsl";
        });
    REQUIRE(params !=
            composition.virtual_includes.end());
    REQUIRE(params->second.find(
                "uniform sampler2D pelican_texture_color_map") !=
            std::string::npos);
    REQUIRE(params->second.find(
                "uniform samplerCube pelican_texture_environment") !=
            std::string::npos);
    REQUIRE(params->second.find(
                "uniform sampler2DArray pelican_texture_layers") !=
            std::string::npos);
    REQUIRE(params->second.find(
                "uniform sampler3D pelican_texture_volume") !=
            std::string::npos);

    ShaderCompiler compiler;
    const auto compiled = compileSurfaceShaders(
        compiler, surface,
        "texture_dimensions.surface",
        SurfacePass::forward);
    requireCompiled(compiled);
    const auto reflection = merge(std::array{
        reflect(compiled.vertex.spirv),
        reflect(compiled.fragment.spirv),
    });
    const auto require_image =
        [&](std::uint32_t binding,
            ReflectedImageViewDimension dimension) {
            const auto found = std::find_if(
                reflection.bindings.begin(),
                reflection.bindings.end(),
                [&](const auto &reflected) {
                    return reflected.set == 2 &&
                           reflected.binding == binding;
                });
            REQUIRE(found != reflection.bindings.end());
            REQUIRE(found->type ==
                    vk::DescriptorType::
                        eCombinedImageSampler);
            REQUIRE(found->image_view_dimension ==
                    dimension);
        };
    require_image(
        7, ReflectedImageViewDimension::two_d);
    require_image(
        8, ReflectedImageViewDimension::cube);
    require_image(
        9,
        ReflectedImageViewDimension::two_d_array);
    require_image(
        10, ReflectedImageViewDimension::three_d);
#if PELICAN_WITH_SPIRV_LINK
    {
        ScopedSpvLinkEnvironment linked_environment{
            "experimental"};
        const auto linked = compileSurfaceShaders(
            compiler, surface,
            "texture_dimensions.surface",
            SurfacePass::forward);
        requireCompiled(linked);
        REQUIRE(linked.experimental_spv_link);
    }
#endif
#endif
}

TEST_CASE("comparison texture accessors use shadow sampler signatures",
          "[surface-compiler][sampler][wp209a]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ScopedSpvLinkEnvironment environment{nullptr};
    constexpr std::string_view source = R"surface(//! pelican.surface v1
//! language: glsl
//! textures:
//!   - { name: shadow, default: "project://shadow.ktx2", color_space: linear, dimension: 2d_array, sampler: { compare: less_equal, address: clamp_to_edge } }

void pelican_surface_v1(in PelicanSurfaceInputV1 input_data,
                        inout PelicanSurfaceV1 surface) {
    float visible = pelican_sample_shadow(
        vec3(input_data.uv, 0.0), 0.5);
    surface.base_color.rgb *= visible;
}
)surface";
    const auto surface = parseSurfaceFormat(
        source, "comparison_texture.surface");
    const auto composition = composeSurfaceShaders(
        surface, "comparison_texture.surface",
        SurfacePass::forward);
    const auto params = std::find_if(
        composition.virtual_includes.begin(),
        composition.virtual_includes.end(),
        [](const auto &include) {
            return include.first ==
                   "__pelican_surface_params.glsl";
        });
    REQUIRE(params !=
            composition.virtual_includes.end());
    REQUIRE(params->second.find(
                "uniform sampler2DArrayShadow "
                "pelican_texture_shadow") !=
            std::string::npos);
    REQUIRE(params->second.find(
                "float pelican_sample_shadow("
                "vec3 coordinates, float reference)") !=
            std::string::npos);

    ShaderCompiler compiler;
    requireCompiled(compileSurfaceShaders(
        compiler, surface,
        "comparison_texture.surface",
        SurfacePass::forward));
#if PELICAN_WITH_SPIRV_LINK
    {
        ScopedSpvLinkEnvironment linked_environment{
            "experimental"};
        const auto linked = compileSurfaceShaders(
            compiler, surface,
            "comparison_texture.surface",
            SurfacePass::forward);
        requireCompiled(linked);
        REQUIRE(linked.experimental_spv_link);
    }
#endif
#endif
}

TEST_CASE(
    "clustered lighting adds typed buffers only to forward surface variants",
    "[surface-compiler][resource-port][clustered][wp208]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto surface =
        parseSurfaceFormat(
            readText(
                fixtureRoot() / "valid" /
                "wp78.surface"),
            "wp78.surface");
    const std::vector<std::string> defines{
        "PELICAN_FEATURE_CLUSTERED_LIGHTING"};
    const auto plain =
        composeSurfaceShaders(
            surface, "wp78.surface",
            SurfacePass::forward);
    const auto clustered =
        composeSurfaceShaders(
            surface, "wp78.surface",
            SurfacePass::forward, defines);
    const auto deferred =
        composeSurfaceShaders(
            surface, "wp78.surface",
            SurfacePass::deferred_geometry,
            defines);
    const std::vector<std::string>
        clustered_shadow_defines{
            "PELICAN_FEATURE_CLUSTERED_LIGHTING",
            "PELICAN_FEATURE_SHADOW",
        };
    const auto clustered_shadow =
        composeSurfaceShaders(
            surface, "wp78.surface",
            SurfacePass::forward,
            clustered_shadow_defines);

    REQUIRE(
        clustered.resource_interface.size() ==
        plain.resource_interface.size() + 2);
    REQUIRE(
        deferred.resource_interface.size() ==
        plain.resource_interface.size());
    REQUIRE(std::none_of(
        deferred.defines.begin(),
        deferred.defines.end(),
        [](const auto &define) {
            return define ==
                   "PELICAN_FEATURE_CLUSTERED_LIGHTING";
        }));
    const auto inventory =
        std::find_if(
            clustered.resource_interface.begin(),
            clustered.resource_interface.end(),
            [](const auto &binding) {
                return binding.port.name ==
                       "light_inventory";
            });
    const auto selection =
        std::find_if(
            clustered.resource_interface.begin(),
            clustered.resource_interface.end(),
            [](const auto &binding) {
                return binding.port.name ==
                       "light_selection";
            });
    REQUIRE(
        inventory !=
        clustered.resource_interface.end());
    REQUIRE(
        selection !=
        clustered.resource_interface.end());
    REQUIRE(
        inventory->descriptor ==
        ShaderResourceDescriptorKind::
            storage_buffer);
    REQUIRE(
        inventory->buffer_element ==
        ShaderResourceBufferElement::uvec4);
    REQUIRE(
        selection->buffer_element ==
        ShaderResourceBufferElement::
            unsigned_integer);
    REQUIRE(
        inventory->expected_stages ==
        vk::ShaderStageFlagBits::eFragment);
    REQUIRE(
        selection->expected_stages ==
        vk::ShaderStageFlagBits::eFragment);
    REQUIRE(
        clustered_shadow.resource_interface
            .at(0)
            .binding == 1);
    REQUIRE(
        clustered_shadow.resource_interface
            .at(1)
            .binding == 2);

    ShaderCompiler compiler;
    requireCompiled(
        compileSurfaceShaders(
            compiler, surface,
            "wp78.surface",
            SurfacePass::deferred_geometry,
            defines));
    const auto compiled =
        compileSurfaceShaders(
            compiler, surface,
            "wp78.surface",
            SurfacePass::forward, defines);
    requireCompiled(compiled);
    const auto reflection =
        merge(std::array{
            reflect(compiled.vertex.spirv),
            reflect(compiled.fragment.spirv),
        });
    REQUIRE_NOTHROW(
        validateShaderResourceInterfaceReflection(
            clustered.resource_interface,
            reflection));
    ShaderLibrary library{
        ShaderLibraryModuleMode::
            reflection_only};
    const auto bundles =
        library.loadFromSurface(
            surface, "wp78.surface",
            SurfacePass::forward, defines);
    const auto &compiler_ports =
        library.get(bundles.fragment)
            .compiler_resource_interface;
    REQUIRE(
        compiler_ports.size() == 2);
    REQUIRE(
        std::any_of(
            compiler_ports.begin(),
            compiler_ports.end(),
            [](const auto &binding) {
                return binding.port.name ==
                       "light_selection";
            }));
    const auto combined_compiled =
        compileSurfaceShaders(
            compiler, surface,
            "wp78.surface",
            SurfacePass::forward,
            clustered_shadow_defines);
    requireCompiled(combined_compiled);
    const auto combined_reflection =
        merge(std::array{
            reflect(
                combined_compiled.vertex.spirv),
            reflect(
                combined_compiled.fragment.spirv),
        });
    REQUIRE_NOTHROW(
        validateShaderResourceInterfaceReflection(
            clustered_shadow
                .resource_interface,
            combined_reflection));
    REQUIRE(std::any_of(
        combined_reflection.bindings.begin(),
        combined_reflection.bindings.end(),
        [](const auto &binding) {
            return binding.set == 1 &&
                   binding.binding == 0 &&
                   binding.name ==
                       directionalShadowSamplerName;
        }));
#endif
}

TEST_CASE(
    "clustered light selector compiles against the generated buffer interface",
    "[surface-compiler][compute][clustered][wp208]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const std::vector<
        ShaderResourceInterfaceBinding>
        interface{
            ShaderResourceInterfaceBinding{
                .port =
                    ShaderResourcePortDefinition{
                        .name = "light_inventory",
                        .resource =
                            "clustered_light_inventory",
                        .kind =
                            ShaderResourcePortKind::buffer,
                        .buffer_element =
                            ShaderResourceBufferElement::
                                uvec4,
                        .access =
                            ShaderResourcePortAccess::
                                storage,
                    },
                .binding = 0,
                .descriptor =
                    ShaderResourceDescriptorKind::
                        storage_buffer,
                .image_view_dimension =
                    ReflectedImageViewDimension::none,
                .buffer_element =
                    ShaderResourceBufferElement::
                        uvec4,
                .expected_stages =
                    vk::ShaderStageFlagBits::eCompute,
                .readable = true,
                .writable = false,
            },
            ShaderResourceInterfaceBinding{
                .port =
                    ShaderResourcePortDefinition{
                        .name = "light_selection",
                        .resource =
                            "clustered_light_selection",
                        .kind =
                            ShaderResourcePortKind::buffer,
                        .buffer_element =
                            ShaderResourceBufferElement::
                                unsigned_integer,
                        .access =
                            ShaderResourcePortAccess::
                                storage,
                    },
                .binding = 1,
                .descriptor =
                    ShaderResourceDescriptorKind::
                        storage_buffer,
                .image_view_dimension =
                    ReflectedImageViewDimension::none,
                .buffer_element =
                    ShaderResourceBufferElement::
                        unsigned_integer,
                .expected_stages =
                    vk::ShaderStageFlagBits::eCompute,
                .readable = true,
                .writable = true,
            },
        };
    ShaderCompileOptions options;
    options.virtual_includes =
        makeShaderResourcePortVirtualIncludes(
            interface);
    ShaderCompiler compiler;
    const auto compiled =
        compiler.compileSource(
            engineResourceOrThrow(
                "shaders/compute/"
                "clustered_light_select.comp"),
            vk::ShaderStageFlagBits::eCompute,
            "engine://shaders/compute/"
            "clustered_light_select.comp",
            options);
    INFO("compute log: " << compiled.log);
    REQUIRE(compiled.ok);
    REQUIRE_FALSE(compiled.spirv.empty());
    REQUIRE_NOTHROW(
        validateShaderResourceInterfaceReflection(
            interface,
            reflect(compiled.spirv)));
#endif
}

TEST_CASE("directional shadow extends only the forward pass-input ABI and leaves feature-off stable",
          "[surface-compiler][shadow][wp205]") {
    const auto source =
        readText(fixtureRoot() / "valid" / "wp78.surface");
    const auto surface =
        parseSurfaceFormat(source, "wp78.surface");
    const auto feature_define =
        std::string{"PELICAN_FEATURE_SHADOW"};
    const auto binding_define =
        std::string{"PELICAN_DIRECTIONAL_SHADOW_BINDING=0"};

    const auto plain = composeSurfaceShaders(
        surface, "wp78.surface", SurfacePass::forward);
    const auto shadow = composeSurfaceShaders(
        surface, "wp78.surface", SurfacePass::forward,
        {feature_define});
    const auto deferred_shadow = composeSurfaceShaders(
        surface, "wp78.surface",
        SurfacePass::deferred_geometry, {feature_define});
    const auto depth_shadow = composeSurfaceShaders(
        surface, "wp78.surface", SurfacePass::depth,
        {feature_define});

    REQUIRE(std::find(
                plain.defines.begin(), plain.defines.end(),
                binding_define) == plain.defines.end());
    REQUIRE(std::find(
                shadow.defines.begin(), shadow.defines.end(),
                binding_define) != shadow.defines.end());
    REQUIRE(std::find(
                deferred_shadow.defines.begin(),
                deferred_shadow.defines.end(),
                binding_define) == deferred_shadow.defines.end());
    REQUIRE(std::find(
                depth_shadow.defines.begin(),
                depth_shadow.defines.end(),
                binding_define) == depth_shadow.defines.end());
    REQUIRE(plain.vertex_source == shadow.vertex_source);
    REQUIRE(plain.fragment_source == shadow.fragment_source);
    REQUIRE(plain.virtual_includes == shadow.virtual_includes);

    const auto refract_path =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "projects" / "example" / "shaders" /
        "refract.surface";
    const auto refract = parseSurfaceFormat(
        readText(refract_path),
        "project://shaders/refract.surface");
    const auto refract_shadow = composeSurfaceShaders(
        refract, "project://shaders/refract.surface",
        SurfacePass::forward, {feature_define});
    REQUIRE(std::find(
                refract_shadow.defines.begin(),
                refract_shadow.defines.end(),
                "PELICAN_DIRECTIONAL_SHADOW_BINDING=1") !=
            refract_shadow.defines.end());

#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    const auto feature_off_before = compileSurfaceShaders(
        compiler, surface, "wp78.surface",
        SurfacePass::forward);
    requireCompiled(feature_off_before);
    const auto feature_on = compileSurfaceShaders(
        compiler, surface, "wp78.surface",
        SurfacePass::forward, {feature_define});
    requireCompiled(feature_on);
    const auto feature_off_after = compileSurfaceShaders(
        compiler, surface, "wp78.surface",
        SurfacePass::forward);
    requireCompiled(feature_off_after);
    REQUIRE(feature_off_before.vertex.spirv ==
            feature_off_after.vertex.spirv);
    REQUIRE(feature_off_before.fragment.spirv ==
            feature_off_after.fragment.spirv);

    ShaderLibrary library{
        ShaderLibraryModuleMode::reflection_only};
    const auto plain_bundles = library.loadFromSurface(
        surface, "wp78.surface", SurfacePass::forward);
    const auto shadow_bundles = library.loadFromSurface(
        surface, "wp78.surface", SurfacePass::forward,
        {feature_define});
    const auto refract_bundles = library.loadFromSurface(
        refract, "project://shaders/refract.surface",
        SurfacePass::forward, {feature_define});

    const auto find_set_one =
        [](const ShaderReflection &reflection,
           std::uint32_t binding,
           std::string_view name) {
            return std::find_if(
                reflection.bindings.begin(),
                reflection.bindings.end(),
                [binding, name](const auto &item) {
                    return item.set == 1 &&
                           item.binding == binding &&
                           item.type ==
                               vk::DescriptorType::
                                   eCombinedImageSampler &&
                           item.name == name;
                });
        };
    const auto &plain_reflection =
        library.get(plain_bundles.fragment).reflection;
    REQUIRE(std::none_of(
        plain_reflection.bindings.begin(),
        plain_reflection.bindings.end(),
        [](const auto &item) {
            return item.set == 1 &&
                   item.name ==
                       directionalShadowSamplerName;
        }));

    const auto &shadow_reflection =
        library.get(shadow_bundles.fragment).reflection;
    REQUIRE(find_set_one(
                shadow_reflection, 0,
                directionalShadowSamplerName) !=
            shadow_reflection.bindings.end());

    const auto &refract_reflection =
        library.get(refract_bundles.fragment).reflection;
    REQUIRE(find_set_one(
                refract_reflection, 0,
                "pelican_screen_opaque_color_texture") !=
            refract_reflection.bindings.end());
    REQUIRE(find_set_one(
                refract_reflection, 1,
                directionalShadowSamplerName) !=
            refract_reflection.bindings.end());
#endif
}

TEST_CASE("standard and toon lighting dogfood only the public surface library",
          "[surface-compiler][dogfood]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    for (const auto resource : {"shaders/material/standard_lighting.glsl",
                                "shaders/material/toon_lighting.glsl"}) {
        const auto surface = engineLightingSurface(resource);
        REQUIRE(surface.hooks.lighting_v1);
        requireCompiled(compileSurfaceShaders(compiler, surface,
                                              std::string{"engine://"} + resource));
    }
#endif
}

TEST_CASE("OpenPBR registers and compiles all six routed cache variants",
          "[surface-compiler][openpbr][variants]") {
    const auto manifest = nlohmann::json::parse(
        engineResourceOrThrow("surfaces/openpbr/manifest.json"));
    REQUIRE(manifest.at("openpbr").at("version") == "1.1.1");
    REQUIRE(manifest.at("openpbr").at("commit") ==
            "f8d6d947dfae4c9b599965a86c22826ea7a8dbfb");
    REQUIRE(manifest.at("lighting") ==
            "engine://shaders/material/openpbr_lighting.glsl");
    REQUIRE(manifest.at("variants").size() == 6);

    const auto lighting = engineResourceOrThrow("shaders/material/openpbr_lighting.glsl");
    REQUIRE(lighting.find("weighted_base_color = surface.base_color.rgb") !=
            std::string_view::npos);
    const auto surface_template =
        engineResourceOrThrow("shaders/material/surface_v1.frag");
    REQUIRE(surface_template.find("pelican_material_instance_uv") !=
            std::string_view::npos);
    REQUIRE(surface_template.find("pelican_material_instance_apply_base_color") !=
            std::string_view::npos);
    for (const auto public_call : {"pelican_light_count()", "pelican_light(",
                                   "pelican_shadow(", "pelican_env_ambient("}) {
        REQUIRE(lighting.find(public_call) != std::string_view::npos);
    }
    for (const auto private_symbol : {"pelicanLights", "pelicanFrame", "PELICAN_SET_",
                                      "layout(set", "pelican_sets.glsl",
                                      "pelican_frame.glsl"}) {
        REQUIRE(lighting.find(private_symbol) == std::string_view::npos);
    }

    std::vector<std::string> names;
    std::vector<std::string> resources;
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
#endif
    for (const auto &entry : manifest.at("variants")) {
        const auto name = entry.at("name").get<std::string>();
        const auto reference = entry.at("surface").get<std::string>();
        const auto resource = reference.substr(std::string{"engine://"}.size());
        names.push_back(name);
        resources.push_back(reference);
        const auto surface = parseSurfaceFormat(engineResourceOrThrow(resource), reference);
        REQUIRE(surface.hooks.surface_v1);
        REQUIRE(surface.hooks.lighting_v1);
        MaterialDefinition material;
        material.name = name;
        material.surface = reference;
        material.routing = MaterialVariantRouting{
            entry.at("alpha_mode") == "opaque" ? MaterialAlphaMode::opaque
                : entry.at("alpha_mode") == "mask" ? MaterialAlphaMode::mask
                                                    : MaterialAlphaMode::blend,
            entry.at("double_sided").get<bool>(),
        };
        const auto lowered = lowerMaterial(material, surface);
        REQUIRE(lowered.target_pass ==
                (material.routing->alpha_mode == MaterialAlphaMode::blend
                     ? "forward_transparent"
                     : "deferred_geometry"));
#if PELICAN_RUNTIME_SHADER_COMPILER
        const auto pass = surfacePassForMaterialRoute(lowered.route);
        const auto composition = composeSurfaceShaders(surface, reference, pass,
                                                       lowered.defines);
        REQUIRE(std::find(composition.defines.begin(), composition.defines.end(),
                          pass == SurfacePass::deferred_geometry
                              ? "PELICAN_PASS_DEFERRED_GEOMETRY"
                              : "PELICAN_PASS_FORWARD") !=
                composition.defines.end());
        requireCompiled(compileSurfaceShaders(compiler, surface, reference, pass,
                                              lowered.defines));
        if (name == "opaque_single_sided") {
            ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
            const auto bundles = library.loadFromSurfaceForMaterial(
                surface, reference, lowered, {"PELICAN_TEST_ROUTED"});
            const auto &defines = library.get(bundles.fragment).defines;
            REQUIRE(std::find(defines.begin(), defines.end(),
                              "PELICAN_GBUFFER_MODEL_OPENPBR_BASE_V1") !=
                    defines.end());
            REQUIRE(std::find(defines.begin(), defines.end(),
                              "PELICAN_PASS_DEFERRED_GEOMETRY") !=
                    defines.end());
            REQUIRE(std::find(defines.begin(), defines.end(),
                              "PELICAN_TEST_ROUTED") != defines.end());
        }
#endif
    }
    REQUIRE(names == std::vector<std::string>{
                         "opaque_single_sided", "opaque_double_sided",
                         "mask_single_sided", "mask_double_sided",
                         "blend_single_sided", "blend_double_sided"});
    REQUIRE(std::set<std::string>{resources.begin(), resources.end()}.size() == 6);

    const auto example_root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                              "projects" / "example";
    const auto example_reference =
        std::string{"engine://surfaces/openpbr/opaque_double.surface"};
    const auto example_surface = parseSurfaceFormat(
        engineResourceOrThrow("surfaces/openpbr/opaque_double.surface"), example_reference);
    MaterialSurfaceCatalog catalog;
    catalog.emplace(example_reference, example_surface);
    const auto material_document = parseMaterialFormatJson(
        nlohmann::json::parse(readText(example_root / "materials" /
                                      "openpbr_coat.material.json")),
        catalog);
    REQUIRE(material_document.warnings.empty());
    REQUIRE(material_document.materials.size() == 1);
    const auto example_lowered = lowerMaterial(material_document.materials.front(),
                                               example_surface);
    REQUIRE(example_lowered.routing->alpha_mode == MaterialAlphaMode::opaque);
    REQUIRE(example_lowered.routing->double_sided);
    REQUIRE(example_lowered.target_pass == "forward_opaque");
    REQUIRE(example_lowered.defines == std::vector<std::string>{
                                           "OPENPBR_PIN_V1_1_1",
                                           "OPENPBR_PIN_F8D6D947DFAE4C9B599965A86C22826EA7A8DBFB"});
}

TEST_CASE("surface compile diagnostics report the authored snippet line",
          "[surface-compiler][diagnostics]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto path = fixtureRoot() / "invalid" / "compile_line.surface";
    const auto surface = parseSurfaceFormat(readText(path), "compile_line.surface");
    ShaderCompiler compiler;
    const auto result = compileSurfaceShaders(compiler, surface, "compile_line.surface");
    REQUIRE_FALSE(result.fragment.ok);
    REQUIRE_THAT(result.fragment.log,
                 Catch::Matchers::ContainsSubstring("compile_line.surface:6"));
    REQUIRE_THAT(result.fragment.log,
                 Catch::Matchers::ContainsSubstring("not_a_declared_symbol"));
#endif
}

TEST_CASE("M3a source composition rejects non-GLSL surfaces by name", "[surface-compiler]") {
    const auto source = readText(fixtureRoot() / "valid" / "minimal_hlsl.surface");
    const auto surface = parseSurfaceFormat(source, "minimal_hlsl.surface");
    REQUIRE_THROWS_WITH(composeSurfaceShaders(surface, "minimal_hlsl.surface"),
                        Catch::Matchers::ContainsSubstring("minimal_hlsl.surface") &&
                            Catch::Matchers::ContainsSubstring("GLSL only"));
}

TEST_CASE("example material is a one-line B surface reference that lowers and compiles",
          "[surface-compiler][example]") {
    const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "example";
    const auto surface_source = readText(root / "shaders" / "toon.surface");
    const auto surface = parseSurfaceFormat(surface_source,
                                            "project://shaders/toon.surface");
    MaterialSurfaceCatalog catalog;
    catalog.emplace("project://shaders/toon.surface", surface);
    const auto material_json = nlohmann::json::parse(
        readText(root / "materials" / "toon.material.json"));
    const auto document = parseMaterialFormatJson(material_json, catalog);
    REQUIRE(document.materials.size() == 1);
    REQUIRE(document.materials.front().surface == "project://shaders/toon.surface");
    const auto lowered = lowerMaterial(document.materials.front(), surface);
    REQUIRE(lowered.hooks.surface_v1);
    REQUIRE(lowered.hooks.lighting_v1);
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    requireCompiled(compileSurfaceShaders(compiler, surface,
                                          "project://shaders/toon.surface"));
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto bundles = library.loadFromSurface(surface, "project://shaders/toon.surface");
    REQUIRE_FALSE(library.get(bundles.vertex).reflection.vertex_inputs.empty());
    REQUIRE_FALSE(library.get(bundles.fragment).reflection.bindings.empty());
    REQUIRE(std::find(library.get(bundles.fragment).defines.begin(),
                      library.get(bundles.fragment).defines.end(),
                      "PELICAN_HAS_LIGHTING_V1") !=
            library.get(bundles.fragment).defines.end());
#endif
}

TEST_CASE("example refraction surface exposes its named screen snapshot accessor",
          "[surface-compiler][snapshot][example]") {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "example" /
                      "shaders" / "refract.surface";
    const auto surface = parseSurfaceFormat(readText(path), "project://shaders/refract.surface");
    REQUIRE(surface.screen_inputs == std::vector<std::string>{"opaque_color"});
    const auto composition = composeSurfaceShaders(surface, "project://shaders/refract.surface");
    const auto params = std::find_if(composition.virtual_includes.begin(),
                                     composition.virtual_includes.end(), [](const auto &include) {
        return include.first == "__pelican_surface_params.glsl";
    });
    REQUIRE(params != composition.virtual_includes.end());
    REQUIRE(params->second.find("layout(set = PELICAN_SET_PASS_INPUT, binding = 0)") !=
            std::string::npos);
    REQUIRE(params->second.find("pelican_screen_opaque_color") != std::string::npos);
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    requireCompiled(compileSurfaceShaders(compiler, surface,
                                          "project://shaders/refract.surface"));
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto bundles = library.loadFromSurface(surface, "project://shaders/refract.surface");
    const auto &bindings = library.get(bundles.fragment).reflection.bindings;
    REQUIRE(std::any_of(bindings.begin(), bindings.end(), [](const auto &binding) {
        return binding.set == 1 && binding.binding == 0 &&
               binding.type == vk::DescriptorType::eCombinedImageSampler;
    }));
#endif
}

TEST_CASE("example depth fade lowers same-pixel depth and emits linearization accessor",
          "[surface-compiler][screen-input][depth-fade][rpe6b1]") {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                      "projects" / "example" / "shaders" /
                      "depth_fade.surface";
    const auto surface = parseSurfaceFormat(
        readText(path), "project://shaders/depth_fade.surface");
    REQUIRE(surface.screen_inputs ==
            std::vector<std::string>{"linear_view_depth"});
    const auto lowered = lowerSurfaceDefaults(
        surface, "project://shaders/depth_fade.surface");
    REQUIRE(lowered.route == MaterialRouteClass::forward_transparent);
    REQUIRE(lowered.screen_input_contracts.size() == 1);
    REQUIRE(lowered.screen_input_contracts.front().footprint.kind ==
            LogicalReadFootprintKind::same_pixel);
    REQUIRE(lowered.screen_input_contracts.front().conversion ==
            std::optional<std::string>{
                "pelican.render.depth_linearize@1"});

    const auto composition = composeSurfaceShaders(
        surface, "project://shaders/depth_fade.surface");
    const auto params = std::find_if(
        composition.virtual_includes.begin(),
        composition.virtual_includes.end(), [](const auto &include) {
            return include.first == "__pelican_surface_params.glsl";
        });
    REQUIRE(params != composition.virtual_includes.end());
    REQUIRE(params->second.find("inverse(pelicanFrame.projection)") !=
            std::string::npos);
    REQUIRE(params->second.find("-view_position.z / view_position.w") !=
            std::string::npos);
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    requireCompiled(compileSurfaceShaders(
        compiler, surface, "project://shaders/depth_fade.surface"));
#endif
}

TEST_CASE(
    "surface physical variant lowers screen and custom image inputs to input attachments",
    "[surface-compiler][material-local-read][wp220]") {
    constexpr std::string_view source_name =
        "project://shaders/local_decal.surface";
    const auto surface = parseSurfaceFormat(
        R"surface(//! pelican.surface v1
//! language: glsl
//! screen_inputs: [scene_depth]
//! resource_ports:
//!   - { name: gbuffer_normal, kind: image, stage: fragment }

void pelican_surface_v1(
    in PelicanSurfaceInputV1 input_data,
    inout PelicanSurfaceV1 surface) {
    vec4 normal_data =
        pelican_sample_gbuffer_normal(vec2(0.0));
    vec4 depth_data =
        pelican_screen_scene_depth(vec2(0.0));
    surface.emissive =
        normal_data.rgb + depth_data.rrr;
}
)surface",
        source_name);
    const std::vector<std::string> local_defines{
        makeSurfaceScreenInputLocalReadDefine(0, 3),
        makeSurfaceResourceLocalReadDefine(0, 5),
    };
    const auto local = composeSurfaceShaders(
        surface, source_name,
        SurfacePass::forward, local_defines);
    REQUIRE(local.resource_interface.size() == 1);
    REQUIRE(
        local.resource_interface.front().binding ==
        1u);
    REQUIRE(
        local.resource_interface.front().descriptor ==
        ShaderResourceDescriptorKind::
            input_attachment);
    REQUIRE(
        local.resource_interface.front()
            .input_attachment_index ==
        5u);
    const auto params = std::find_if(
        local.virtual_includes.begin(),
        local.virtual_includes.end(),
        [](const auto &include) {
            return include.first ==
                   "__pelican_surface_params.glsl";
        });
    REQUIRE(params !=
            local.virtual_includes.end());
    REQUIRE(
        params->second.find(
            "input_attachment_index = 3") !=
        std::string::npos);
    REQUIRE(
        params->second.find(
            "input_attachment_index = 5") !=
        std::string::npos);
    REQUIRE(
        params->second.find("subpassLoad") !=
        std::string::npos);

    const auto sampled = composeSurfaceShaders(
        surface, source_name,
        SurfacePass::forward);
    REQUIRE(
        sampled.resource_interface.front().descriptor ==
        ShaderResourceDescriptorKind::
            combined_image_sampler);

    const std::vector<std::string> layered_defines{
        makeSurfaceResourceLayeredDefine(0),
    };
    const auto layered = composeSurfaceShaders(
        surface, source_name,
        SurfacePass::forward, layered_defines);
    REQUIRE(
        layered.resource_interface.front()
            .image_view_dimension ==
        ReflectedImageViewDimension::
            two_d_array);
    const auto layered_params = std::find_if(
        layered.virtual_includes.begin(),
        layered.virtual_includes.end(),
        [](const auto &include) {
            return include.first ==
                   "__pelican_surface_params.glsl";
        });
    REQUIRE(
        layered_params !=
        layered.virtual_includes.end());
    REQUIRE(
        layered_params->second.find(
            "uniform sampler2DArray "
            "pelican_resource_gbuffer_normal") !=
        std::string::npos);
    REQUIRE(
        layered_params->second.find(
            "pelican_sample_gbuffer_normal"
            "(uv, pelican_view_index())") !=
        std::string::npos);
    const auto cube = composeSurfaceShaders(
        surface, source_name,
        SurfacePass::forward,
        {makeSurfaceResourceCubeDefine(0)});
    REQUIRE(
        cube.resource_interface.front()
            .image_view_dimension ==
        ReflectedImageViewDimension::cube);
    const auto cube_params = std::find_if(
        cube.virtual_includes.begin(),
        cube.virtual_includes.end(),
        [](const auto &include) {
            return include.first ==
                   "__pelican_surface_params.glsl";
        });
    REQUIRE(
        cube_params !=
        cube.virtual_includes.end());
    REQUIRE(
        cube_params->second.find(
            "uniform samplerCube "
            "pelican_resource_gbuffer_normal") !=
        std::string::npos);
    REQUIRE_THROWS_WITH(
        composeSurfaceShaders(
            surface, source_name,
            SurfacePass::forward,
            {
                makeSurfaceResourceLocalReadDefine(
                    0, 5),
                makeSurfaceResourceLayeredDefine(0),
            }),
        Catch::Matchers::ContainsSubstring(
            "cannot select input-attachment and layered sampled "
            "ABIs simultaneously"));
    REQUIRE_THROWS_WITH(
        composeSurfaceShaders(
            surface, source_name,
            SurfacePass::forward,
            {
                makeSurfaceResourceLocalReadDefine(
                    0, 5),
                makeSurfaceResourceCubeDefine(0),
            }),
        Catch::Matchers::ContainsSubstring(
            "cannot select input-attachment and cube sampled "
            "ABIs simultaneously"));
    REQUIRE_THROWS_WITH(
        composeSurfaceShaders(
            surface, source_name,
            SurfacePass::forward,
            {
                makeSurfaceResourceLayeredDefine(0),
                makeSurfaceResourceCubeDefine(0),
            }),
        Catch::Matchers::ContainsSubstring(
            "cannot select layered-array and cube sampled "
            "ABIs simultaneously"));

#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    const auto compiled = compileSurfaceShaders(
        compiler, surface, source_name,
        SurfacePass::forward, local_defines);
    requireCompiled(compiled);
    const auto fragment =
        reflect(compiled.fragment.spirv);
    const auto require_input_attachment =
        [&](std::uint32_t binding,
            std::uint32_t input_attachment_index) {
            const auto found = std::find_if(
                fragment.bindings.begin(),
                fragment.bindings.end(),
                [&](const auto &candidate) {
                    return candidate.set == 1 &&
                           candidate.binding ==
                               binding;
                });
            REQUIRE(found !=
                    fragment.bindings.end());
            REQUIRE(
                found->type ==
                vk::DescriptorType::
                    eInputAttachment);
            REQUIRE(
                found->input_attachment_index ==
                input_attachment_index);
        };
    require_input_attachment(0, 3);
    require_input_attachment(1, 5);

    const auto layered_compiled =
        compileSurfaceShaders(
            compiler, surface, source_name,
            SurfacePass::forward,
            layered_defines);
    requireCompiled(layered_compiled);
    const auto layered_fragment =
        reflect(
            layered_compiled.fragment.spirv);
    const auto layered_binding =
        std::find_if(
            layered_fragment.bindings.begin(),
            layered_fragment.bindings.end(),
            [](const auto &candidate) {
                return candidate.set == 1 &&
                       candidate.binding == 1;
            });
    REQUIRE(
        layered_binding !=
        layered_fragment.bindings.end());
    REQUIRE(
        layered_binding->type ==
        vk::DescriptorType::
            eCombinedImageSampler);
    REQUIRE(
        layered_binding
            ->image_view_dimension ==
        ReflectedImageViewDimension::
            two_d_array);
    REQUIRE_NOTHROW(
        validateShaderResourceInterfaceReflection(
            layered.resource_interface,
            layered_fragment));
#endif
}

TEST_CASE(
    "surface physical variant compiles a cube material resource accessor",
    "[surface-compiler][material-resource][cube][wp236]") {
    constexpr std::string_view source_name =
        "project://shaders/environment.surface";
    const auto surface = parseSurfaceFormat(
        R"surface(//! pelican.surface v1
//! language: glsl
//! resource_ports:
//!   - { name: environment, kind: image, stage: fragment }

void pelican_surface_v1(
    in PelicanSurfaceInputV1 input_data,
    inout PelicanSurfaceV1 surface) {
    surface.emissive =
        pelican_sample_environment(
            normalize(input_data.normal)).rgb;
}
)surface",
        source_name);
    const auto composition =
        composeSurfaceShaders(
            surface, source_name,
            SurfacePass::forward,
            {makeSurfaceResourceCubeDefine(0)});
    REQUIRE(
        composition.resource_interface.front()
            .port.view ==
        ShaderResourcePortView::cube);
    REQUIRE(
        composition.resource_interface.front()
            .image_view_dimension ==
        ReflectedImageViewDimension::cube);
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    const auto compiled =
        compileSurfaceShaders(
            compiler, surface, source_name,
            SurfacePass::forward,
            {makeSurfaceResourceCubeDefine(0)});
    requireCompiled(compiled);
    const auto reflection =
        reflect(compiled.fragment.spirv);
    const auto binding = std::find_if(
        reflection.bindings.begin(),
        reflection.bindings.end(),
        [](const auto &candidate) {
            return candidate.set == 1 &&
                   candidate.binding == 0;
        });
    REQUIRE(binding != reflection.bindings.end());
    REQUIRE(
        binding->image_view_dimension ==
        ReflectedImageViewDimension::cube);
#endif
}

TEST_CASE("unchanged example toon surface compiles all skinned template variants",
          "[surface-compiler][skeletal]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" / "example" /
                      "shaders" / "toon.surface";
    const auto authored = readText(path);
    const auto surface = parseSurfaceFormat(authored, "project://shaders/toon.surface");
    REQUIRE(authored.substr(surface.code_offset) == surface.code);
    ShaderCompiler compiler;
    for (const auto pass : {SurfacePass::main, SurfacePass::depth, SurfacePass::velocity}) {
        const auto composition = composeSurfaceShaders(surface, "project://shaders/toon.surface", pass,
                                                       {"PELICAN_SKINNED"});
        REQUIRE(std::find(composition.defines.begin(), composition.defines.end(), "PELICAN_SKINNED") !=
                composition.defines.end());
        requireCompiled(compileSurfaceShaders(compiler, surface, "project://shaders/toon.surface", pass,
                                              {"PELICAN_SKINNED"}));
    }
#endif
}

} // namespace Pelican
