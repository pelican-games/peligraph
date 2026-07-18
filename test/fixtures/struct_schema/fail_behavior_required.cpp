#include "../../../src/core/userpublic/details/schema/structfieldschema.hpp"

struct BadBehaviorSchema {
    float speed = 1.0f;
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::required(Pelican::field<&BadBehaviorSchema::speed>("speed")));
};

int main() {}
