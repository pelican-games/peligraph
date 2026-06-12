#pragma once

#include <details/ecs/component.hpp>
#include <details/ecs/entity.hpp>

namespace Pelican {

template <class T> struct ComponentIdByType;

#define DECLARE_COMPONENT(_name, _id)                                                                                  \
    template <> struct Pelican::ComponentIdByType<_name> {                                                             \
        static constexpr ComponentId value = _id;                                                                      \
    };

#define DECLARE_COMPONENT_CLASS(_name, _id)                                                                            \
    class _name;                                                                                                       \
    DECLARE_COMPONENT(_name, _id);

DECLARE_COMPONENT(EntityId, 0);

static constexpr size_t MAX_COMPONENTS = 64;
using ComponentMask = uint64_t;

} // namespace Pelican