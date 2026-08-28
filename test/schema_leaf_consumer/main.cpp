#include <schema.hpp>

#include <iostream>
#include <string>

int main() {
    using namespace Pelican::Schema;
    if (allFieldTypes.size() != 17 || typeRules().size() != 17) return 1;
    for (const auto type : allFieldTypes) {
        const auto name = fieldTypeName(type);
        if (fieldTypeFromName(name) != type) return 2;
    }
    FieldDeclaration i8{
        .name = "i8",
        .type = FieldType::I8,
        .default_value = -8,
    };
    const auto canonical = resolveObject(std::span{&i8, std::size_t{1}},
                                         nlohmann::json::object());
    if (canonical.dump() != R"({"i8":-8})") return 3;
    std::cout << "WP358_LEAF_CONSUMER=17 core_include_dirs=0\n";
    return 0;
}

