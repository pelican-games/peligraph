#pragma once

#include "../container.hpp"

#include "componentinfo.hpp"
#include "archetypemigration.hpp"
#include <details/ecs/coretemplate.hpp>

namespace Pelican {

using SystemId = uint64_t;

DECLARE_MODULE(ECSCore) {
    ECSCoreTemplatePublic sub;

  public:
    ECSCoreTemplatePublic &getTemplatePublicModule() { return sub; }

    std::vector<EntityId> createEntities(std::span<const ComponentId> component_ids, size_t count,
                                         const ECSCoreTemplatePublic::PopulateBatch &populate = {}) {
        return sub.createEntities(component_ids, count, populate);
    }
    EntityId createEntity(std::span<const ComponentId> component_ids,
                          const std::function<void(std::span<void *>)> &populate = {}) {
        return sub.createEntity(component_ids, populate);
    }
    [[nodiscard]] bool remove(EntityId id) { return sub.remove(id); }
    void addComponent(EntityId entity, ComponentId component,
                      const ECSArchetypeMigration::Populate &populate = {},
                      ECSArchetypeMigration::Adapters adapters = {}) {
        ECSArchetypeMigration::add(sub, entity, component, populate, adapters);
    }
    void removeComponent(EntityId entity, ComponentId component,
                         ECSArchetypeMigration::Adapters adapters = {}) {
        ECSArchetypeMigration::remove(sub, entity, component, adapters);
    }
    void clearEntities() { sub.clearEntities(); }

    template <class TSystem, class... TComponents>
    SystemId registerSystem(TSystem & system, std::vector<SystemId> && depends_list, bool force_update = false) {
        return sub.registerSystem<TSystem, TComponents...>(system, std::move(depends_list), force_update);
    }
    
    template <class TSystem, class... TComponents>
    SystemId registerSystemForce(TSystem & system, std::vector<SystemId> && depends_list) {
        return sub.registerSystem<TSystem, TComponents...>(system, std::move(depends_list), true);
    }
    void unregisterSystem(SystemId system_id) { sub.unregisterSystem(system_id); }

    void update() { sub.update(); };
};

} // namespace Pelican
