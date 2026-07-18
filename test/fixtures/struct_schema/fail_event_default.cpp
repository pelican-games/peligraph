#include "../../../src/core/userpublic/details/schema/structfieldschema.hpp"

struct BadEventSchema {
    int value = 0;
    static constexpr auto schema = Pelican::structFields(
        Pelican::eventPayloadPolicy,
        Pelican::defaulted(Pelican::field<&BadEventSchema::value>("value"), 0));
};

int main() {}
