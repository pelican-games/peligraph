#include "introlinkcommand.hpp"

#include <iostream>

namespace Pelican::DevCli {

// PELICAN_WITH_INTRO_LINK=OFF: no intro link source is compiled. The wording
// follows the engine's build_feature_disabled@1 message.
int runIntroLinkCommand(int, char *[]) {
    std::cerr << "build_feature_disabled@1: This binary was built with PELICAN_WITH_INTRO_LINK=OFF: "
                 "pelican_cli intro-link is unavailable"
              << std::endl;
    return 1;
}

} // namespace Pelican::DevCli
