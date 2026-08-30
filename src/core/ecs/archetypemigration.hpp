#pragma once

#include <details/ecs/component.hpp>
#include <details/ecs/entity.hpp>

#include <functional>
#include <memory>
#include <span>
#include <vector>

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

class ECSArchetypeMigrationToken {
    struct State;
    std::unique_ptr<State> state_;

    explicit ECSArchetypeMigrationToken(std::unique_ptr<State> state) noexcept;
    friend class ECSArchetypeMigration;

  public:
    ECSArchetypeMigrationToken() noexcept;
    ~ECSArchetypeMigrationToken();
    ECSArchetypeMigrationToken(ECSArchetypeMigrationToken &&) noexcept;
    ECSArchetypeMigrationToken &operator=(ECSArchetypeMigrationToken &&) noexcept;
    ECSArchetypeMigrationToken(const ECSArchetypeMigrationToken &) = delete;
    ECSArchetypeMigrationToken &operator=(const ECSArchetypeMigrationToken &) = delete;

    explicit operator bool() const noexcept { return state_ != nullptr; }
    bool published() const noexcept;
    void *stagedComponent() const noexcept;
    void *removedComponent() const noexcept;

    void publish() noexcept;
    void rollback() noexcept;
    void finish() noexcept;
};

enum class ECSEntityMutationKind {
    create,
    destroy,
};

class ECSEntityMutationToken {
    struct State;
    std::unique_ptr<State> state_;

    explicit ECSEntityMutationToken(std::unique_ptr<State> state) noexcept;
    friend class ECSEntityMutation;

  public:
    ECSEntityMutationToken() noexcept;
    ~ECSEntityMutationToken();
    ECSEntityMutationToken(ECSEntityMutationToken &&) noexcept;
    ECSEntityMutationToken &operator=(ECSEntityMutationToken &&) noexcept;
    ECSEntityMutationToken(const ECSEntityMutationToken &) = delete;
    ECSEntityMutationToken &operator=(const ECSEntityMutationToken &) = delete;

    explicit operator bool() const noexcept { return state_ != nullptr; }
    EntityId entity() const noexcept;
    ECSEntityMutationKind kind() const noexcept;
    bool published() const noexcept;
    void publish() noexcept;
    void rollback() noexcept;
    void finish() noexcept;
};

class ECSEntityMutation {
  public:
    using Populate = std::function<void(std::span<void *>)>;

    static ECSEntityMutationToken
    prepareCreate(ECSCoreTemplatePublic &core,
                  std::span<const ComponentId> component_ids,
                  const Populate &populate = {});
    static ECSEntityMutationToken prepareDestroy(ECSCoreTemplatePublic &core,
                                                 EntityId entity);
};

class ECSArchetypeMigration {
  public:
    using Populate = std::function<void(void *)>;
    using Adapters = std::span<ECSArchetypeMigrationAdapter *const>;

    static ECSArchetypeMigrationToken
    prepareAdd(ECSCoreTemplatePublic &core, EntityId entity,
               ComponentId component, const Populate &populate = {},
               Adapters adapters = {});
    static ECSArchetypeMigrationToken
    prepareRemove(ECSCoreTemplatePublic &core, EntityId entity,
                  ComponentId component, Adapters adapters = {});

    static void add(ECSCoreTemplatePublic &core, EntityId entity, ComponentId component,
                    const Populate &populate = {}, Adapters adapters = {});
    static void remove(ECSCoreTemplatePublic &core, EntityId entity, ComponentId component,
                       Adapters adapters = {});

  private:
    static ECSArchetypeMigrationToken
    prepare(ECSCoreTemplatePublic &core, EntityId entity, ComponentId component,
            ECSArchetypeMigrationKind kind, const Populate &populate,
            Adapters adapters);
};

} // namespace Pelican
