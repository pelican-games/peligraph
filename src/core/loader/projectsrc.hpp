#pragma once

#include "../container.hpp"

namespace Pelican {

class ProjectSource {
    std::string path, raw_data;

  public:
    ProjectSource(DependencyContainer &con);
    ~ProjectSource();

    void setSourceByFile(std::string _path) { path = _path; }
    void setSourceByData(std::string _data) { raw_data = _data; }
    std::string loadSource() const;
};

} // namespace Pelican
