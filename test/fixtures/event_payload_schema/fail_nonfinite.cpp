#include "../../../src/core/userpublic/details/event/payloadschema.hpp"

#include <limits>

struct BadEvent {
    double value = 0;
    static constexpr auto pelican_payload = Pelican::payloadFields(Pelican::field<&BadEvent::value>(
        "value", Pelican::frange(0.0, std::numeric_limits<double>::infinity())));
};

int main() {}
