#include <components/modelview.hpp>
#include <components/predefined.hpp>
#include <gameobjects.hpp>
#include <gamesystem.hpp>

namespace {

class ExamplePlayerControl {
    Pelican::GameObjectId object = Pelican::invalidGameObjectId;
    bool object_created = false;

    Pelican::GameObjectId createControlledObject(Pelican::GameContext &ctx) {
        Pelican::LocalTransformComponent transform{
            .scale = Pelican::vec3{0.35f, 0.35f, 0.35f},
            .rotation = Pelican::quat{0.0f, 0.7071f, -0.7071f, 0.0f},
            .pos = Pelican::vec3{0.0f, -1.2f, 1.0f},
            .parent = Pelican::invalidGameObjectId,
        };
        Pelican::SimpleModelViewUpdateComponent model_update;
        model_update.model_name = "character";
        model_update.dirty = true;

        const auto id = Pelican::GameObjects::add()
                            .addComponent<Pelican::TransformComponent>()
                            .addComponent<Pelican::LocalTransformComponent>(transform)
                            .addComponent<Pelican::SimpleModelViewComponent>()
                            .addComponent<Pelican::SimpleModelViewUpdateComponent>(model_update)
                            .finish();
        (void)ctx.setLocalTransform(id, transform);
        return id;
    }

  public:
    void update(Pelican::GameContext &ctx) {
        if (!object_created) {
            object = createControlledObject(ctx);
            object_created = true;
        }

        if (!ctx.actionsConfigured()) {
            return;
        }

        const auto move = ctx.actionAxis2("move");
        if (move.x == 0.0f && move.y == 0.0f) {
            return;
        }

        auto transform = ctx.localTransform(object);
        const auto dt = static_cast<float>(ctx.deltaTime() > 0.0 ? ctx.deltaTime() : 1.0 / 60.0);
        constexpr float speed = 2.5f;
        transform.pos.x += move.x * speed * dt;
        transform.pos.z += move.y * speed * dt;
        (void)ctx.setLocalTransform(object, transform);
    }
};

} // namespace

PELICAN_REGISTER_SYSTEM(ExamplePlayerControl, 100);
