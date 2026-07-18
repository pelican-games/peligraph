#include "../../../src/core/userpublic/details/schema/structfieldschema.hpp"

struct BadComponentSchema {
    int value = 0;
    static constexpr auto schema = Pelican::structFields(
        Pelican::componentPolicy("bad_component"),
        Pelican::field<&BadComponentSchema::value>("value"));
};

int main() {}
