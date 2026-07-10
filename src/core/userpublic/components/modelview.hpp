#pragma once

#include "../../renderer/modelinstance.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace Pelican {

struct SimpleModelViewUpdateComponent {
    std::string model_name;
    uint8_t dirty = 0;

    SimpleModelViewUpdateComponent() = default;
    SimpleModelViewUpdateComponent(const SimpleModelViewUpdateComponent &) = default;
    SimpleModelViewUpdateComponent(SimpleModelViewUpdateComponent &&) noexcept = default;
    SimpleModelViewUpdateComponent &operator=(const SimpleModelViewUpdateComponent &) = default;
    SimpleModelViewUpdateComponent &operator=(SimpleModelViewUpdateComponent &&) noexcept = default;
    ~SimpleModelViewUpdateComponent() = default;

    template <class T> void ref(T &ar) { ar.prop("model", model_name); }

    void init() { dirty = true; }
    void deinit() noexcept {}
};

} // namespace Pelican
