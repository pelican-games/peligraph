#include <gamecontext.hpp>
#include <physics/abi_v1.hpp>
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

extern "C" __declspec(dllexport) std::uint32_t pelican_physics_service_public_api_link_probe() {
    auto api = Pelican::Physics::descriptor<Pelican::Physics::ApiV1>();
    return static_cast<std::uint32_t>(
        Pelican::Physics::getApiV1(Pelican::Physics::abiVersionV1, &api));
}

extern "C" __declspec(dllexport) std::size_t pelican_physquery_public_api_link_probe(
    Pelican::GameContext *context) {
    if (context == nullptr) {
        return sizeof(Pelican::phys::ObjectRaycastHit) + sizeof(Pelican::phys::RaycastQueryHit);
    }

    const Pelican::phys::Ray ray{};
    const Pelican::phys::QueryFilter filter{};
    const auto legacy_closest = context->raycastClosest(ray);
    const auto detailed_closest = context->raycastClosest(ray, filter);
    const auto all_hits = context->raycastAll(ray, filter);
    const auto legacy_overlaps = context->overlapAll(Pelican::phys::Sphere{});
    const auto detailed_overlaps = context->overlapAllHits(Pelican::phys::Sphere{}, filter);
    return legacy_closest.has_value() + detailed_closest.has_value() + all_hits.size() +
           legacy_overlaps.size() + detailed_overlaps.size();
}
