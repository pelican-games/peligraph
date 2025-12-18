#pragma once

#include "../container.hpp"
#include "../vkcore/image.hpp"
#include <glm/glm.hpp>
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct UiDrawRequest {
    vk::ImageView target_view;
    vk::Extent2D target_extent;
};

// stb_image を用いて UI 画像を読み込み、fullscreen パスの結果上に重ね描きする
DECLARE_MODULE(UiRenderer) {
    vk::Device device;

    vk::UniqueDescriptorSetLayout descset_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorPool desc_pool;

    struct UiTexture {
        std::string name;                         // UI 要素の名前
        ImageWrapper image;
        vk::UniqueImageView view;
        vk::UniqueDescriptorSet descset;
        vk::Extent2D pixel_size;
        glm::vec3 position; // 0-1 normalized (origin: top-left), z for depth
        glm::vec2 center = glm::vec2{0.5f, 0.5f}; // anchor point within image (0-1)
        float scale = 1.0f;
    };
    std::unordered_map<std::string, UiTexture> ui_textures;

  public:
    UiRenderer();
    ~UiRenderer();

    void render(vk::CommandBuffer cmd_buf, const UiDrawRequest &request) const;
    
    // 名前で UI 要素を取得・更新
    bool updateUI(const std::string& name, glm::vec3 position, float scale = -1.0f);
    bool updateUIPosition(const std::string& name, glm::vec3 position);
    bool updateUIScale(const std::string& name, float scale);
    bool updateUICenter(const std::string& name, glm::vec2 center);
    bool getUI(const std::string& name, glm::vec3& out_pos, float& out_scale) const;
};

} // namespace Pelican
