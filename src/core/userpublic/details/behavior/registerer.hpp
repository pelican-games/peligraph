#pragma once

#include "../../behavior.hpp"
#include "../event/registerer.hpp"
#include "../reload/registrationowner.hpp"
#include "../schema/structfieldjson.hpp"

#include <concepts>
#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

namespace Pelican::internal {

struct RawBehaviorInstance {
    Behavior *behavior = nullptr;
    void *params = nullptr;
    const std::type_info *params_type = &typeid(void);
};

using BehaviorCreateFn = RawBehaviorInstance (*)(std::string_view canonical_params);
using BehaviorDestroyFn = void (*)(RawBehaviorInstance &instance) noexcept;
using BehaviorCanonicalizeParamsFn = std::string (*)(const nlohmann::json &params);
using BehaviorEventFn = void (*)(Behavior &behavior, const void *event,
                                 BehaviorContext &ctx);

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
    std::vector<BehaviorEventHandlerRegistration> event_handlers;
    RegistrationOwner owner = engineRegistrationOwner;
};

template <class Type, class Event>
concept HasBehaviorEvent = requires(Type &behavior, const Event &event,
                                    BehaviorContext &ctx) {
    { behavior.onEvent(event, ctx) } -> std::same_as<void>;
};

template <class Type, class Event>
void appendBehaviorEventHandler(
    std::vector<BehaviorEventHandlerRegistration> &handlers) {
    if constexpr (HasBehaviorEvent<Type, Event>) {
        handlers.push_back(BehaviorEventHandlerRegistration{
            .event_type = std::type_index{typeid(Event)},
            .dispatch = [](Behavior &behavior, const void *event,
                           BehaviorContext &ctx) {
                static_cast<Type &>(behavior).onEvent(
                    *static_cast<const Event *>(event), ctx);
            },
        });
    }
}

template <class Type, int Index, class Lookup>
void appendBehaviorEventHandlerAt(
    std::vector<BehaviorEventHandlerRegistration> &handlers, Lookup lookup) {
    if constexpr (requires {
                      typename decltype(lookup.template operator()<Index>())::type;
                  }) {
        using Event = typename decltype(lookup.template operator()<Index>())::type;
        appendBehaviorEventHandler<Type, Event>(handlers);
    }
}

template <class Type, class Lookup, int... Indices>
std::vector<BehaviorEventHandlerRegistration> collectBehaviorEventHandlers(
    std::integer_sequence<int, Indices...>, Lookup lookup) {
    std::vector<BehaviorEventHandlerRegistration> handlers;
    (appendBehaviorEventHandlerAt<Type, Indices>(handlers, lookup), ...);
    return handlers;
}

template <class Params>
std::vector<StructFieldSchema> materializeBehaviorParamsSchema() {
    static_assert(
        std::same_as<typename decltype(Params::schema)::policy_type,
                     BehaviorParamsPolicy>,
        "Behavior Params::schema must use BehaviorParamsPolicy");
    std::vector<StructFieldSchema> fields;
    fields.reserve(Params::schema.fields.size());
    for (const auto &field : Params::schema.fields) {
        fields.push_back(materializeStructField(field));
    }
    return fields;
}

inline nlohmann::ordered_json behaviorParamsRangeFingerprint(
    const StructFieldRange &range) {
    return std::visit(
        [](const auto &value) -> nlohmann::ordered_json {
            using Range = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<Range, std::monostate>) {
                return nullptr;
            } else {
                return nlohmann::ordered_json::array({value.first, value.second});
            }
        },
        range);
}

template <class Params>
std::string materializeBehaviorParamsSchemaFingerprint() {
    static_assert(
        std::same_as<typename decltype(Params::schema)::policy_type,
                     BehaviorParamsPolicy>,
        "Behavior Params::schema must use BehaviorParamsPolicy");

    Params defaults{};
    decodeBehaviorParams(nlohmann::json::object(), defaults, Params::schema);
    const auto encoded_defaults = encodeBehaviorParams(defaults, Params::schema);
    nlohmann::ordered_json fields = nlohmann::ordered_json::array();
    std::apply(
        [&](const auto &...policy_field) {
            ([&] {
                using PolicyField = std::remove_cvref_t<decltype(policy_field)>;
                using Traits = PolicyFieldTraits<PolicyField>;
                const auto descriptor = materializeStructField(
                    policy_field.field.descriptor);
                nlohmann::ordered_json field{
                    {"name", descriptor.name},
                    {"type", static_cast<std::uint8_t>(descriptor.type)},
                    {"range", behaviorParamsRangeFingerprint(descriptor.range)},
                    {"unit", descriptor.unit},
                    {"presence", static_cast<std::uint8_t>(Traits::presence)},
                    {"default", encoded_defaults.at(descriptor.name)},
                };
                if constexpr (Traits::has_enum_values) {
                    nlohmann::ordered_json values = nlohmann::ordered_json::array();
                    for (const auto &entry : policy_field.enum_values.values) {
                        using Enum = std::remove_cvref_t<decltype(entry.value)>;
                        using Underlying = std::underlying_type_t<Enum>;
                        if constexpr (std::is_signed_v<Underlying>) {
                            values.push_back(nlohmann::ordered_json{
                                {"name", entry.name},
                                {"value", static_cast<std::int64_t>(entry.value)},
                            });
                        } else {
                            values.push_back(nlohmann::ordered_json{
                                {"name", entry.name},
                                {"value", static_cast<std::uint64_t>(entry.value)},
                            });
                        }
                    }
                    field["enum_values"] = std::move(values);
                }
                fields.push_back(std::move(field));
            }(),
             ...);
        },
        Params::schema.declarations);
    return fields.dump();
}

