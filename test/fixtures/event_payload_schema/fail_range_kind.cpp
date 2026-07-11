#include "../../../src/core/userpublic/details/event/payloadschema.hpp"

struct BadEvent {
    std::string value;
    static constexpr auto pelican_payload =
        Pelican::payloadFields(Pelican::field<&BadEvent::value>("value", Pelican::irange(0, 1)));
};

int main() {}
