#pragma once

#include <schema.hpp>

#include <nlohmann/json.hpp>

#include <string_view>

namespace Pelican {

// Project/offline ingestion of the schema_fields wire shape. Vocabulary and
// declaration rules remain owned by pelican_schema_leaf.
Schema::FieldDeclaration parseSchemaFieldDeclaration(
    const nlohmann::json &wire, std::string_view path = "schema field");

} // namespace Pelican

