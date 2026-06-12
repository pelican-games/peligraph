#pragma once

#include <cstdint>
#include <geom/vec.hpp>

namespace Pelican {

struct SphereColliderComponent {
    vec3 pos;
    float radius;

    template <class T>
    void ref(T& ar) {
        ar.prop("pos", pos);
        ar.prop("radius", radius);
    }

    void init();
    void deinit();
};

// struct BoxColliderComponent {
//     vec3 pos;
//     vec3 size;

//     template <class T>
//     void ref(T& ar) {
//         ar.prop("pos", pos);
//         ar.prop("size", size);
//     }

//     void init();
//     void deinit();
// };

}  // namespace Pelican
