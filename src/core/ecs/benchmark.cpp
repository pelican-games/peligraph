#include "core.hpp"
#include "../profiler.hpp"
#include "componentinfo.hpp"
#include "predefined.hpp"
#include "predefined/transform.hpp"
#include <iostream>
#include <vector>
#include <random>
#include <glm/glm.hpp>

using namespace Pelican;

namespace Pelican {

struct VelocityComponent {
    glm::vec3 velocity;
};
struct RenderComponent {
    uint32_t mesh_id;
    uint32_t material_id;
};
DECLARE_COMPONENT(VelocityComponent, 10);
DECLARE_COMPONENT(RenderComponent, 11);

}

// Systems
struct PhysicsSystem {
    using QueryComponents = std::tuple<TransformComponent *, VelocityComponent *>;

    void process(std::tuple<TransformComponent *, VelocityComponent *> components, size_t count) {
        auto [transforms, velocities] = components;
        for (size_t i = 0; i < count; ++i) {
            transforms[i].pos += velocities[i].velocity * 0.016f; // dt = 16ms
        }
    }
};

struct RenderSystem {
    using QueryComponents = std::tuple<TransformComponent *, RenderComponent *>;

    void process(std::tuple<TransformComponent *, RenderComponent *> components, size_t count) {
        auto [transforms, renders] = components;
        volatile int dummy = 0; // Prevent optimization
        for (size_t i = 0; i < count; ++i) {
            // Simulate rendering overhead (matrix calculation etc)
            glm::mat4 model = glm::mat4(1.0f);
            model = glm::translate(model, transforms[i].pos);
            model = glm::scale(model, transforms[i].scale);
            dummy += renders[i].mesh_id + static_cast<int>(model[3][0]);
        }
    }
};

int main() {
    Pelican::setupLogger();

    // Initialize ComponentInfoManager
    auto& componentInfoManager = GET_MODULE(ComponentInfoManager);
    componentInfoManager.registerComponent(ComponentInfo{
        .id = ComponentIdByType<EntityId>::value,
        .size = sizeof(EntityId),
        .name = "EntityId",
    });
    componentInfoManager.registerComponent(ComponentInfo{
        .id = ComponentIdByType<TransformComponent>::value,
        .size = sizeof(TransformComponent),
        .name = "TransformComponent",
    });
    componentInfoManager.registerComponent(ComponentInfo{
        .id = ComponentIdByType<VelocityComponent>::value,
        .size = sizeof(VelocityComponent),
        .name = "VelocityComponent",
    });
    componentInfoManager.registerComponent(ComponentInfo{
        .id = ComponentIdByType<RenderComponent>::value,
        .size = sizeof(RenderComponent),
        .name = "RenderComponent",
    });

    ECSCore ecs;
    PhysicsSystem physicsSystem;
    RenderSystem renderSystem;

    // Register systems
    ecs.registerSystem<PhysicsSystem, TransformComponent, VelocityComponent>(physicsSystem, {});
    ecs.registerSystem<RenderSystem, TransformComponent, RenderComponent>(renderSystem, {});

    // Allocate entities with mixed archetypes
    const int entityCount = 10000000;
    std::cout << "Allocating " << entityCount << " entities with mixed archetypes..." << std::endl;

    // Archetype 1: Static (Transform) - 40%
    {
        int count = entityCount * 0.4;
        std::vector<ComponentId> ids = {ComponentIdByType<TransformComponent>::value};
        std::vector<void*> ptrs(1);
        ecs.allocateEntity(std::span(ids), std::span(ptrs), count);
        
        TransformComponent* transforms = static_cast<TransformComponent*>(ptrs[0]);
        for(int i=0; i<count; ++i) transforms[i].pos = glm::vec3(i, 0, 0);
    }

    // Archetype 2: Dynamic (Transform + Velocity) - 40%
    {
        int count = entityCount * 0.4;
        std::vector<ComponentId> ids = {
            ComponentIdByType<TransformComponent>::value,
            ComponentIdByType<VelocityComponent>::value
        };
        std::vector<void*> ptrs(2);
        ecs.allocateEntity(std::span(ids), std::span(ptrs), count);
        
        TransformComponent* transforms = static_cast<TransformComponent*>(ptrs[0]);
        VelocityComponent* velocities = static_cast<VelocityComponent*>(ptrs[1]);
        for(int i=0; i<count; ++i) {
            transforms[i].pos = glm::vec3(i, 10, 0);
            velocities[i].velocity = glm::vec3(1.0f, 0.0f, 0.0f);
        }
    }

    // Archetype 3: Visible Dynamic (Transform + Velocity + Render) - 20%
    {
        int count = entityCount * 0.2;
        std::vector<ComponentId> ids = {
            ComponentIdByType<TransformComponent>::value,
            ComponentIdByType<VelocityComponent>::value,
            ComponentIdByType<RenderComponent>::value
        };
        std::vector<void*> ptrs(3);
        ecs.allocateEntity(std::span(ids), std::span(ptrs), count);
        
        TransformComponent* transforms = static_cast<TransformComponent*>(ptrs[0]);
        VelocityComponent* velocities = static_cast<VelocityComponent*>(ptrs[1]);
        RenderComponent* renders = static_cast<RenderComponent*>(ptrs[2]);
        for(int i=0; i<count; ++i) {
            transforms[i].pos = glm::vec3(i, 20, 0);
            velocities[i].velocity = glm::vec3(0.0f, 1.0f, 0.0f);
            renders[i].mesh_id = i % 10;
        }
    }

    std::cout << "Starting realistic benchmark..." << std::endl;

    TimeProfilerStart("ECS_Benchmark_Realistic_Total");
    for (int i = 0; i < 100; ++i) {
        TimeProfilerStart("ECS_Update");
        ecs.update();
        TimeProfilerEnd("ECS_Update");
    }
    TimeProfilerEnd("ECS_Benchmark_Realistic_Total");

    std::cout << "Benchmark finished." << std::endl;
    
    return 0;
}
