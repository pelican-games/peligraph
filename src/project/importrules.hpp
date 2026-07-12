#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct ImportRecipe {
    std::string name;
    nlohmann::json options = nlohmann::json::object();
};

struct ImportRule {
    std::string match;
    ImportRecipe recipe;
};

struct ImportRules {
    std::vector<ImportRule> rules;
    std::map<std::string, std::optional<ImportRecipe>> defaults;
};

enum class ImportRuleLayer {
    rule,
    defaults,
    builtin,
};

struct ImportRuleMatch {
    ImportRecipe recipe;
    ImportRuleLayer layer = ImportRuleLayer::builtin;
    std::optional<std::size_t> rule_index;
    std::string default_extension;
};

ImportRules parseImportRulesJson(const nlohmann::json &document);
std::optional<ImportRuleMatch> evaluateImportRules(const ImportRules &rules,
                                                   std::string_view project_relative_file);
bool importGlobMatches(std::string_view pattern, std::string_view project_relative_file);

} // namespace Pelican
