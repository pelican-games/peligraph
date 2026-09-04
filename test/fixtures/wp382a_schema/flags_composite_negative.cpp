#include <details/schema/structfieldjson.hpp>

enum class BadFlag : unsigned { A = 1, AB = 3 };
struct Params {
    Pelican::Flags<BadFlag> flags;
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(
            Pelican::field<&Params::flags>("flags"),
            Pelican::Flags<BadFlag>{},
            Pelican::enumValues(Pelican::enumValue("A", BadFlag::A),
                                Pelican::enumValue("AB", BadFlag::AB))));
};

static_assert(Params::schema.fields.size() == 1);
