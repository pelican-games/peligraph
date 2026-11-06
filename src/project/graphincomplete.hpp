#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "graphmodule.hpp"

namespace Pelican {

struct GraphIncompleteInput {
    std::string scope;
    std::string node;
    std::string port;
    std::string substitute;
};

struct GraphIncompleteResult {
    nlohmann::json document;
    std::vector<GraphIncompleteInput> inputs;
};

GraphIncompleteResult completeUnconnectedInputs(nlohmann::json authoring_document,
                                                const GraphModuleRegistry &modules);

nlohmann::json graphIncompleteInputsJson(const std::vector<GraphIncompleteInput> &inputs);
std::string graphIncompleteDiagnostic(nlohmann::json authoring_document, std::string_view diagnostic);

} // namespace Pelican
