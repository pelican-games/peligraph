#include "importrules.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

namespace {

constexpr std::string_view rules_schema = "pelican.import_rules";
constexpr int rules_version = 1;

const std::set<std::string, std::less<>> recipes{
    "atlas_pack",
    "extract_scene",
    "psd_layers",
};

std::string lowerAscii(std::string_view value) {
    std::string result{value};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

void requireKnownKeys(const nlohmann::json &object, const std::set<std::string, std::less<>> &known,
                      std::string_view context) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!known.contains(it.key())) {
            throw std::runtime_error(std::string{context} + " has unknown key '" + it.key() + "'");
        }
    }
}

void validateRelativePath(std::string_view value, std::string_view context) {
    if (value.empty()) {
        throw std::runtime_error(std::string{context} + " must not be empty");
    }
    if (value.find('\\') != std::string_view::npos) {
        throw std::runtime_error(std::string{context} + " must use forward slashes: " +
                                 std::string{value});
    }
    if (value.starts_with('/') || value.find("://") != std::string_view::npos) {
        throw std::runtime_error(std::string{context} + " must be project-relative: " +
                                 std::string{value});
    }
    const std::filesystem::path path{std::string{value}};
    if (path.is_absolute() || path.has_root_name()) {
        throw std::runtime_error(std::string{context} + " must be project-relative: " +
                                 std::string{value});
    }
    for (const auto &component : path) {
        if (component == "..") {
            throw std::runtime_error(std::string{context} + " must not escape the project: " +
                                     std::string{value});
        }
    }
}

