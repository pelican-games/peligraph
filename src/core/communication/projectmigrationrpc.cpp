#include "projectmigrationrpc.hpp"

#include "rpcserver.hpp"

#include "../../project/projectmigration.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace Pelican {
namespace {

using OrderedJson = nlohmann::ordered_json;

constexpr std::string_view restartRequiredCode =
    "project.migration_restart_required@1";

[[noreturn]] void throwMigrationRpcError(const ProjectMigrationError &error) {
    throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError, error.what(),
                              OrderedJson{{"error_code", error.code()},
                                          {"error_fields", error.fields()}},
                              PreserveJsonRpcDataOrder{});
}

std::map<std::string, std::string> expectedDigestsParam(const nlohmann::json &params) {
    constexpr auto method = "apply_project_migration";
    if (!params.is_object() || !params.contains("expected") ||
        !params.at("expected").is_object()) {
        throw JsonRpcHandlerError(
            JsonRpcErrorCodes::invalidParams,
            std::string{method} +
                " params requires object field 'expected' ({path: digest}, "
                "as plan_project_migration returned it)");
    }
    std::map<std::string, std::string> digests;
    for (const auto &[path, digest] : params.at("expected").items()) {
        if (!digest.is_string()) {
            throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams,
                                      std::string{method} +
                                          " expected digests must be strings");
        }
        digests.emplace(path, digest.get<std::string>());
    }
    return digests;
}

void configureHandlers(RpcServer &server, std::filesystem::path project_root,
                       const DocumentMigrationRegistry &registry,
                       std::function<void()> on_applied) {
    server.setOrderedHandler(
        "plan_project_migration",
        [project_root, &registry](const nlohmann::json &params) {
            if (!params.is_null() && !params.is_object()) {
                throw JsonRpcHandlerError(
                    JsonRpcErrorCodes::invalidParams,
                    "plan_project_migration params must be an object");
            }
            try {
                return planProjectMigration(project_root, registry).toJson();
            } catch (const ProjectMigrationError &error) {
                throwMigrationRpcError(error);
            }
        },
        [](const OrderedJson &) { return true; });
    server.setOrderedHandler(
        "apply_project_migration",
        [project_root, &registry,
         on_applied = std::move(on_applied)](const nlohmann::json &params) {
            ProjectMigrationApplyOptions options;
            options.expected_digests = expectedDigestsParam(params);
            try {
                const auto result =
                    applyProjectMigration(project_root, registry, options);
                if (result.applied && on_applied) on_applied();
                auto response = result.toJson();
                // The documents on disk moved; whatever this process read
                // from them before did not.
                response["restart_required"] = result.applied;
                return response;
            } catch (const ProjectMigrationError &error) {
                throwMigrationRpcError(error);
            }
        },
        [](const OrderedJson &) { return true; });
}

} // namespace

void configureProjectMigrationRpcHandlers(
    RpcServer &server, std::filesystem::path project_root,
    const DocumentMigrationRegistry &registry) {
    configureHandlers(server, std::move(project_root), registry, {});
}

bool runProjectMigrationRpcServer(std::istream &input, std::ostream &output,
                                  std::filesystem::path project_root,
                                  const ProjectMigrationError &required,
                                  const DocumentMigrationRegistry &registry) {
    RpcServer server{input, output};
    auto applied = std::make_shared<bool>(false);
    configureHandlers(server, std::move(project_root), registry,
                      [applied] { *applied = true; });
    const std::string refusal = required.what();
    server.setRequestGate(
        [applied, refusal](std::string_view method) -> std::optional<std::string> {
            if (method == "plan_project_migration" ||
                method == "apply_project_migration") {
                return std::nullopt;
            }
            if (*applied) {
                return std::string{restartRequiredCode} +
                       ": {\"message\":\"the project was migrated; start a new "
                       "player to open it\"}";
            }
            return refusal;
        });
    server.run();
    return *applied;
}

} // namespace Pelican
