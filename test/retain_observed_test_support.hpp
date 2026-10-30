#pragma once

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::TestSupport {
// Declare only the resources this fixture intends to read, before compilation.
// Merge these leaves without overwriting policy. The runtime audit below checks
// the declarations against successful reads, independently of this list.
template<class Json>
void retainObserved(Json &root, std::string_view graph, const std::vector<std::string> &names) {
    const std::set<std::string> expected(names.begin(), names.end());
    if (expected.size() != names.size()) throw std::logic_error("duplicate readback observation");
    auto &planning = root["rendering"]["target_planning"];
    auto &resources = planning["graphs"][std::string{graph}]["resources"];
    for (const auto &name : names) {
        auto &resource = resources[std::string{name}];
        if (resource.contains("retain") && resource.at("retain") != true)
            throw std::logic_error("retainObserved would overwrite existing policy: " + std::string{name});
        resource["retain"] = true;
    }
    std::set<std::string> retained;
    for (const auto &[name, resource] : resources.items())
        if (resource.contains("retain") && resource.at("retain") == true) retained.insert(name);
    if (retained != expected) throw std::logic_error("retainObserved would include undeclared resources");
}
template<class Json>
void validateRetainObservations(const Json &root, std::string_view graph,
                               const std::vector<std::string> &successful) {
    std::set<std::string> declared;
    for (const auto &[name, resource] : root.at("rendering").at("target_planning").at("graphs").at(graph).at("resources").items())
        if (resource.contains("retain") && resource.at("retain") == true) declared.insert(name);
    if (declared != std::set<std::string>(successful.begin(), successful.end()))
        throw std::logic_error("declared retain differs from successful readbacks");
}

// Shared by every test translation unit. A successful read is the only way
// to add to observed; the declaration list never supplies observations.
struct RetainObservationAudit {
    struct Key {
        std::string fixture, graph;
        int variant = 0;
        auto operator<=>(const Key &) const = default;
    };
    struct Entry { std::set<std::string> declared, observed; };
    std::map<Key, Entry> entries;
    std::uint64_t epoch = 0;
    void reset() { entries.clear(); ++epoch; }
    void declare(const Key &key, const std::set<std::string> &declared) {
        auto [it, inserted] = entries.try_emplace(key, Entry{declared, {}});
        if (!inserted && it->second.declared != declared)
            throw std::logic_error("fixture retain declarations changed without a new fixture identity");
    }
    void record(const Key &key, const std::set<std::string> &declared, std::string_view resource) {
        declare(key, declared);
        entries.at(key).observed.emplace(resource);
    }
    std::vector<std::string> mismatches() const {
        std::vector<std::string> result;
        for (const auto &[key, entry] : entries) if (entry.declared != entry.observed)
            result.push_back(nlohmann::json{{"fixture",key.fixture},{"graph",key.graph},{"variant",key.variant},
                {"declared_retain",entry.declared},{"successful_readbacks",entry.observed}}.dump());
        return result;
    }
};
inline RetainObservationAudit retainObservationAudit;
// Core-backed test access installs this hook. The generic Catch listener also
// runs in project-only executables, where there is no renderer to inspect.
inline void (*declareActiveRetainFixture)() = nullptr;
inline void retainObservedFile(const std::filesystem::path &path, std::string_view graph,
                               const std::vector<std::string> &names) {
    auto root = nlohmann::ordered_json::parse(std::ifstream{path});
    retainObserved(root, graph, names);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << root.dump(2);
    if (!out) throw std::runtime_error("cannot write observed fixture " + path.string());
}
} // namespace Pelican::TestSupport
