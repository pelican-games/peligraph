#include <components/spriteview.hpp>
#include <gamecontext.hpp>
#include <gamesystem.hpp>
#include <sprite/flipbook.hpp>

namespace {

class SpriteFlipbookDemoSystem {
    Pelican::GameObjectId object = Pelican::invalidGameObjectId;
    const Pelican::sprite::FlipbookClip clip{
        {{"demo_atlas#sprite/left", 0.20}, {"demo_atlas#sprite/right", 0.20}},
        Pelican::sprite::FlipbookPlayback::loop,
    };

  public:
    void update(Pelican::GameContext &context) {
        if (object == Pelican::invalidGameObjectId) {
            context.setCamera("PixelCamera");
            Pelican::LocalTransformComponent transform{
                .scale = {1.0f, 1.0f, 1.0f},
                .rotation = {0.0f, 0.0f, 0.0f, 1.0f},
                .pos = {0.0f, -0.30f, 0.3f},
                .parent = Pelican::invalidGameObjectId,
            };
            Pelican::SpriteViewComponent sprite;
            sprite.texture = "demo_atlas#sprite/left";
            object = context.createSpriteObject(transform, sprite);
            context.logInfo("S2D-1 flipbook demo: public helper is animating a user-created sprite");
        }
        (void)clip.apply(context, object, context.time());
    }
};

} // namespace

PELICAN_REGISTER_SYSTEM(SpriteFlipbookDemoSystem, 100);
