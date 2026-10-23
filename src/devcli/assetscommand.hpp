#pragma once

#include "../project/assetsmanifest.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::DevCli {

int runAssetsCommand(int argc, char *argv[]);

// EB-1 (dist-config): `assets verify --full` over every asset store of the
// project that declares a manifest (.pelican/local.json's roots included),
// in project.json's order. Empty when the project declares no asset_stores
// or none of them a manifest. Throws on a project.json the assets commands
// refuse, and on a manifest that does not read.
struct DeclaredAssetStoreVerification {
    std::string name;
    std::filesystem::path root;
    std::filesystem::path manifest;
    AssetManifestVerificationResult result;
};
std::vector<DeclaredAssetStoreVerification> verifyDeclaredAssetStores(const std::filesystem::path &project_arg);

// The manifest issue kind as `assets verify` and get_assets_verification
// name it (content_mismatch, missing, extra, ...).
std::string_view assetManifestIssueKindLabel(AssetManifestIssueKind kind);

} // namespace Pelican::DevCli
