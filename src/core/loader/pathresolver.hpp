#pragma once

#include "../container.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Pelican {

struct EngineResourceId {
    std::string id;
};

using ResolvedRef = std::variant<std::filesystem::path, EngineResourceId>;

DECLARE_MODULE(PathResolver) {
    bool configured = false;
    std::filesystem::path project_root_abs;
    bool allow_absolute_paths = false;

    ResolvedRef resolveRef(std::string_view ref, bool cli_origin) const;

  public:
    void setup(const std::filesystem::path &project_root_abs, bool allow_absolute_paths);
    void resetForTesting();

    bool isSetup() const { return configured; }

    ResolvedRef resolveProjectRef(std::string_view ref) const;
    ResolvedRef resolveCliRef(std::string_view ref) const;
    std::filesystem::path resolveExistingFile(std::string_view ref) const;
    std::string loadText(std::string_view ref) const;
    std::vector<std::byte> loadBytes(std::string_view ref) const;
};

} // namespace Pelican
