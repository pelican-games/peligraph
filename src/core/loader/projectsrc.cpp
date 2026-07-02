#include "projectsrc.hpp"
#include <filesystem>
#include <fstream>
#include <utility>

namespace Pelican {

ProjectSource::ProjectSource() {}
ProjectSource::~ProjectSource() {}

void ProjectSource::setProjectData(std::string _data) {
    project_data = std::move(_data);
    project_data_set = true;
}

std::string ProjectSource::loadSource() const {
    if (!path.empty()) {
        const auto sz = std::filesystem::file_size(path);
        std::ifstream f{path, std::ios_base::binary};
        std::string loaded_data;
        loaded_data.resize(sz, '\0');
        f.read(loaded_data.data(), sz);
        return loaded_data;
    }
    return raw_data;
}

std::string ProjectSource::loadProjectSource() const {
    if (!project_data_set) {
        throw std::runtime_error("no project.json data is specified");
    }
    return project_data;
}

} // namespace Pelican
