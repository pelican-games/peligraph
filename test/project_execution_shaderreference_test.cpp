// Include each moved public header in its own translation unit.
#include "../src/project/shaderreference.hpp"

#include <stdexcept>

bool projectPassInfoDefaults();
bool projectPassLoopIndexDefaults();
bool projectComputeTaskDefaults();

int main() {
    using namespace Pelican;
    const auto shader = makeShaderReference("shaders/example", ShaderStage::compute);
    if (shader.ref != "shaders/example" || shader.stage != ShaderStage::compute ||
        shader.kind != ShaderReferenceKind::stem || shader.backend_specific) return 1;
    const DeclaredShaderReference declared{DeclaredShaderStage::miss, 2, "shaders/miss"};
    if (declared.index != 2 || declared.ref != "shaders/miss") return 2;
    try {
        (void)makeShaderReference("shaders/example.comp", ShaderStage::compute);
        return 3;
    } catch (const std::runtime_error &) {
    }
    return projectPassInfoDefaults() && projectPassLoopIndexDefaults() &&
                   projectComputeTaskDefaults() ? 0 : 4;
}
