#include "../../container.hpp"
#include <details/ecs/coretemplate.hpp>

#include "transform.hpp"
#include <components/collider.hpp>
#include <span>

namespace Pelican {

DECLARE_MODULE(SimpleCollisionSystem) {
  public:
    using Query = std::span<ChunkView<TransformComponent, SphereColliderComponent>>;
    void process(Query chunks);
};

} // namespace Pelican