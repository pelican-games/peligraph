#include "../../../src/core/userpublic/details/schema/structfieldschema.hpp"

enum class Mode { Idle, Run };

struct BadBehaviorSchema {
    Mode mode = Mode::Idle;
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&BadBehaviorSchema::mode>("mode"), Mode::Idle));
};

int main() {}
