#include "projectsrc.hpp"
#include <filesystem>
#include <fstream>

namespace Pelican {

ProjectSource::ProjectSource(DependencyContainer &con) {}
ProjectSource::~ProjectSource() {}

std::string ProjectSource::loadSource() const {
    if (!path.empty()) {
        const auto sz = std::filesystem::file_size(path);
        std::ifstream f{path, std::ios_base::binary};
        std::string loaded_data;
        loaded_data.resize(sz, '\0');
        f.read(loaded_data.data(), sz);
    }
    if (!raw_data.empty())
        return raw_data;
    throw std::runtime_error("no project data is specified");
}

} // namespace Pelican