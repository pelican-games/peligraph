#pragma once

#include <geom/quat.hpp>
#include <geom/vec.hpp>
#include <glm/ext/quaternion_float.hpp>
#include <glm/glm.hpp>

namespace Pelican {

inline glm::vec2 to_glm(vec2 v) { return glm::vec2{v.x, v.y}; }
inline glm::vec3 to_glm(vec3 v) { return glm::vec3{v.x, v.y, v.z}; }
inline glm::vec4 to_glm(vec4 v) { return glm::vec4{v.x, v.y, v.z, v.w}; }

inline glm::quat to_glm(quat v) { return glm::quat{v.w, v.x, v.y, v.z}; }

} // namespace Pelican
