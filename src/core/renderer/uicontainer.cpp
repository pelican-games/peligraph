#include "uicontainer.hpp"
#include "../log.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stb_image.h>

namespace Pelican {

namespace {

vk::UniqueDescriptorSetLayout createDescSetLayout(vk::Device device) {
    vk::DescriptorSetLayoutBinding binding;
    binding.binding = 0;
    binding.descriptorCount = 1;
    binding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    binding.stageFlags = vk::ShaderStageFlagBits::eFragment;

    vk::DescriptorSetLayoutCreateInfo ci;
    ci.setBindings(binding);
    return device.createDescriptorSetLayoutUnique(ci);
}

vk::UniqueSampler createSampler(vk::Device device) {
    vk::SamplerCreateInfo ci;
    ci.magFilter = vk::Filter::eLinear;
    ci.minFilter = vk::Filter::eLinear;
    ci.mipmapMode = vk::SamplerMipmapMode::eLinear;
    ci.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    ci.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    ci.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    ci.maxAnisotropy = 1.0f;
    ci.anisotropyEnable = false;
    ci.unnormalizedCoordinates = false;
    return device.createSamplerUnique(ci);
}

vk::UniqueDescriptorPool createDescriptorPool(vk::Device device, uint32_t max_sets = 128) {
    vk::DescriptorPoolSize pool_size;
    pool_size.type = vk::DescriptorType::eCombinedImageSampler;
    pool_size.descriptorCount = max_sets;

    vk::DescriptorPoolCreateInfo ci;
    ci.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    ci.maxSets = max_sets;
    ci.setPoolSizes(pool_size);
    return device.createDescriptorPoolUnique(ci);
}

ImageWrapper createImageFromStb(const std::string &file, vk::Device device) {
    int tex_w = 0, tex_h = 0, tex_comp = 0;
    stbi_uc *pixels = stbi_load(file.c_str(), &tex_w, &tex_h, &tex_comp, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error("Failed to load UI image: " + file);
    }

    vk::Extent3D extent{static_cast<uint32_t>(tex_w), static_cast<uint32_t>(tex_h), 1};

    const auto &vkcore = GET_MODULE(VulkanManageCore);
    auto image = vkcore.allocImage(extent, vk::Format::eR8G8B8A8Unorm,
                                   vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                   vma::MemoryUsage::eAutoPreferDevice, {});

    GET_MODULE(VulkanUtils).safeTransferMemoryToImage(
        image, pixels, extent.width * extent.height * 4,
        VulkanUtils::ImageTransferInfo{
            .old_layout = vk::ImageLayout::eUndefined,
            .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .dst_stage = vk::PipelineStageFlagBits::eFragmentShader,
            .dst_access = vk::AccessFlagBits::eShaderRead,
        });

    stbi_image_free(pixels);
    return image;
}

vk::UniqueImageView createImageView(vk::Device device, const ImageWrapper &image) {
    vk::ImageViewCreateInfo ci;
    ci.image = image.image.get();
    ci.viewType = vk::ImageViewType::e2D;
    ci.format = image.format;
    ci.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    ci.subresourceRange.baseMipLevel = 0;
    ci.subresourceRange.levelCount = 1;
    ci.subresourceRange.baseArrayLayer = 0;
    ci.subresourceRange.layerCount = 1;
    return device.createImageViewUnique(ci);
}

} // namespace

UIContainer::UIContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()} {
    
    auto descset_layout_unique = createDescSetLayout(device);
    auto sampler_unique = createSampler(device);
    auto desc_pool_unique = createDescriptorPool(device);
    
    descset_layout = descset_layout_unique.release();
    sampler = sampler_unique.release();
    desc_pool = desc_pool_unique.release();

    // JSON設定からUI画像を読み込む
    try {
        const std::string json_path = "ui_overlay.json";
        if (std::filesystem::exists(json_path)) {
            std::ifstream ifs(json_path, std::ios::binary);
            const std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            const auto json = nlohmann::json::parse(data);
            const auto images = json.value("images", nlohmann::json::array());
            
            for (const auto &img : images) {
                if (!img.contains("file"))
                    continue;

                const std::string file = img.value("file", "");
                const std::string ui_name = img.value("name", file);
                const float scale = img.value("scale", 1.0f);
                
                const auto pos_arr = img.value("position", std::vector<float>{0.0f, 0.0f, 0.0f});
                glm::vec3 pos{0.0f};
                if (pos_arr.size() >= 2) {
                    pos.x = pos_arr[0];
                    pos.y = pos_arr[1];
                }
                if (pos_arr.size() >= 3) {
                    pos.z = pos_arr[2];
                }

                const auto center_arr = img.value("center", std::vector<float>{0.5f, 0.5f});
                glm::vec2 center{0.5f, 0.5f};
                if (center_arr.size() >= 2) {
                    center.x = center_arr[0];
                    center.y = center_arr[1];
                }

                if (std::filesystem::exists(file)) {
                    registerUI(ui_name, file, pos, center, scale);
                } else {
                    LOG_WARNING(logger, "UI image file not found: {}", file);
                }
            }
        } else {
            LOG_WARNING(logger, "ui_overlay.json not found. UI overlay will be skipped.");
        }
    } catch (const std::exception &e) {
        LOG_WARNING(logger, "UI config load failed: {}", e.what());
    }
    
    LOG_INFO(logger, "UI Container initialized with {} textures", ui_textures.size());
}

