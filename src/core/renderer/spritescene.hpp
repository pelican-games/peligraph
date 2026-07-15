#pragma once

#include "../container.hpp"
#include "../ecs/predefined/transform.hpp"
#include <components/spriteview.hpp>
#include <sprite/spriteworld.hpp>

#include <memory>
#include <nlohmann/json_fwd.hpp>
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
    void prepareAssets(std::span<const SpriteSceneItem> items);
    void rebuild(std::span<const SpriteSceneItem> items);
    void updatePixelPolicy(std::uint32_t viewport_width, std::uint32_t viewport_height);
    void clear();
    const sprite::SpriteFrame &frame() const;
    nlohmann::json statusJson() const;
    std::size_t commandCountForTesting() const;
};

} // namespace Pelican