class UserBehaviorRegistererTemplatePublic {
    std::vector<BehaviorRegistration> behaviors;

    PELICAN_API void __registerBehavior(BehaviorRegistration registration);

    friend void unregisterBehaviors(RegistrationOwner owner) noexcept;
    friend std::size_t behaviorRegistrationCount(RegistrationOwner owner) noexcept;

  public:
    template <class Type>
    void registerBehavior(
        std::string stable_name, std::uint32_t schema_version,
        std::vector<BehaviorEventHandlerRegistration> event_handlers) {
        static_assert(std::derived_from<Type, Behavior>,
                      "registered behavior must derive from Pelican::Behavior");
        static_assert(std::default_initializable<Type>,
                      "registered behavior must be default initializable");
        static_assert(requires { typename Type::Params; Type::Params::schema; },
                      "registered behavior must declare Params with Params::schema");
        using Params = typename Type::Params;
        static_assert(std::default_initializable<Params>,
                      "Behavior Params must be default initializable");

        if (stable_name.empty()) {
            throw std::runtime_error("behavior stable name must not be empty");
        }
        if (schema_version == 0) {
            throw std::runtime_error("behavior schema version must be positive");
        }

        __registerBehavior(BehaviorRegistration{
            .stable_name = std::move(stable_name),
            .schema_version = schema_version,
            .behavior_type = std::type_index{typeid(Type)},
            .params_type = std::type_index{typeid(Params)},
            .params_schema = materializeBehaviorParamsSchema<Params>(),
            .params_schema_fingerprint =
                materializeBehaviorParamsSchemaFingerprint<Params>(),
            .canonicalize_params = [](const nlohmann::json &params) -> std::string {
                Params decoded{};
                decodeBehaviorParams(params, decoded, Params::schema);
                return encodeBehaviorParams(decoded, Params::schema).dump();
            },
            .create = [](std::string_view canonical_params) -> RawBehaviorInstance {
                auto params = std::make_unique<Params>();
                decodeBehaviorParams(nlohmann::json::parse(canonical_params), *params,
                                     Params::schema);
                auto behavior = std::make_unique<Type>();
                RawBehaviorInstance result{
                    .behavior = behavior.release(),
                    .params = params.release(),
                    .params_type = &typeid(Params),
                };
                return result;
            },
            .destroy = [](RawBehaviorInstance &instance) noexcept {
                delete static_cast<Type *>(instance.behavior);
                delete static_cast<Params *>(instance.params);
                instance = {};
            },
            .event_handlers = std::move(event_handlers),
        });
    }

    const std::vector<BehaviorRegistration> &registeredBehaviors() const noexcept {
        return behaviors;
    }

    const BehaviorRegistration *findByName(std::string_view stable_name) const noexcept;
    const BehaviorRegistration *findByNameAndOwner(
        std::string_view stable_name, RegistrationOwner owner) const noexcept;
};

PELICAN_API UserBehaviorRegistererTemplatePublic &getBehaviorRegisterer();
void unregisterBehaviors(RegistrationOwner owner) noexcept;
std::size_t behaviorRegistrationCount(RegistrationOwner owner) noexcept;
std::string canonicalizeBehaviorParams(std::string_view stable_name,
                                       const nlohmann::json &params);
void validateBehaviorReload(RegistrationOwner active_owner,
                            RegistrationOwner candidate_owner,
                            const nlohmann::json *authoring_scenes);

} // namespace Pelican::internal

#ifndef PELICAN_DETAIL_CONCAT_INNER
#define PELICAN_DETAIL_CONCAT_INNER(a, b) a##b
#endif
#ifndef PELICAN_DETAIL_CONCAT
#define PELICAN_DETAIL_CONCAT(a, b) PELICAN_DETAIL_CONCAT_INNER(a, b)
#endif

#define PELICAN_REGISTER_BEHAVIOR_IMPL(Type, stable_name, schema_version, unique_id)                 \
    namespace {                                                                                      \
    struct PELICAN_DETAIL_CONCAT(PelicanBehaviorAutoRegister_, unique_id) {                          \
        PELICAN_DETAIL_CONCAT(PelicanBehaviorAutoRegister_, unique_id)() {                           \
            auto event_handlers =                                                                    \
                ::Pelican::internal::collectBehaviorEventHandlers<Type>(                             \
                    std::make_integer_sequence<int, unique_id>{},                                    \
                    []<int Index>()                                                                   \
                        -> decltype(pelicanEventCatalogEntry(                                         \
                            ::Pelican::internal::EventCatalogTag<Index>{})) {                         \
                        return {};                                                                    \
                    });                                                                               \
            ::Pelican::internal::getBehaviorRegisterer().registerBehavior<Type>(                     \
                stable_name, schema_version, std::move(event_handlers));                             \
        }                                                                                             \
    };                                                                                                \
    static const PELICAN_DETAIL_CONCAT(PelicanBehaviorAutoRegister_, unique_id)                      \
        PELICAN_DETAIL_CONCAT(pelican_behavior_auto_register_, unique_id);                           \
    }

#define PELICAN_REGISTER_BEHAVIOR(Type, stable_name, schema_version)                                 \
    PELICAN_REGISTER_BEHAVIOR_IMPL(Type, stable_name, schema_version, __COUNTER__)
