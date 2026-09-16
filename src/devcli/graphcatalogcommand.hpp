#pragma once

namespace Pelican::DevCli {

// Prints the built-in graph module catalog as JSON: what every registered
// module brings, so a module author can discover types, operations and sinks
// without reading the engine sources.
int runGraphCatalogCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