void validateGlob(std::string_view pattern, std::string_view context) {
    validateRelativePath(pattern, context);
    std::size_t start = 0;
    while (start <= pattern.size()) {
        const auto end = pattern.find('/', start);
        const auto segment = pattern.substr(start, end == std::string_view::npos ? pattern.size() - start
                                                                                : end - start);
        if (segment.empty() || segment == ".") {
            throw std::runtime_error(std::string{context} + " has an empty or dot path segment: " +
                                     std::string{pattern});
        }
        if (segment.find("**") != std::string_view::npos && segment != "**") {
            throw std::runtime_error(std::string{context} +
                                     " may use '**' only as a complete path segment: " +
                                     std::string{pattern});
        }
        if (segment.find('[') != std::string_view::npos || segment.find(']') != std::string_view::npos) {
            throw std::runtime_error(std::string{context} + " does not support character classes: " +
                                     std::string{pattern});
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
}

void validateOptions(const ImportRecipe &recipe, std::string_view context) {
    if (!recipe.options.is_object()) {
        throw std::runtime_error(std::string{context} + " options must be an object");
    }
    if (recipe.name == "atlas_pack") {
        requireKnownKeys(recipe.options, {"max_size", "padding"}, std::string{context} + " options");
        if (const auto it = recipe.options.find("max_size"); it != recipe.options.end()) {
            if (!it->is_number_integer() || it->get<long long>() <= 0) {
                throw std::runtime_error(std::string{context} + " option max_size must be a positive integer");
            }
        }
        if (const auto it = recipe.options.find("padding"); it != recipe.options.end()) {
            if (!it->is_number_integer() || it->get<long long>() < 0) {
                throw std::runtime_error(std::string{context} + " option padding must be a non-negative integer");
            }
        }
        return;
    }
    requireKnownKeys(recipe.options, {}, std::string{context} + " options");
}

ImportRecipe parseRecipe(const nlohmann::json &value, std::string_view context,
                         bool allow_string) {
    ImportRecipe recipe;
    if (allow_string && value.is_string()) {
        recipe.name = value.get<std::string>();
    } else {
        if (!value.is_object()) {
            throw std::runtime_error(std::string{context} + " must be a recipe string, object, or null");
        }
        requireKnownKeys(value, {"recipe", "options"}, context);
        const auto recipe_it = value.find("recipe");
        if (recipe_it == value.end() || !recipe_it->is_string()) {
            throw std::runtime_error(std::string{context} + " requires string recipe");
        }
        recipe.name = recipe_it->get<std::string>();
        if (const auto options = value.find("options"); options != value.end()) {
            recipe.options = *options;
        }
    }
    if (!recipes.contains(recipe.name)) {
        throw std::runtime_error(std::string{context} + " has unknown recipe '" + recipe.name + "'");
    }
    validateOptions(recipe, context);
    return recipe;
}

std::vector<std::string_view> splitPath(std::string_view path) {
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        segments.push_back(path.substr(start, end == std::string_view::npos ? path.size() - start : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return segments;
}

bool segmentMatches(std::string_view pattern, std::string_view value) {
    std::vector<std::vector<bool>> dp(pattern.size() + 1,
                                      std::vector<bool>(value.size() + 1, false));
    dp[0][0] = true;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        for (std::size_t j = 0; j <= value.size(); ++j) {
            if (!dp[i][j]) {
                continue;
            }
            if (pattern[i] == '*') {
                dp[i + 1][j] = true;
                if (j < value.size()) {
                    dp[i][j + 1] = true;
                }
            } else if (pattern[i] == '?') {
                if (j < value.size()) {
                    dp[i + 1][j + 1] = true;
                }
            } else if (j < value.size() && pattern[i] == value[j]) {
                dp[i + 1][j + 1] = true;
            }
        }
    }
    return dp[pattern.size()][value.size()];
}

ImportRecipe builtinForExtension(std::string_view extension) {
    if (extension == ".glb") {
        return {"extract_scene", nlohmann::json::object()};
    }
    if (extension == ".psd") {
        return {"psd_layers", nlohmann::json::object()};
    }
    if (extension == ".png") {
        return {"atlas_pack", nlohmann::json::object()};
    }
    return {};
}

} // namespace

ImportRules parseImportRulesJson(const nlohmann::json &document) {
    if (!document.is_object()) {
        throw std::runtime_error("import rules document must be an object");
    }
    requireKnownKeys(document, {"schema", "version", "rules", "defaults"}, "import rules document");
    if (document.value("schema", std::string{}) != rules_schema) {
        throw std::runtime_error("import rules schema must be 'pelican.import_rules'");
    }
    if (!document.contains("version") || !document.at("version").is_number_integer() ||
        document.at("version").get<int>() != rules_version) {
        throw std::runtime_error("import rules version must be exactly 1");
    }
    if (!document.contains("rules") || !document.at("rules").is_array()) {
        throw std::runtime_error("import rules document requires rules array");
    }

    ImportRules parsed;
    for (std::size_t i = 0; i < document.at("rules").size(); ++i) {
        const auto &value = document.at("rules").at(i);
        const auto context = "import rules rules[" + std::to_string(i) + "]";
        if (!value.is_object()) {
            throw std::runtime_error(context + " must be an object");
        }
        requireKnownKeys(value, {"match", "recipe", "options"}, context);
        const auto match_it = value.find("match");
        if (match_it == value.end() || !match_it->is_string()) {
            throw std::runtime_error(context + " requires string match");
        }
        const auto recipe_it = value.find("recipe");
        if (recipe_it == value.end() || !recipe_it->is_string()) {
            throw std::runtime_error(context + " requires string recipe");
        }
        const auto match = match_it->get<std::string>();
        validateGlob(match, context + " match");
        nlohmann::json recipe_value{{"recipe", *recipe_it}};
        if (const auto options = value.find("options"); options != value.end()) {
            recipe_value["options"] = *options;
        }
        parsed.rules.push_back({match, parseRecipe(recipe_value, context, false)});
    }

    if (const auto defaults = document.find("defaults"); defaults != document.end()) {
        if (!defaults->is_object()) {
            throw std::runtime_error("import rules defaults must be an object");
        }
        for (auto it = defaults->begin(); it != defaults->end(); ++it) {
            auto extension = lowerAscii(it.key());
            if (extension.empty() || extension.front() != '.' || extension.size() == 1 ||
                extension.find_first_of("/*?\\") != std::string::npos) {
                throw std::runtime_error("import rules defaults key must be a .extension: " + it.key());
            }
            if (it->is_null()) {
                parsed.defaults.emplace(std::move(extension), std::nullopt);
            } else {
                parsed.defaults.emplace(std::move(extension),
                                        parseRecipe(*it, "import rules defaults['" + it.key() + "']", true));
            }
        }
    }
    return parsed;
}

bool importGlobMatches(std::string_view pattern, std::string_view project_relative_file) {
    const auto patterns = splitPath(pattern);
    const auto values = splitPath(project_relative_file);
    std::vector<std::vector<bool>> dp(patterns.size() + 1,
                                      std::vector<bool>(values.size() + 1, false));
    dp[0][0] = true;
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        for (std::size_t j = 0; j <= values.size(); ++j) {
            if (!dp[i][j]) {
                continue;
            }
            if (patterns[i] == "**") {
                dp[i + 1][j] = true;
                if (j < values.size()) {
                    dp[i][j + 1] = true;
                }
            } else if (j < values.size() && segmentMatches(patterns[i], values[j])) {
                dp[i + 1][j + 1] = true;
            }
        }
    }
    return dp[patterns.size()][values.size()];
}

std::optional<ImportRuleMatch> evaluateImportRules(const ImportRules &rules,
                                                   std::string_view project_relative_file) {
    validateRelativePath(project_relative_file, "import candidate path");
    for (std::size_t i = 0; i < rules.rules.size(); ++i) {
        if (importGlobMatches(rules.rules[i].match, project_relative_file)) {
            return ImportRuleMatch{rules.rules[i].recipe, ImportRuleLayer::rule, i, {}};
        }
    }

    const auto extension = lowerAscii(std::filesystem::path{std::string{project_relative_file}}.extension().string());
    if (const auto found = rules.defaults.find(extension); found != rules.defaults.end()) {
        if (!found->second) {
            return std::nullopt;
        }
        return ImportRuleMatch{*found->second, ImportRuleLayer::defaults, std::nullopt, extension};
    }
    auto builtin = builtinForExtension(extension);
    if (builtin.name.empty()) {
        return std::nullopt;
    }
    return ImportRuleMatch{std::move(builtin), ImportRuleLayer::builtin, std::nullopt, extension};
}

} // namespace Pelican
