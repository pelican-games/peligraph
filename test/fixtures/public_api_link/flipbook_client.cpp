#include <gamecontext.hpp>
#include <sprite/flipbook.hpp>

#include <cstddef>

extern "C" __declspec(dllexport) bool pelican_flipbook_public_api_link_probe(
    Pelican::GameContext *context, Pelican::GameObjectId object, double local_time_seconds) {
    const Pelican::sprite::FlipbookClip clip{
        {{"fixture#sprite/left", 0.1}, {"fixture#sprite/right", 0.2}},
        Pelican::sprite::FlipbookPlayback::loop,
    };
    if (context == nullptr) {
        return clip.frameCount() == 2 && clip.frameIndex(local_time_seconds) < 2;
    }
    return clip.apply(*context, object, local_time_seconds);
}
