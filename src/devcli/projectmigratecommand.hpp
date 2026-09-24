#pragma once

#include <iosfwd>

namespace Pelican {
class DocumentMigrationRegistry;
}

namespace Pelican::DevCli {

// `pelican_cli project migrate --project <dir|project.json> [--dry-run]`.
// argv[0] is "migrate". Writes the plan (with --dry-run) or the apply result
// to `out` as JSON and returns 0; a named failure goes to `err` as
// "<code>: <fields JSON>" and the command returns -1. pelican_cli passes the
// engine's own registry; a test binary passes one with its test-only steps.
int runProjectMigrateCommand(int argc, char *argv[],
                             const DocumentMigrationRegistry &registry,
                             std::ostream &out, std::ostream &err);

} // namespace Pelican::DevCli
