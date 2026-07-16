#include <components/spriteview.hpp>
#include <gamecontext.hpp>
#include <gamesystem.hpp>
#include <platformer/charactercontroller2d.hpp>
#include <sprite/flipbook.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <variant>

namespace {

struct PlayerLanded {
    std::uint64_t collider = 0;

    template <class T> void ref(T &archive) {
        archive.prop("collider", collider);
    }
};

class SpriteFlipbookDemoSystem {
    Pelican::GameObjectId object = Pelican::invalidGameObjectId;
    Pelican::phys::Shape body = Pelican::phys::Capsule{
        .center = {-2.4f, 0.6001f, 0.0f},
        .rotation = {0.0f, 0.0f, 0.0f, 1.0f},
        .half_height = 0.4f,
        .radius = 0.2f,
    };
    Pelican::vec3 velocity{0.0f, 0.0f, 0.0f};
    bool grounded = false;
    double locomotion_time = 0.0;
    const Pelican::sprite::FlipbookClip clip{
        {{"demo_atlas#sprite/left", 0.20}, {"demo_atlas#sprite/right", 0.20}},
        Pelican::sprite::FlipbookPlayback::loop,
    };

    static Pelican::vec3 bodyCenter(const Pelican::phys::Shape &shape) {
        return std::visit([](const auto &value) { return value.center; }, shape);
    }

  public:
    void update(Pelican::GameContext &context) {
        if (object == Pelican::invalidGameObjectId) {
            context.setCamera("PixelCamera");
            Pelican::LocalTransformComponent transform{
                .scale = {1.0f, 1.0f, 1.0f},
                .rotation = {0.0f, 0.0f, 0.0f, 1.0f},
                .pos = {-2.4f, 0.6001f, 0.3f},
                .parent = Pelican::invalidGameObjectId,
            };
            Pelican::SpriteViewComponent sprite;
            sprite.texture = "demo_atlas#sprite/left";
            sprite.size = {0.4f, 1.2f};
            sprite.has_explicit_size = 1;
            sprite.pivot = {0.5f, 0.5f};
            sprite.layer = 10;
            object = context.createSpriteObject(transform, sprite);
            context.logInfo(
                "S2D-2 demo: A/D move, Space jumps, S drops through one-way floors");
        }

        const auto input = context.actionAxis2("move");
        const double dt = std::clamp(context.deltaTime(), 0.0, 0.1);
        velocity.x = input.x * 2.5f;
        if (context.actionPressed("jump") && grounded) velocity.y = 7.5f;
        velocity.y -= 18.0f * static_cast<float>(dt);

        Pelican::platformer::MoveAndSlide2DSettings settings;
        settings.collide_with_one_way = input.y > -0.5f;
        const bool was_grounded = grounded;
        const auto motion = Pelican::platformer::moveAndSlide(
            context, body,
            {velocity.x * static_cast<float>(dt),
             velocity.y * static_cast<float>(dt), 0.0f},
            {}, settings);
        body = motion.shape;
        grounded = motion.grounded;
        if (grounded && velocity.y < 0.0f) velocity.y = 0.0f;
        if (motion.hit_ceiling && velocity.y > 0.0f) velocity.y = 0.0f;

        auto transform = context.localTransform(object);
        const auto center = bodyCenter(body);
        transform.pos.x = center.x;
        transform.pos.y = center.y;
        (void)context.setLocalTransform(object, transform);

        if (std::abs(input.x) > 0.01f) {
            locomotion_time += dt * 3.0;
            if (auto sprite = context.spriteView(object)) {
                sprite->flip_x = input.x < 0.0f ? 1 : 0;
                (void)context.setSpriteView(object, *sprite);
            }
        }
        (void)clip.apply(context, object, locomotion_time);

        if (grounded && !was_grounded) {
            for (const auto &contact : motion.contacts) {
                if (contact.kind == Pelican::platformer::ContactKind2D::ground) {
                    context.emit(PlayerLanded{contact.hit.identity.collider_id.value});
                    break;
                }
            }
        }
    }

    void onEvent(const PlayerLanded &event, Pelican::GameContext &context) {
        context.logInfo("S2D-2 landed on collider " + std::to_string(event.collider));
    }
};

} // namespace

PELICAN_REGISTER_EVENT(PlayerLanded);
PELICAN_REGISTER_SYSTEM(SpriteFlipbookDemoSystem, 100);
