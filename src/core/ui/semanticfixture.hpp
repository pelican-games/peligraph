#pragma once

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::ui {

struct FixtureIssue {
    std::string clause;
    std::string path;
    std::string message;
    auto operator<=>(const FixtureIssue &) const = default;
};

struct FixtureValidation {
    std::vector<FixtureIssue> schema_issues;
    std::vector<FixtureIssue> semantic_issues;
    bool schemaValid() const noexcept { return schema_issues.empty(); }
    bool semanticValid() const noexcept { return schema_issues.empty() && semantic_issues.empty(); }
    bool failedClause(std::string_view clause) const noexcept;
};

std::vector<FixtureIssue> validateSemanticFixtureSchema(const nlohmann::json &fixture);
std::vector<FixtureIssue> validateSemanticFixture(const nlohmann::json &fixture);
FixtureValidation validateSemanticFixtureAll(const nlohmann::json &fixture);
bool semanticFixtureEqual(const nlohmann::json &actual, const nlohmann::json &expected);
std::string writeSemanticFixture(const nlohmann::json &fixture);

struct CoverageGateResult {
    std::size_t checked_fixtures = 0;
    std::size_t checked_entries = 0;
    std::vector<std::string> failures;
    explicit operator bool() const noexcept { return failures.empty(); }
};

CoverageGateResult runSemanticCoverageGate(const std::filesystem::path &repo_root);

} // namespace Pelican::ui
