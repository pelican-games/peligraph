#include "shadercontainer.hpp"
#include "../vkcore/core.hpp"
#include <cstdint>
#include <stdexcept>

static vk::UniqueShaderModule createShaderModule(vk::Device device, size_t len, const char *data) {
    if (data == nullptr || len == 0) {
        throw std::runtime_error("Shader module data must not be empty");
    }
    if (len % sizeof(uint32_t) != 0) {
        throw std::runtime_error("Shader module data size must be a multiple of 4 bytes");
    }

    vk::ShaderModuleCreateInfo create_info;
    create_info.codeSize = len;
    create_info.pCode = reinterpret_cast<const uint32_t *>(data);

    return device.createShaderModuleUnique(create_info);
}

namespace Pelican {

ShaderContainer::ShaderContainer() {}

GlobalShaderId ShaderContainer::registerShader(size_t len, const char *data) {
    return shaders.reg(createShaderModule(GET_MODULE(VulkanManageCore).getDevice(), len, data));
}
vk::ShaderModule ShaderContainer::getShader(GlobalShaderId id) const { return shaders.get(id).get(); }

} // namespace Pelican
