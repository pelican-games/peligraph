#pragma once

#include "../../renderer/modelinstance.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace Pelican {

struct SimpleModelViewComponent {
    std::optional<ModelInstanceId> model_instance_id;

    template <class T> void ref(T &ar) {}

    void init();
    void deinit();
};

} // namespace Pelican
