#pragma once

namespace Pelican::DevCli {

// `pelican_cli project init <dir> [--template standard|blank]`. The templates
// themselves live in pelican_project (projecttemplate.hpp), where Studio's
// File > New Project... reaches the same function.
// `pelican_cli project migrate --project <dir> [--dry-run]` (MG-1) moves a
// project's documents forward through the engine's registered migration steps
// (projectmigratecommand.hpp).
int runProjectCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
