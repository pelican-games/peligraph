#pragma once

namespace Pelican::DevCli {

// `pelican_cli project init <dir> [--template standard|blank]`. The templates
// themselves live in pelican_project (projecttemplate.hpp), where Studio's
// File > New Project... reaches the same function.
int runProjectCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
