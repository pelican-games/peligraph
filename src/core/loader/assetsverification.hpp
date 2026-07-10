#pragma once

#include "pathresolver.hpp"

#include <filesystem>
#include <vector>

namespace Pelican {

struct StartupAssetsVerificationSummary {
    int stores = 0;
    int matched = 0;
    int info = 0;
    int warnings = 0;
    int errors = 0;

    bool shouldContinueLoading() const { return errors == 0; }
};

StartupAssetsVerificationSummary verifyAssetsAtStartup(
    const std::vector<AssetStoreStatus> &stores,
    const std::filesystem::path &cache_path,
    bool strict_assets);

} // namespace Pelican
