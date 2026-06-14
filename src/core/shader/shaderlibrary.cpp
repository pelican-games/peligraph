#include "shaderlibrary.hpp"
#include "shadercompiler.hpp"
#include "../loader/fileio.hpp"
#include "../vkcore/core.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <system_error>

namespace Pelican {

namespace {

std::string lowerExtension(const std::filesystem::path &path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::vector<uint32_t> bytesToSpirv(const std::string &data, const std::filesystem::path &path) {
    if (data.empty()) {
        throw std::runtime_error("Shader SPIR-V file is empty: " + path.string());
    }
    if (data.size() % sizeof(uint32_t) != 0) {
        throw std::runtime_error("Shader SPIR-V byte size is not a multiple of 4: " + path.string());
    }

    std::vector<uint32_t> spirv(data.size() / sizeof(uint32_t));
    std::memcpy(spirv.data(), data.data(), data.size());
    return spirv;
}

std::optional<std::filesystem::file_time_type> lastWriteTime(const std::filesystem::path &path) {
    std::error_code ec;
    const auto timestamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    return timestamp;
}

} // namespace

ShaderLibrary::ShaderLibrary(ShaderLibraryModuleMode mode) : module_mode{mode} {}

ShaderBundle ShaderLibrary::buildFromFile(const std::filesystem::path &path, uint64_t version) const {
    if (lowerExtension(path) == ".spv") {
        return buildFromSpirv(bytesToSpirv(readBinaryFile(path.string()), path), path, version, "");
    }

#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    const auto result = compiler.compileFile(path);
    if (!result.ok) {
        throw std::runtime_error("Shader compile failed: " + path.string() + "\n" + result.log);
    }
    return buildFromSpirv(result.spirv, path, version, result.log);
#else
    throw std::runtime_error("Runtime shader compiler is disabled; only .spv shader files are accepted: " +
                             path.string());
#endif
}

ShaderBundle ShaderLibrary::buildFromSpirv(std::span<const uint32_t> spirv, std::filesystem::path source_path,
                                           uint64_t version, std::string log) const {
    ShaderBundle bundle;
    bundle.module = createShaderModule(spirv);
    bundle.reflection = reflect(spirv);
    bundle.source_path = std::move(source_path);
    bundle.version = version;
    bundle.log = std::move(log);
    return bundle;
}

vk::UniqueShaderModule ShaderLibrary::createShaderModule(std::span<const uint32_t> spirv) const {
    if (spirv.empty()) {
        throw std::runtime_error("Shader module data must not be empty");
    }

    if (module_mode == ShaderLibraryModuleMode::reflection_only) {
        return {};
    }

    vk::ShaderModuleCreateInfo create_info;
    create_info.codeSize = spirv.size_bytes();
    create_info.pCode = spirv.data();
    return GET_MODULE(VulkanManageCore).getDevice().createShaderModuleUnique(create_info);
}

void ShaderLibrary::markDirty(ShaderBundleId id) {
    if (std::find(dirty_bundles.begin(), dirty_bundles.end(), id) == dirty_bundles.end()) {
        dirty_bundles.push_back(id);
    }
}

ShaderBundleId ShaderLibrary::loadFromFile(const std::filesystem::path &path) {
    const auto id = bundles.reg(buildFromFile(path, 1));
    bundle_ids.push_back(id);
    if (const auto timestamp = lastWriteTime(path)) {
        source_write_times[id] = *timestamp;
    }
    return id;
}

ShaderBundleId ShaderLibrary::loadFromSpirv(std::span<const uint32_t> spirv, std::string_view name) {
    const auto id = bundles.reg(buildFromSpirv(spirv, {}, 1, std::string{name}));
    bundle_ids.push_back(id);
    return id;
}

const ShaderBundle &ShaderLibrary::get(ShaderBundleId id) const { return bundles.get(id); }

bool ShaderLibrary::reload(ShaderBundleId id) {
    auto &current = bundles.get(id);
    if (current.source_path.empty()) {
        current.log = "Shader bundle has no source path";
        return false;
    }

    try {
        auto replacement = buildFromFile(current.source_path, current.version + 1);
        current.module = std::move(replacement.module);
        current.reflection = std::move(replacement.reflection);
        current.source_path = std::move(replacement.source_path);
        current.version = replacement.version;
        current.log = std::move(replacement.log);
        if (const auto timestamp = lastWriteTime(current.source_path)) {
            source_write_times[id] = *timestamp;
        }
        markDirty(id);
        return true;
    } catch (const std::exception &ex) {
        current.log = ex.what();
        return false;
    }
}

size_t ShaderLibrary::reloadModifiedSources(std::chrono::steady_clock::time_point now) {
    if (now < next_source_poll_time) {
        return 0;
    }
    next_source_poll_time = now + std::chrono::seconds{1};

    size_t reloaded_count = 0;
    for (const auto id : bundle_ids) {
        const auto &bundle = bundles.get(id);
        if (bundle.source_path.empty()) {
            continue;
        }

        const auto timestamp = lastWriteTime(bundle.source_path);
        if (!timestamp) {
            continue;
        }

        auto found = source_write_times.find(id);
        if (found == source_write_times.end()) {
            source_write_times[id] = *timestamp;
            continue;
        }
        if (found->second == *timestamp) {
            continue;
        }

        if (reload(id)) {
            ++reloaded_count;
        }
    }
    return reloaded_count;
}

std::vector<ShaderBundleId> ShaderLibrary::takeDirtyBundles() {
    auto dirty = std::move(dirty_bundles);
    dirty_bundles.clear();
    return dirty;
}

} // namespace Pelican
