#include <details/component/registerer.hpp>
#include <details/ecs/componentdeclare.hpp>
#include <details/event/registerer.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

// Do not include frozen behavior.hpp here: that public header intentionally
// reaches headers outside the v2 snapshot.  Spell the frozen entrance record
// locally so this fixture proves the actual v2 ABI without mixing current
// userpublic definitions into the DLL.
namespace Pelican {
class BehaviorContext;
class Behavior {
  public:
    virtual ~Behavior() = default;
    virtual void onInit(BehaviorContext &) {}
    virtual void onUpdate(BehaviorContext &) {}
    virtual void onDestroy(BehaviorContext &) noexcept {}
};

namespace internal {
struct RawBehaviorInstance {
    Behavior *behavior = nullptr;
    void *params = nullptr;
    const std::type_info *params_type = &typeid(void);
};
struct RawBehaviorParams {
    void *value = nullptr;
    const std::type_info *type = &typeid(void);
};
using BehaviorCreateFn = RawBehaviorInstance (*)(std::string_view);
using BehaviorDestroyFn = void (*)(RawBehaviorInstance &) noexcept;
using BehaviorPrepareParamsFn = RawBehaviorParams (*)(std::string_view);
using BehaviorApplyPreparedParamsFn = void (*)(void *, RawBehaviorParams &) noexcept;
using BehaviorDestroyPreparedParamsFn = void (*)(RawBehaviorParams &) noexcept;
using BehaviorCanonicalizeParamsFn = std::string (*)(const nlohmann::json &);
using BehaviorEventFn = void (*)(Behavior &, const void *, BehaviorContext &);
struct BehaviorEventHandlerRegistration {
    std::type_index event_type = std::type_index{typeid(void)};
    BehaviorEventFn dispatch = nullptr;
};
struct BehaviorRegistration {
    std::string stable_name;
    std::uint32_t schema_version = 0;
    std::type_index behavior_type = std::type_index{typeid(void)};
    std::type_index params_type = std::type_index{typeid(void)};
    std::vector<StructFieldSchema> params_schema;
    std::string params_schema_fingerprint;
    BehaviorCanonicalizeParamsFn canonicalize_params = nullptr;
    BehaviorCreateFn create = nullptr;
    BehaviorDestroyFn destroy = nullptr;
    BehaviorPrepareParamsFn prepare_params = nullptr;
    BehaviorApplyPreparedParamsFn apply_prepared_params = nullptr;
    BehaviorDestroyPreparedParamsFn destroy_prepared_params = nullptr;
    std::vector<BehaviorEventHandlerRegistration> event_handlers;
    RegistrationOwner owner = engineRegistrationOwner;
    RegistrationToken token;
};
class UserBehaviorRegistererTemplatePublic {
    PELICAN_API RegistrationToken
    pelican_register_behavior_v2(BehaviorRegistration registration);

  public:
    RegistrationToken registerFrozenV2(BehaviorRegistration registration) {
        return pelican_register_behavior_v2(std::move(registration));
    }
};
PELICAN_API UserBehaviorRegistererTemplatePublic &getBehaviorRegisterer();
} // namespace internal
} // namespace Pelican

namespace {

struct FrozenV2Params {
    std::uint32_t value = 7;
};

class FrozenV2Behavior final : public Pelican::Behavior {};

struct FrozenV2Event {
    std::uint32_t value = 0;
    static constexpr auto pelican_payload = Pelican::structFields(
        Pelican::eventPayloadPolicy,
        Pelican::required(Pelican::field<&FrozenV2Event::value>("value")));
    template <class Archive> void ref(Archive &archive) {
        archive.prop("value", value);
    }
};

struct FrozenV2Component {
    std::uint32_t value = 0;

    template <class Archive> void ref(Archive &archive) {
        archive.prop("value", value);
    }
};

} // namespace

DECLARE_COMPONENT(FrozenV2Component, 60);

namespace {

class FrozenV2ComponentRegistration final {
  public:
    FrozenV2ComponentRegistration() {
        (void)Pelican::internal::getComponentRegisterer()
            .registerComponent<FrozenV2Component>(
                "wp382a_frozen_v2_component");
    }
};

FrozenV2ComponentRegistration component_registration;

class FrozenV2BehaviorRegistration final {
  public:
    FrozenV2BehaviorRegistration() {
        using namespace Pelican::internal;
        (void)getBehaviorRegisterer().registerFrozenV2(
            BehaviorRegistration{
                .stable_name = "wp382a_frozen_v2_behavior",
                .schema_version = 1,
                .behavior_type = std::type_index{typeid(FrozenV2Behavior)},
                .params_type = std::type_index{typeid(FrozenV2Params)},
                .params_schema = {},
                .params_schema_fingerprint = "wp382a-frozen-v2",
                .canonicalize_params = [](const nlohmann::json &) {
                    return nlohmann::json{{"value", 7}}.dump();
                },
                .create = [](std::string_view) {
                    return RawBehaviorInstance{
                        .behavior = new FrozenV2Behavior,
                        .params = new FrozenV2Params,
                        .params_type = &typeid(FrozenV2Params),
                    };
                },
                .destroy = [](RawBehaviorInstance &instance) noexcept {
                    delete static_cast<FrozenV2Behavior *>(instance.behavior);
                    delete static_cast<FrozenV2Params *>(instance.params);
                    instance = {};
                },
                .prepare_params = [](std::string_view) {
                    return RawBehaviorParams{
                        .value = new FrozenV2Params,
                        .type = &typeid(FrozenV2Params),
                    };
                },
                .apply_prepared_params = [](void *live,
                                            RawBehaviorParams &prepared) noexcept {
                    *static_cast<FrozenV2Params *>(live) =
                        *static_cast<FrozenV2Params *>(prepared.value);
                },
                .destroy_prepared_params = [](RawBehaviorParams &prepared) noexcept {
                    delete static_cast<FrozenV2Params *>(prepared.value);
                    prepared = {};
                },
            });
    }
};

FrozenV2BehaviorRegistration behavior_registration;

} // namespace

PELICAN_REGISTER_EVENT(FrozenV2Event);
