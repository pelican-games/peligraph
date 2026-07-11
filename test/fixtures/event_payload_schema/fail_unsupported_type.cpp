#include "../../../src/core/userpublic/details/event/payloadschema.hpp"

struct BadEvent {
    bool value = false;
    static constexpr auto pelican_payload = Pelican::payloadFields(Pelican::field<&BadEvent::value>("value"));
};

int main() {}
