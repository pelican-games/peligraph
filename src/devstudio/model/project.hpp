#pragma once

#include <string>

#define DECLARE_PROPERTY(name, type)                                                                                   \
  private:                                                                                                             \
    type _##name;                                                                                                      \
                                                                                                                       \
  public:                                                                                                              \
    const type &get##name() const { return _##name; }                                                                  \
    void set##name(const type &name) { _##name = name; }

namespace PelicanStudio {

class ProjectInfo {
    DECLARE_PROPERTY(Name, std::string);
};

} // namespace PelicanStudio
