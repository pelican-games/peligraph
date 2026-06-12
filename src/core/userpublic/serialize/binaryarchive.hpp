#pragma once

#include <cstdint>
#include <geom/quat.hpp>
#include <geom/vec.hpp>
#include <string>

namespace Pelican {

class BinaryArchive {
  public:
    void prop(const char *name, int8_t &val);
    void prop(const char *name, int16_t &val);
    void prop(const char *name, int32_t &val);
    void prop(const char *name, int64_t &val);
    void prop(const char *name, uint8_t &val);
    void prop(const char *name, uint16_t &val);
    void prop(const char *name, uint32_t &val);
    void prop(const char *name, uint64_t &val);
    void prop(const char *name, float &val);
    void prop(const char *name, double &val);
    void prop(const char *name, std::string &val);
    void prop(const char *name, vec2 &val);
    void prop(const char *name, vec3 &val);
    void prop(const char *name, vec4 &val);
    void prop(const char *name, quat &val);
};

} // namespace Pelican
