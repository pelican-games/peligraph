#pragma once

#include <string>
#include <vector>

namespace Pelican {

// Physical shader defines selected by the active material render pipeline.
// Both glTF and project-authored material producers must compile against this
// same snapshot before registering their runtime resources.
std::vector<std::string> activeMaterialShaderDefines();

} // namespace Pelican
