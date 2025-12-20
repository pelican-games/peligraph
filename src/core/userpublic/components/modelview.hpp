#pragma once

#include "../../renderer/modelinstance.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace Pelican {

struct SimpleModelViewUpdateComponent {
    std::string model_name;
    uint8_t dirty;

    template <class T> void ref(T &ar) { ar.prop("model", model_name); }

    void init() { dirty = true; }
    void deinit() {}
};

} // namespace Pelican
