#pragma once

#include "../container.hpp"

namespace Pelican {

DECLARE_MODULE(ProjectSource) {
    std::string path;
    std::string raw_data{"{}"};
    std::string project_data;
    bool project_data_set = false;
    bool ignore_engine_version = false;

  public:
    ProjectSource();
    ~ProjectSource();

    void setSourceByFile(std::string _path) { path = _path; }
    void setSourceByData(std::string _data) { raw_data = _data; }
    void setProjectData(std::string _data);
    void setIgnoreEngineVersion(bool ignore) { ignore_engine_version = ignore; }
    std::string loadSource() const;
    bool hasProjectSource() const { return project_data_set; }
    std::string loadProjectSource() const;
    bool ignoresEngineVersion() const { return ignore_engine_version; }
};

} // namespace Pelican
