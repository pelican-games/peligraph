#include <gamecontext.hpp>
#include <platformer/charactercontroller2d.hpp>
#include <physics/abi_v2.hpp>
#include <sprite/flipbook.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

struct PublicLinkEvent {
    std::uint64_t value = 0;

    template <class T> void ref(T &archive) {
        archive.prop("value", value);
    }
};

} // namespace

PELICAN_REGISTER_EVENT(PublicLinkEvent);

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
    auto api = Pelican::Physics::descriptor<Pelican::Physics::ApiV2>();
    return static_cast<std::uint32_t>(
        Pelican::Physics::getApiV2(Pelican::Physics::abiVersionV2, &api));
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
    const auto cast_hits = context->shapeCastAll(
        Pelican::phys::Sphere{}, Pelican::vec3{1.0F, 0.0F, 0.0F}, filter);
    const auto cast_closest = context->shapeCastClosest(
        Pelican::phys::Sphere{}, Pelican::vec3{1.0F, 0.0F, 0.0F}, filter);
    return legacy_closest.has_value() + detailed_closest.has_value() + all_hits.size() +
           legacy_overlaps.size() + detailed_overlaps.size() + cast_hits.size() +
           cast_closest.has_value();
}

extern "C" __declspec(dllexport) std::size_t pelican_platformer_public_api_link_probe(
    Pelican::GameContext *context) {
    const Pelican::phys::Shape body = Pelican::phys::Capsule{
        .center = {0.0F, 1.0F, 0.0F},
        .rotation = {0.0F, 0.0F, 0.0F, 1.0F},
        .half_height = 0.4F,
        .radius = 0.2F,
    };
    Pelican::platformer::MoveAndSlide2DResult result;
    if (context != nullptr) {
        result = Pelican::platformer::moveAndSlide(
            *context, body, Pelican::vec3{0.1F, -0.1F, 0.0F});
    } else {
        const Pelican::platformer::ShapeCastAll2DQuery no_hits =
            [](const Pelican::phys::Shape &, Pelican::vec3,
               const Pelican::phys::QueryFilter &) {
                return std::vector<Pelican::phys::ShapeCastQueryHit>{};
            };
        result = Pelican::platformer::moveAndSlide(
            body, Pelican::vec3{0.1F, -0.1F, 0.0F}, no_hits);
    }
    return result.contacts.size() + (result.grounded ? 1U : 0U);
}

extern "C" __declspec(dllexport) void pelican_event_emit_public_api_link_probe(
    Pelican::GameContext *context) {
    if (context != nullptr) context->emit(PublicLinkEvent{7});
}

extern "C" __declspec(dllexport) std::size_t pelican_light_public_api_link_probe(
    Pelican::GameContext *context) {
    if (context == nullptr) return 0;
    return context->setDirectionalLightDirection("Sun", {0.0F, -1.0F, 0.0F}) +
           context->setDirectionalLightIntensity("Sun", 2.0F) +
           context->setPointLightPosition("Lamp", {1.0F, 2.0F, 3.0F}) +
           context->setSpotLightDirection("Cone", {0.0F, -1.0F, 0.0F});
}
