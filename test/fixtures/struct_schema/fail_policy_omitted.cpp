#include "../../../src/core/userpublic/details/schema/structfieldschema.hpp"

struct BadSchema {
    int value = 0;
    static constexpr auto schema =
        Pelican::structFields(Pelican::required(Pelican::field<&BadSchema::value>("value")));
};

int main() {}
