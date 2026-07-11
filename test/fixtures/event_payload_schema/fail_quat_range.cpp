#include "../../../src/core/userpublic/details/event/payloadschema.hpp"

struct BadEvent {
    Pelican::quat value{};
    static constexpr auto pelican_payload =
        Pelican::payloadFields(Pelican::field<&BadEvent::value>("value", Pelican::frange(0.0, 1.0)));
};

int main() {}
