#include "../../../src/core/userpublic/details/schema/structfieldschema.hpp"

#include <cstdint>
#include <type_traits>

struct EventFields {
    std::int32_t count = 0;

    static constexpr auto schema = Pelican::structFields(
        Pelican::eventPayloadPolicy,
        Pelican::required(Pelican::field<&EventFields::count>("count", Pelican::irange(0, 10))));
};

static_assert(std::same_as<typename decltype(EventFields::schema)::policy_type,
                           Pelican::EventPayloadPolicy>);
static_assert(EventFields::schema.presence[0] == Pelican::StructFieldPresence::Required);

int main() {
    return EventFields::schema.fields[0].name == "count" ? 0 : 1;
}
