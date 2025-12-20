#include "collision.hpp"
#define OBJECTS_SIZE 1000000

namespace Pelican {

void SimpleCollisionSystem::process(Query chunks) {
    std::vector<SphereColliderComponent> world_objects;
    world_objects.reserve(OBJECTS_SIZE);

    // オブジェクトを列挙(今は球だけ)
    for (auto &chunk : chunks) {
        auto [transforms, colliders] = chunk.components;
        size_t count = chunk.count;

        for (size_t i = 0; i < count; i++) {
            const auto& t = transforms[i];
            const auto& c = colliders[i];
            Pelican::vec3 world_pos;
            world_pos.x = t.pos.x;
            world_pos.y = t.pos.y;
            world_pos.z = t.pos.z;

            world_objects.emplace_back(SphereColliderComponent{
                .pos = world_pos,
                .radius = c.radius 
            });
        }
    }

    // 衝突検出
    size_t count = world_objects.size();
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            const auto& s1 = world_objects[i];
            const auto& s2 = world_objects[j];

            float dx = s1.pos.x - s2.pos.x;
            float dy = s1.pos.y - s2.pos.y;
            float dz = s1.pos.z - s2.pos.z;
            float dist2 = dx*dx + dy*dy + dz*dz;
            
            float radSum = s1.radius + s2.radius;

            if (dist2 < (radSum * radSum)) {
                // 衝突を検出した処理(何書けばいいのかわからん)
            }
        }
    }
}

}
