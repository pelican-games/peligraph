#include "../../../src/core/userpublic/details/event/payloadschema.hpp"

struct BadEvent {
    float value = 0;
    static constexpr auto pelican_payload =
        Pelican::payloadFields(Pelican::field<&BadEvent::value>("value", Pelican::frange(0.0, 1.0e300)));
};

int main() {}
