#include "rpcserver.hpp"

#include "../build_features.hpp"

namespace Pelican {

void runEngineRpcServer(std::istream &, std::ostream &) {
    throwBuildFeatureDisabled("PELICAN_WITH_RPC", "--rpc is unavailable");
}

} // namespace Pelican
