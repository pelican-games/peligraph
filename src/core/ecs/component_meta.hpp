#pragma once

#include <cstdint>

#include "component.hpp"

#include "entity.hpp"
#include "predefined/modelview.hpp"
#include "predefined/transform.hpp"

namespace Pelican {

template <class T> struct ComponentIdByType {
    static constexpr ComponentId value;
};

#define DECLARE_COMPONENT(_name, _id)                                                                                  \
    template <> struct ComponentIdByType<_name> {                                                                      \
        static constexpr ComponentId value = _id;                                                                      \
    };

#define DECLARE_COMPONENT_SIZE(_name, _id)                                                                             \
    case _id:                                                                                                          \
        return sizeof(_name);

inline uint32_t getSizeFromComponentId(ComponentId id) {
    switch (id) {
        DECLARE_COMPONENT_SIZE(EntityId, 0)
        DECLARE_COMPONENT_SIZE(TransformComponent, 1)
        DECLARE_COMPONENT_SIZE(SimpleModelViewComponent, 2)
    }
}

// predefined
DECLARE_COMPONENT(EntityId, 0);
DECLARE_COMPONENT(TransformComponent, 1);
DECLARE_COMPONENT(SimpleModelViewComponent, 2);

} // namespace Pelican