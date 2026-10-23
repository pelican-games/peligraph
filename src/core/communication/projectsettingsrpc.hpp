#pragma once

#include <string_view>

namespace Pelican {

class AssetsVerification;
class PathResolver;
class RpcServer;

namespace watch {
class ReloadService;
}

// PJ-1: a settings edit found project.json changed since its digest was read.
inline constexpr std::string_view projectSettingsExternalModificationCode =
    "external_modification";
// PJ-1: another writer holds the project's document transaction.
inline constexpr std::string_view projectSettingsTransactionBusyCode =
    "project.settings_transaction_busy@1";

// PJ-1. project.json's `hot_reload` and `asset_stores`, written from the
// editor and applied to the running engine in place.
//
//   get_project_settings {} ->
//     {project_digest, hot_reload: [..] | null, asset_stores: {..} | null}
//     (the two keys as project.json holds them, null when absent)
//
//   edit_project_settings {base_project_digest,
//                          hot_reload?: [paths] | null,
//                          asset_stores?: {name: {mount, manifest?}} | null}
//     A key left out keeps its value; null removes it. hot_reload replaces
//     the declaration (written normalized, as the watch reads it);
//     asset_stores is the whole new declaration. Checked as the engine opens a
//     project (parseProjectHotReload, the asset store checks), then written
//     through the project document transaction with base_project_digest as
//     its CAS, then applied: the resolver's stores and declaration
//     (PathResolver::replaceDeclarations), the file watch (ReloadService
//     setup again), and the asset verification (a newly declared manifest
//     whose file does not exist is written from the disk, and a rerun is
//     started). ->
//     {committed: true, changed, project_digest, previous_project_digest,
//      applied: {hot_reload, asset_stores}, previous: {hot_reload, asset_stores},
//      asset_stores_change: {added, removed, manifests_declared, manifests_removed},
//      assets_verification: {state, reason?, generated, failed} | null,
//      restart_required: {reason} | null}
//
// Refusals (application errors, data {error_code, error_fields}; nothing is
// written): external_modification {expected, actual},
// project.hot_reload_invalid@1 {key, reason},
// project.settings_invalid@1 {key, store?, reason},
// project.settings_change_unsupported@1 {key, store?, reason},
// project.settings_transaction_busy@1 {path}.
// `reload_service` / `assets_verification` may be null (a host without them):
// the part they would apply is answered as restart_required.
void configureProjectSettingsRpcHandlers(RpcServer &server,
                                         PathResolver &path_resolver,
                                         watch::ReloadService *reload_service,
                                         AssetsVerification *assets_verification);

} // namespace Pelican
