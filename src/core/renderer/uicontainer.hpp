#pragma once

#include "../container.hpp"
#include "../vkcore/image.hpp"
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct UiTexture {
    std::string name;
    ImageWrapper image;
    vk::UniqueImageView view;
    vk::UniqueDescriptorSet descset;
    vk::Extent2D pixel_size;
    glm::vec3 position; // 0-1 normalized (origin: top-left), z for depth
    glm::vec2 center = glm::vec2{0.5f, 0.5f}; // anchor point within image (0-1)
    float scale = 1.0f;
};

DECLARE_MODULE(UIContainer) {
    vk::Device device;
    vk::DescriptorSetLayout descset_layout;
    vk::Sampler sampler;
    vk::DescriptorPool desc_pool;
    
    std::unordered_map<std::string, UiTexture> ui_textures;

  public:
    UIContainer();
    ~UIContainer();

    // UI要素の登録
    void registerUI(const std::string& name, const std::string& file_path, 
                   glm::vec3 position, glm::vec2 center, float scale);

    // UI要素の更新
    bool updateUI(const std::string& name, glm::vec3 position, float scale = -1.0f);
    bool updateUIPosition(const std::string& name, glm::vec3 position);
    bool updateUIScale(const std::string& name, float scale);
    bool updateUICenter(const std::string& name, glm::vec2 center);
    
    // UI要素の取得
    bool getUI(const std::string& name, glm::vec3& out_pos, float& out_scale) const;
    const std::unordered_map<std::string, UiTexture>& getAllTextures() const { return ui_textures; }
    
    // レンダラー用のリソース取得
    vk::DescriptorSetLayout getDescriptorSetLayout() const { return descset_layout; }
    vk::Sampler getSampler() const { return sampler; }
};

} // namespace Pelican
