#pragma once

#include "gamecontext.hpp"

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <typeindex>
#include <type_traits>

namespace Pelican {

PELICAN_DEFINE_HANDLE(BehaviorAttachmentHandle, std::uint64_t)

inline constexpr BehaviorAttachmentHandle invalidBehaviorAttachmentHandle{};

// BehaviorSystem participates in the same (order, stable name) total order as
// user game systems for both event delivery and update.
inline constexpr int behaviorSystemOrder = 50;
inline constexpr std::string_view behaviorSystemName = "BehaviorSystem";

class BehaviorAttachmentArena;

class PELICAN_API BehaviorContext : public GameContext {
    GameObjectId self_id = invalidGameObjectId;
    BehaviorAttachmentHandle attachment_handle = invalidBehaviorAttachmentHandle;
    std::uint64_t attachment_sequence = 0;
    void *params_value = nullptr;
    const std::type_info *params_type = &typeid(void);

    BehaviorContext(GameObjectId self, BehaviorAttachmentHandle attachment,
                    std::uint64_t sequence, void *params,
                    const std::type_info *params_type_info) noexcept;

    friend class BehaviorAttachmentArena;

  public:
    GameObjectId self() const noexcept { return self_id; }
    BehaviorAttachmentHandle attachment() const noexcept { return attachment_handle; }
    std::uint64_t attachmentSeq() const noexcept { return attachment_sequence; }

    template <class Params> Params &params() const {
        using Value = std::remove_cvref_t<Params>;
        if (params_value == nullptr || params_type == nullptr || *params_type != typeid(Value)) {
            throw std::logic_error("behavior params type does not match the registered Params type");
        }
        return *static_cast<Value *>(params_value);
    }

    // Structural changes requested by a behavior callback are applied at the
    // next behavior boundary. A deferred create has no live EntityId yet and
    // therefore returns invalidGameObjectId.
    GameObjectId createObject(const LocalTransformComponent &transform) const;
    GameObjectId createSpriteObject(const LocalTransformComponent &transform,
                                    const SpriteViewComponent &sprite) const;
    [[nodiscard]] bool removeObject(GameObjectId id) const;
};

class PELICAN_API Behavior {
  public:
    virtual ~Behavior() = default;
    virtual void onInit(BehaviorContext &) {}
    virtual void onUpdate(BehaviorContext &) {}
    virtual void onDestroy(BehaviorContext &) noexcept {}
};

} // namespace Pelican

#include "details/behavior/registerer.hpp"
