#include "rpcserver.hpp"
#include "projectmigrationrpc.hpp"

#include "../build_features.hpp"

namespace Pelican {

void runEngineRpcServer(std::istream &, std::ostream &) {
    throwBuildFeatureDisabled("PELICAN_WITH_RPC", "--rpc is unavailable");
}

bool runProjectMigrationRpcServer(std::istream &, std::ostream &,
                                  std::filesystem::path,
                                  const ProjectMigrationError &,
                                  const DocumentMigrationRegistry &) {
    throwBuildFeatureDisabled("PELICAN_WITH_RPC", "--rpc is unavailable");
}

} // namespace Pelican
