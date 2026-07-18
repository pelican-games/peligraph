#include "../../../src/core/userpublic/details/schema/structfieldschema.hpp"

enum class Mode { Idle, Run };

struct BehaviorFields {
    bool enabled = false;
    Mode mode = Mode::Idle;

    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&BehaviorFields::enabled>("enabled"), true),
        Pelican::defaulted(
            Pelican::field<&BehaviorFields::mode>("mode"), Mode::Idle,
            Pelican::enumValues(Pelican::enumValue("idle", Mode::Idle),
                                Pelican::enumValue("run", Mode::Run))));
};

static_assert(BehaviorFields::schema.fields[0].type == Pelican::StructFieldType::Bool);
static_assert(BehaviorFields::schema.fields[1].type == Pelican::StructFieldType::Enum);
static_assert(BehaviorFields::schema.presence[0] == Pelican::StructFieldPresence::Defaulted);

int main() {}
