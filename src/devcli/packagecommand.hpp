#pragma once

namespace Pelican::DevCli {

// X-1: the agent-facing half of packaging.
//
//   pelican_cli package install <dir> --project <project>
//   pelican_cli package list --project <project>
//   pelican_cli package inspect <dir>
//
// `install` vendors a package directory into `<project>/packages/<namespace>/`
// and records it in project.json.  `list` re-reads what is installed and
// checks each recorded digest against the bytes on disk.  `inspect` validates
// one package directory and dumps its manifest, so a tool can see what a
// package brings without installing it.
int runPackageCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
