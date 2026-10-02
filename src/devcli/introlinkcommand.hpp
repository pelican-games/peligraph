#pragma once

namespace Pelican::DevCli {

// `pelican_cli intro-link bake|serve ...` (IL-1). With
// PELICAN_WITH_INTRO_LINK=OFF this is the stub in introlinkdisabled.cpp.
int runIntroLinkCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
