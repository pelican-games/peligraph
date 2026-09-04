#include <details/schema/structfieldjson.hpp>

enum class BadFlag : unsigned { None = 0, A = 1 };
struct Params {
    Pelican::Flags<BadFlag> flags;
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(
            Pelican::field<&Params::flags>("flags"),
            Pelican::Flags<BadFlag>{},
            Pelican::enumValues(Pelican::enumValue("None", BadFlag::None),
                                Pelican::enumValue("A", BadFlag::A))));
};

static_assert(Params::schema.fields.size() == 1);
