#pragma once

#include <serialize/jsonarchive.hpp>

#include <geom/vec.hpp>
#include <geom/quat.hpp>

#include <string>
#include <type_traits>

namespace Pelican {

struct ColliderComponent {
    std::string shape = "sphere";
    vec3 pos{0.0f, 0.0f, 0.0f};
    quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    float radius = 0.5f;
    vec3 half_extents{0.5f, 0.5f, 0.5f};
    float half_height = 0.5f;

    template <class T>
    void ref(T& ar) {
        if constexpr (std::is_same_v<std::remove_cvref_t<T>, JsonArchiveLoader>) {
            loadFromJsonArchive(ar);
        } else {
            ar.prop("shape", shape);
            ar.prop("pos", pos);
            ar.prop("rotation", rotation);
            ar.prop("radius", radius);
            ar.prop("half_extents", half_extents);
            ar.prop("half_height", half_height);
        }
    }

    void loadFromJsonArchive(const JsonArchiveLoader &archive);
    void validate() const;
    void init();
    void deinit();
};

}  // namespace Pelican
