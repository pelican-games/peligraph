#include "shadercompiler.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

#if PELICAN_RUNTIME_SHADER_COMPILER
#include <shaderc/shaderc.hpp>
#endif

namespace Pelican {

namespace {

std::string readTextFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader source: " + path.string());
    }

    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

std::filesystem::path normalizedPath(const std::filesystem::path &path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return normalized;
    }
    return std::filesystem::absolute(path, ec);
}

#if PELICAN_RUNTIME_SHADER_COMPILER
shaderc_shader_kind toShadercKind(vk::ShaderStageFlagBits stage) {
    switch (stage) {
    case vk::ShaderStageFlagBits::eVertex:
        return shaderc_vertex_shader;
    case vk::ShaderStageFlagBits::eFragment:
        return shaderc_fragment_shader;
    case vk::ShaderStageFlagBits::eCompute:
        return shaderc_compute_shader;
    case vk::ShaderStageFlagBits::eRaygenKHR:
        return shaderc_raygen_shader;
    case vk::ShaderStageFlagBits::eMissKHR:
        return shaderc_miss_shader;
    case vk::ShaderStageFlagBits::eClosestHitKHR:
        return shaderc_closesthit_shader;
    default:
        throw std::runtime_error("Unsupported shader stage for shaderc");
    }
}

struct IncludeResultStorage {
    shaderc_include_result result{};
    std::string source_name;
    std::string content;
};

class FileIncluder final : public shaderc::CompileOptions::IncluderInterface {
    std::vector<std::filesystem::path> include_dirs;

    static shaderc_include_result *makeResult(std::string source_name, std::string content) {
        auto storage = std::make_unique<IncludeResultStorage>();
        storage->source_name = std::move(source_name);
        storage->content = std::move(content);
        storage->result.source_name = storage->source_name.c_str();
        storage->result.source_name_length = storage->source_name.size();
        storage->result.content = storage->content.c_str();
        storage->result.content_length = storage->content.size();
        storage->result.user_data = storage.get();
        return &storage.release()->result;
    }

    static shaderc_include_result *makeError(std::string message) { return makeResult("", std::move(message)); }

  public:
    explicit FileIncluder(std::vector<std::filesystem::path> dirs) : include_dirs{std::move(dirs)} {}

    shaderc_include_result *GetInclude(const char *requested_source, shaderc_include_type type,
                                       const char *requesting_source, size_t) override {
        std::vector<std::filesystem::path> roots;
        if (type == shaderc_include_type_relative && requesting_source != nullptr) {
            const std::filesystem::path requesting_path{requesting_source};
            if (requesting_path.has_parent_path()) {
                roots.push_back(requesting_path.parent_path());
            }
        }
        roots.insert(roots.end(), include_dirs.begin(), include_dirs.end());

        const std::filesystem::path requested_path{requested_source};
        std::vector<std::filesystem::path> candidates;
        if (requested_path.is_absolute()) {
            candidates.push_back(requested_path);
        } else {
            for (const auto &root : roots) {
                candidates.push_back(root / requested_path);
            }
        }

        for (const auto &candidate : candidates) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(candidate, ec)) {
                continue;
            }
            try {
                const auto resolved = normalizedPath(candidate).string();
                return makeResult(resolved, readTextFile(candidate));
            } catch (const std::exception &ex) {
                return makeError(ex.what());
            }
        }

        return makeError("Shader include not found: " + std::string{requested_source});
    }

    void ReleaseInclude(shaderc_include_result *data) override {
        if (data == nullptr) {
            return;
        }
        delete static_cast<IncludeResultStorage *>(data->user_data);
    }
};

ShaderCompileResult compileGlsl(std::string_view source, vk::ShaderStageFlagBits stage, std::string_view name,
                                std::string_view entry_point,
                                const std::vector<std::filesystem::path> &include_dirs,
                                const std::vector<std::string> &defines) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    options.SetIncluder(std::make_unique<FileIncluder>(include_dirs));
    for (const auto &define : defines) {
        options.AddMacroDefinition(define);
    }

    const std::string source_text{source};
    const std::string source_name{name};
    const std::string entry{entry_point};
    auto result = compiler.CompileGlslToSpv(source_text, toShadercKind(stage), source_name.c_str(), entry.c_str(),
                                            options);

    ShaderCompileResult output;
    output.log = result.GetErrorMessage();
    output.ok = result.GetCompilationStatus() == shaderc_compilation_status_success;
    if (output.ok) {
        output.spirv.assign(result.cbegin(), result.cend());
    }
    return output;
}
#endif

} // namespace

std::optional<vk::ShaderStageFlagBits> inferShaderStageFromPath(const std::filesystem::path &path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (ext == ".vert") {
        return vk::ShaderStageFlagBits::eVertex;
    }
    if (ext == ".frag") {
        return vk::ShaderStageFlagBits::eFragment;
    }
    if (ext == ".comp") {
        return vk::ShaderStageFlagBits::eCompute;
    }
    if (ext == ".rgen") {
        return vk::ShaderStageFlagBits::eRaygenKHR;
    }
    if (ext == ".rmiss") {
        return vk::ShaderStageFlagBits::eMissKHR;
    }
    if (ext == ".rchit") {
        return vk::ShaderStageFlagBits::eClosestHitKHR;
    }
    return std::nullopt;
}

ShaderCompileResult ShaderCompiler::compileFile(const std::filesystem::path &path, const ShaderCompileOptions &opts) {
    const auto stage = opts.stage.value_or(inferShaderStageFromPath(path).value_or(vk::ShaderStageFlagBits{}));
    if (stage == vk::ShaderStageFlagBits{}) {
        return {{}, "Unable to infer shader stage from path: " + path.string(), false};
    }

#if PELICAN_RUNTIME_SHADER_COMPILER
    auto compile_include_dirs = include_dirs;
    if (path.has_parent_path()) {
        const auto source_dir = normalizedPath(path.parent_path());
        compile_include_dirs.insert(compile_include_dirs.begin(), source_dir / "shaders" / "include");
        compile_include_dirs.insert(compile_include_dirs.begin(), source_dir);
    }
    return compileGlsl(readTextFile(path), stage, normalizedPath(path).string(), opts.entry_point,
                       compile_include_dirs, opts.defines);
#else
    (void)path;
    return {{}, "Runtime shader compiler is disabled", false};
#endif
}

ShaderCompileResult ShaderCompiler::compileSource(std::string_view source, vk::ShaderStageFlagBits stage,
                                                  std::string_view name, const ShaderCompileOptions &opts) {
#if PELICAN_RUNTIME_SHADER_COMPILER
    return compileGlsl(source, stage, name, opts.entry_point, include_dirs, opts.defines);
#else
    (void)source;
    (void)stage;
    (void)name;
    (void)opts;
    return {{}, "Runtime shader compiler is disabled", false};
#endif
}

void ShaderCompiler::addIncludeDir(const std::filesystem::path &dir) { include_dirs.push_back(normalizedPath(dir)); }

} // namespace Pelican
