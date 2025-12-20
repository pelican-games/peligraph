#pragma once

#include "../../renderer/modelinstance.hpp"
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>

namespace Pelican {

struct SimpleModelViewUpdateComponent {
    std::string model_name;
    uint8_t dirty;

    template <class T> void ref(T &ar) { ar.prop("model", model_name); }

    auto &operator=(const SimpleModelViewUpdateComponent &o) {
        // model_name = o.model_name;
        // としたいがこうしないとなぜかデータぶっ壊れる
        // TODO: 調査
        model_name.clear();
        model_name.reserve(o.model_name.size());
        std::copy(o.model_name.begin(), o.model_name.end(), std::back_inserter(model_name));
        dirty = o.dirty;
        return *this;
    }

    void init() { dirty = true; }
    void deinit() {}
};

} // namespace Pelican
