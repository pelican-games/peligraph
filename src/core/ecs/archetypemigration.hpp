#pragma once

#include <details/ecs/component.hpp>
#include <details/ecs/entity.hpp>

#include <functional>
#include <span>

namespace Pelican {

class ECSCoreTemplatePublic;

enum class ECSArchetypeMigrationKind {
    add,
    remove,
};

struct ECSArchetypeMigrationPrepareContext {
    ECSCoreTemplatePublic &core;
    EntityId entity;
    ComponentId component;
    ECSArchetypeMigrationKind kind;
    // For remove this is the currently published component. For add it is null.
    void *live_component;
    // For add this is initialized staging storage. For remove it is null.
    void *staged_component;
};

struct ECSArchetypeMigrationPublishContext {
    ECSCoreTemplatePublic &core;
    EntityId entity;
    ComponentId component;
    ECSArchetypeMigrationKind kind;
    // The newly published component for add; null after remove.
    void *live_component;
};

// This is only a composition boundary. Light, collider, behavior, and other special
// attachments remain separate adapters owned by their respective projection WPs.
// prepare() may throw but must not change published state. rollback() is invoked for
// every attempted prepare (including the one that threw). Publication must not fail.
class ECSArchetypeMigrationAdapter {
  public:
    virtual ~ECSArchetypeMigrationAdapter() = default;
    virtual void prepare(const ECSArchetypeMigrationPrepareContext &context) = 0;
    virtual void rollback(const ECSArchetypeMigrationPrepareContext &context) noexcept = 0;
    virtual void publish(const ECSArchetypeMigrationPublishContext &context) noexcept = 0;
};

class ECSArchetypeMigration {
  public:
    using Populate = std::function<void(void *)>;
    using Adapters = std::span<ECSArchetypeMigrationAdapter *const>;

    static void add(ECSCoreTemplatePublic &core, EntityId entity, ComponentId component,
                    const Populate &populate = {}, Adapters adapters = {});
    static void remove(ECSCoreTemplatePublic &core, EntityId entity, ComponentId component,
                       Adapters adapters = {});

  private:
    static void migrate(ECSCoreTemplatePublic &core, EntityId entity, ComponentId component,
                        ECSArchetypeMigrationKind kind, const Populate &populate,
                        Adapters adapters);
};

} // namespace Pelican