UIContainer::~UIContainer() {
    if (descset_layout) {
        device.destroyDescriptorSetLayout(descset_layout);
    }
    if (sampler) {
        device.destroySampler(sampler);
    }
    if (desc_pool) {
        device.destroyDescriptorPool(desc_pool);
    }
}

void UIContainer::registerUI(const std::string& name, const std::string& file_path, 
                            glm::vec3 position, glm::vec2 center, float scale) {
    try {
        UiTexture texture;
        texture.name = name;
        texture.image = createImageFromStb(file_path, device);
        texture.view = createImageView(device, texture.image);
        texture.pixel_size = vk::Extent2D{texture.image.extent.width, texture.image.extent.height};
        texture.position = position;
        texture.center = center;
        texture.scale = scale;

        vk::DescriptorSetAllocateInfo alloc_info;
        alloc_info.descriptorPool = desc_pool;
        alloc_info.setSetLayouts(descset_layout);
        auto descsets = device.allocateDescriptorSetsUnique(alloc_info);
        texture.descset = std::move(descsets[0]);

        vk::DescriptorImageInfo image_info;
        image_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        image_info.imageView = texture.view.get();
        image_info.sampler = sampler;

        vk::WriteDescriptorSet write;
        write.dstSet = texture.descset.get();
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        write.pImageInfo = &image_info;

        device.updateDescriptorSets(write, {});

        LOG_INFO(logger, "UI texture registered: {} ({}x{} @ {},{},{}, scale={}, center={},{})", name, 
                texture.pixel_size.width, texture.pixel_size.height, 
                texture.position.x, texture.position.y, texture.position.z, texture.scale,
                texture.center.x, texture.center.y);
        
        ui_textures[name] = std::move(texture);
    } catch (const std::exception &e) {
        LOG_WARNING(logger, "Failed to register UI texture ({}): {}", file_path, e.what());
    }
}

bool UIContainer::updateUI(const std::string& name, glm::vec3 position, float scale) {
    auto it = ui_textures.find(name);
    if (it != ui_textures.end()) {
        it->second.position = position;
        if (scale >= 0.0f) {
            it->second.scale = scale;
        }
        return true;
    }
    return false;
}

bool UIContainer::updateUIPosition(const std::string& name, glm::vec3 position) {
    auto it = ui_textures.find(name);
    if (it != ui_textures.end()) {
        it->second.position = position;
        return true;
    }
    return false;
}

bool UIContainer::updateUIScale(const std::string& name, float scale) {
    auto it = ui_textures.find(name);
    if (it != ui_textures.end()) {
        it->second.scale = scale;
        return true;
    }
    return false;
}

bool UIContainer::updateUICenter(const std::string& name, glm::vec2 center) {
    auto it = ui_textures.find(name);
    if (it != ui_textures.end()) {
        it->second.center = center;
        return true;
    }
    return false;
}

bool UIContainer::getUI(const std::string& name, glm::vec3& out_pos, float& out_scale) const {
    auto it = ui_textures.find(name);
    if (it != ui_textures.end()) {
        out_pos = it->second.position;
        out_scale = it->second.scale;
        return true;
    }
    return false;
}

} // namespace Pelican
