#pragma once

#include "shadercompiler.hpp"
#include "spvlink.hpp"
#include "../../project/surfaceformat.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {

enum class SurfacePass {
    main,
    depth,
    velocity,
};

struct SurfaceShaderComposition {
    std::string vertex_source;
    std::string fragment_source;
    std::vector<std::pair<std::string, std::string>> virtual_includes;
    std::vector<std::string> defines;
};

struct SurfaceCompileResult {
    ShaderCompileResult vertex;
    ShaderCompileResult fragment;
    std::vector<SpvLinkBinding> vertex_bindings;
    std::vector<SpvLinkBinding> fragment_bindings;
    std::string vertex_cache_key;
    std::string fragment_cache_key;
    bool experimental_spv_link = false;
};

SurfaceShaderComposition composeSurfaceShaders(const SurfaceFormatDocument &surface,
                                                std::string_view source_name,
                                                SurfacePass pass = SurfacePass::main,
                                                std::vector<std::string> defines = {});

SurfaceCompileResult compileSurfaceShaders(ShaderCompiler &compiler,
                                           const SurfaceFormatDocument &surface,
                                           std::string_view source_name,
                                           SurfacePass pass = SurfacePass::main,
                                           std::vector<std::string> defines = {});

std::string_view surfacePassName(SurfacePass pass);

// Selection is deliberately process-explicit and defaults to false.  Merely
// linking SPIRV-Tools into the engine never changes the WP78 source path.
bool surfaceSpvLinkExperimentalEnabled();

} // namespace Pelican
