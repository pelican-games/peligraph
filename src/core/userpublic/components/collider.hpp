#pragma once

#include <serialize/jsonarchive.hpp>

#include <geom/vec.hpp>
#include <geom/quat.hpp>

#include <cstdint>
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
    std::uint32_t layer = 1U;
    std::uint32_t mask = ~std::uint32_t{0};
    bool trigger = false;
    bool one_way = false;

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
            ar.prop("layer", layer);
            ar.prop("mask", mask);
            std::uint8_t trigger_value = trigger ? 1U : 0U;
            std::uint8_t one_way_value = one_way ? 1U : 0U;
            ar.prop("trigger", trigger_value);
            ar.prop("one_way", one_way_value);
            trigger = trigger_value != 0;
            one_way = one_way_value != 0;
        }
    }

    void loadFromJsonArchive(const JsonArchiveLoader &archive);
    void validate() const;
    void init();
    void deinit() noexcept;
};

}  // namespace Pelican
