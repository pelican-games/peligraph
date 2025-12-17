#pragma once

#include "../../renderer/modelinstance.hpp"
#include <cstdint>
#include <string>

namespace Pelican {

struct SimpleModelViewComponent {
    ModelInstanceId model_instance_id;
    std::string model_name;

    template <class T> void ref(T &ar) { ar.prop("model", model_name); }

    void init();
    void deinit();
};

} // namespace Pelican
