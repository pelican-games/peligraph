#include "../../../src/core/userpublic/details/event/payloadschema.hpp"

struct BadEvent {
    int value = 0;
    static constexpr auto pelican_payload = Pelican::payloadFields(Pelican::field<&BadEvent::value>(""));
};

int main() {}
