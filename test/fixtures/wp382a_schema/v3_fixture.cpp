#include <behavior.hpp>
#include <details/component/registerer.hpp>
#include <details/ecs/componentdeclare.hpp>

#include <cstdint>
#include <optional>
#include <string>

#ifndef WP382A_SCHEMA_VARIANT
#define WP382A_SCHEMA_VARIANT 0
#endif

namespace {

enum class InstanceFlag : std::uint32_t {
    CullDisable = 1u,
    FlipFacing = 2u,
#if WP382A_SCHEMA_VARIANT == 3
    ForceOpaque = 8u,
#else
    ForceOpaque = 4u,
#endif
};

struct OverrideRow {
    std::string occurrence_id;
    std::optional<std::uint8_t> mask;
    std::optional<Pelican::Flags<InstanceFlag>> instance_flags;
    std::optional<std::uint32_t> custom_index;

    static constexpr auto schema = Pelican::structFields(
        Pelican::componentPolicy("wp382a_override_row"),
        Pelican::required(
            Pelican::field<&OverrideRow::occurrence_id>("occurrence_id")),
#if WP382A_SCHEMA_VARIANT == 4
        Pelican::required(Pelican::field<&OverrideRow::mask>(
            "mask", Pelican::urange(0, 255))),
#else
        Pelican::optionalOverride(Pelican::field<&OverrideRow::mask>(
            "mask", Pelican::urange(0, 255))),
#endif
        Pelican::optionalOverride(
            Pelican::field<&OverrideRow::instance_flags>("instance_flags"),
            Pelican::enumValues(
                Pelican::enumValue("cull_disable", InstanceFlag::CullDisable),
                Pelican::enumValue("flip_facing", InstanceFlag::FlipFacing),
                Pelican::enumValue("force_opaque", InstanceFlag::ForceOpaque))),
        Pelican::optionalOverride(Pelican::field<&OverrideRow::custom_index>(
            "custom_index", Pelican::urange(0, 0x00ffffffu))));
};

struct CarrierParams {
    Pelican::List<OverrideRow> overrides;
    bool enabled = false;

#if WP382A_SCHEMA_VARIANT == 2
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(
            Pelican::field<&CarrierParams::enabled>("enabled"), false),
        Pelican::defaulted(
            Pelican::field<&CarrierParams::overrides>("overrides")));
#else
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(
            Pelican::field<&CarrierParams::overrides>("overrides")),
        Pelican::defaulted(
            Pelican::field<&CarrierParams::enabled>("enabled"),
#if WP382A_SCHEMA_VARIANT == 1
            true
#else
            false
#endif
                ));
#endif
};

class CarrierBehavior final : public Pelican::Behavior {
  public:
    using Params = CarrierParams;
};

#if !defined(WP382A_RELOAD_ONLY)
struct CarrierEvent {
    Pelican::List<OverrideRow> rows;

    static constexpr auto pelican_payload = Pelican::structFields(
        Pelican::eventPayloadPolicy,
        Pelican::required(Pelican::field<&CarrierEvent::rows>("rows")));

    template <class Archive> void ref(Archive &archive) {
        archive.prop("rows", rows);
    }
};

struct CarrierComponent {
    Pelican::List<OverrideRow> rows;

    static constexpr auto schema = Pelican::structFields(
        Pelican::componentPolicy("wp382a_v3_component"),
        Pelican::defaulted(Pelican::field<&CarrierComponent::rows>("rows")));
};
#endif

} // namespace

#if !defined(WP382A_RELOAD_ONLY)
DECLARE_COMPONENT(CarrierComponent, 61);

namespace {

class CarrierComponentRegistration final {
  public:
    CarrierComponentRegistration() {
        (void)Pelican::internal::getComponentRegisterer()
            .registerComponent<CarrierComponent>("wp382a_v3_component");
    }
};

CarrierComponentRegistration component_registration;

} // namespace

PELICAN_REGISTER_EVENT(CarrierEvent);
#endif
PELICAN_REGISTER_BEHAVIOR(CarrierBehavior, "wp382a_carrier", 1);
