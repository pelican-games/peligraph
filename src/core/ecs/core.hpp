#pragma once

#include "../container.hpp"

#include "componentinfo.hpp"
#include <details/ecs/coretemplate.hpp>

namespace Pelican {

using SystemId = uint64_t;

DECLARE_MODULE(ECSCore) {
    ECSCoreTemplatePublic sub;

  public:
    EntityId allocateEntity(std::span<const ComponentId> component_ids, std::span<void *> component_ptrs,
                            size_t count) {
        return sub.allocateEntity(component_ids, component_ptrs, count);
    }
    void remove(EntityId id) { sub.remove(id); }
    void compaction() { sub.compaction(); }

    template <class TSystem, class... TComponents>
    SystemId registerSystem(TSystem & system, std::vector<SystemId> && depends_list) {
        return sub.registerSystem<TSystem, TComponents...>(system, std::move(depends_list));
    }
    void unregisterSystem(SystemId system_id) { sub.unregisterSystem(system_id); }

    void update() { sub.update(); };
};

} // namespace Pelican