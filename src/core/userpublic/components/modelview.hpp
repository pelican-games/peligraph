#pragma once

#include "../../renderer/modelinstance.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace Pelican {

struct SimpleModelViewComponent {
    std::string model_name;
    uint8_t dirty = 0;
    std::optional<ModelInstanceId> model_instance_id;
    // Runtime-only join key. SceneLoader assigns the stable authoring identity
    // after entity creation; it is never serialized by ref().
    std::uint64_t authoring_object_id = 0;

    template <class T> void ref(T &ar) { ar.prop("model", model_name); }

    void init();
    void deinit() noexcept;
};

} // namespace Pelican
