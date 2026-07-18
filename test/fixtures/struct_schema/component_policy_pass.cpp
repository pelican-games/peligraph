#include "../../../src/core/userpublic/details/schema/structfieldschema.hpp"

struct ComponentFields {
    float required_value = 0.0f;
    float defaulted_value = 1.0f;

    static constexpr auto schema = Pelican::structFields(
        Pelican::componentPolicy("fixture_component"),
        Pelican::required(Pelican::field<&ComponentFields::required_value>("required_value")),
        Pelican::defaulted(Pelican::field<&ComponentFields::defaulted_value>("defaulted_value"), 1.0f));
};

static_assert(ComponentFields::schema.presence[0] == Pelican::StructFieldPresence::Required);
static_assert(ComponentFields::schema.presence[1] == Pelican::StructFieldPresence::Defaulted);
static_assert(ComponentFields::schema.policy.codecName() == "fixture_component");

int main() {}
