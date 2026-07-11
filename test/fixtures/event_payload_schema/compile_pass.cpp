#include "../../../src/core/userpublic/details/event/payloadschema.hpp"

#include <cstdint>

struct PassEvent {
    std::int32_t count = 0;
    float speed = 0;
    static constexpr auto pelican_payload = Pelican::payloadFields(
        Pelican::field<&PassEvent::count>("count", Pelican::irange(0, 10)),
        Pelican::field<&PassEvent::speed>("speed", Pelican::frange(0.0, 100.0), "m/s"));
};

static_assert(PassEvent::pelican_payload.fields.size() == 2);
static_assert(PassEvent::pelican_payload.fields[1].type == Pelican::PayloadFieldType::F32);

int main() {
    return PassEvent::pelican_payload.fields[0].name == "count" ? 0 : 1;
}
