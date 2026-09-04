#include <details/behavior/registerer.hpp>

#include <cstddef>

// Same fields and (on the supported ABI) the same total size, but two
// same-sized type_index fields are swapped.  Size-only freezing misses this.
struct ReorderedBehaviorRegistrationV2 {
    std::string stable_name;
    std::uint32_t schema_version;
    std::type_index params_type;
    std::type_index behavior_type;
    std::vector<Pelican::StructFieldSchema> params_schema;
    std::string params_schema_fingerprint;
    Pelican::internal::BehaviorCanonicalizeParamsFn canonicalize_params;
    Pelican::internal::BehaviorCreateFn create;
    Pelican::internal::BehaviorDestroyFn destroy;
    Pelican::internal::BehaviorPrepareParamsFn prepare_params;
    Pelican::internal::BehaviorApplyPreparedParamsFn apply_prepared_params;
    Pelican::internal::BehaviorDestroyPreparedParamsFn destroy_prepared_params;
    std::vector<Pelican::internal::BehaviorEventHandlerRegistration> event_handlers;
    Pelican::internal::RegistrationOwner owner;
    Pelican::internal::RegistrationToken token;
};

static_assert(sizeof(ReorderedBehaviorRegistrationV2) ==
              sizeof(Pelican::internal::BehaviorRegistration));
static_assert(offsetof(ReorderedBehaviorRegistrationV2, params_type) ==
                  offsetof(Pelican::internal::BehaviorRegistration,
                           params_type),
              "FrozenV2FieldOffsetMismatch");
