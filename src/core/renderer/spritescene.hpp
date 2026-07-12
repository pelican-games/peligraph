#pragma once

#include "../container.hpp"
#include "../ecs/predefined/transform.hpp"
#include <components/spriteview.hpp>
#include <sprite/spriteworld.hpp>

#include <memory>
#include <span>

namespace Pelican {

struct SpriteSceneItem {
    EntityId entity;
    const TransformComponent *transform = nullptr;
    const SpriteViewComponent *view = nullptr;
};

DECLARE_MODULE(SpriteScene) {
    struct State;
    std::unique_ptr<State> state;

  public:
    SpriteScene();
    ~SpriteScene();
    void rebuild(std::span<const SpriteSceneItem> items);
    void clear();
    const sprite::SpriteFrame &frame() const;
    std::size_t commandCountForTesting() const;
};

} // namespace Pelican
