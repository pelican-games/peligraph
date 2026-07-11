#include "../../../src/core/userpublic/details/event/payloadschema.hpp"

struct BadEvent {
    int first = 0;
    int second = 0;
    static constexpr auto pelican_payload = Pelican::payloadFields(
        Pelican::field<&BadEvent::first>("same"), Pelican::field<&BadEvent::second>("same"));
};

int main() {}
