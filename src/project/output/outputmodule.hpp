#pragma once

namespace Pelican {

class GraphModuleRegistry;

// VO-1a: pelican.output@1. Registers only the removable `output.file` sink.
void registerOutputGraphModule(GraphModuleRegistry &registry);

} // namespace Pelican
