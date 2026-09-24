#pragma once

#include <filesystem>
#include <iosfwd>

namespace Pelican {

class DocumentMigrationRegistry;
class ProjectMigrationError;
class RpcServer;

// MG-1. The editor RPC face of `project migrate`:
//
//   plan_project_migration  {} ->
//     {project_root, required, engine_version, saved_by_engine,
//      documents:[{path, schema, from, to, steps, digest, manual_items}],
//      expected:{<path>: <sha256>}}
//   apply_project_migration {expected:{<path>: <sha256>}} ->
//     the plan above plus {applied, backup, saved_by_engine_after,
//     restart_required}
//
// `expected` is the CAS: the digests of every document apply writes
// (project.json included), exactly as the plan returned them. A project that
// moved since is project.migration_plan_stale@1 and nothing is written.
// Failures are application errors whose data is
// {error_code, error_fields} (ProjectMigrationError).
void configureProjectMigrationRpcHandlers(
    RpcServer &server, std::filesystem::path project_root,
    const DocumentMigrationRegistry &registry);

// What an --rpc player serves instead of the engine when opening the project
// stopped at project.migration_required@1: the two methods above, and every
// other method refused with "project.migration_required@1: {fields}" (or,
// once a migration is applied, "project.migration_restart_required@1: {}"),
// until the input ends. The engine is never started in this process; after an
// apply the caller starts a new player. Returns whether a migration was
// applied.
bool runProjectMigrationRpcServer(std::istream &input, std::ostream &output,
                                  std::filesystem::path project_root,
                                  const ProjectMigrationError &required,
                                  const DocumentMigrationRegistry &registry);

} // namespace Pelican
