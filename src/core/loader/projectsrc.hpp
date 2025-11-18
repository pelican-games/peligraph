#pragma once

#include "../container.hpp"

namespace Pelican {

DECLARE_MODULE(ProjectSource) {
    std::string path, raw_data;

  public:
    ProjectSource();
    ~ProjectSource();

    void setSourceByFile(std::string _path) { path = _path; }
    void setSourceByData(std::string _data) { raw_data = _data; }
    std::string loadSource() const;
};

} // namespace Pelican
