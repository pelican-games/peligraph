#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace Pelican {

// Retain existing tokens, member order, BOM and line endings. Changed/new
// containers use two spaces; new members follow the existing members.
std::string renderJsonDocument(const nlohmann::ordered_json &document,
                               std::string_view original_bytes);

// A value inserted into a larger document inherits that document's line
// ending and nesting depth, but has neither a BOM nor a trailing newline.
std::string renderJsonDocumentValue(const nlohmann::ordered_json &value,
                                    std::string_view original_value,
                                    std::string_view enclosing_document,
                                    std::size_t depth);

} // namespace Pelican
