#include "uirenderer.hpp"

#include "../log.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"
#include "../vkcore/rendertarget.hpp"
#include "../shader/shadercontainer.hpp"
#include "battery/embed.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <stb_image.h>

namespace Pelican {

namespace {

struct UiPushConstant {
    glm::vec3 pos;   // x, y (normalized 0-1), z (depth 0-1)
    float _pad1;     // padding for alignment
    glm::vec2 size;  // normalized size
    float _pad2, _pad3;  // padding for alignment
};

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

vk::UniquePipelineLayout createPipelineLayout(vk::Device device, vk::DescriptorSetLayout descset_layout) {
    vk::PushConstantRange push_const_range;
    push_const_range.stageFlags = vk::ShaderStageFlagBits::eVertex;
    push_const_range.offset = 0;
    push_const_range.size = sizeof(UiPushConstant);

    vk::PipelineLayoutCreateInfo ci;
    ci.setSetLayouts(descset_layout);
    ci.setPushConstantRanges(push_const_range);
    return device.createPipelineLayoutUnique(ci);
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

vk::UniquePipeline createPipeline(vk::Device device, vk::PipelineLayout layout, vk::ShaderModule vert_shader,
                                  vk::ShaderModule frag_shader, vk::Format color_format) {
    vk::PipelineShaderStageCreateInfo vert_stage;
    vert_stage.stage = vk::ShaderStageFlagBits::eVertex;
    vert_stage.module = vert_shader;
    vert_stage.pName = "main";

    vk::PipelineShaderStageCreateInfo frag_stage;
    frag_stage.stage = vk::ShaderStageFlagBits::eFragment;
    frag_stage.module = frag_shader;
    frag_stage.pName = "main";

    const std::array stages{vert_stage, frag_stage};

    vk::PipelineVertexInputStateCreateInfo vertex_input; // no vertex buffer

    vk::PipelineInputAssemblyStateCreateInfo input_asm;
    input_asm.topology = vk::PrimitiveTopology::eTriangleList;
    input_asm.primitiveRestartEnable = false;

    vk::PipelineViewportStateCreateInfo viewport_state;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo raster;
    raster.polygonMode = vk::PolygonMode::eFill;
    raster.cullMode = vk::CullModeFlagBits::eNone;
    raster.frontFace = vk::FrontFace::eCounterClockwise;
    raster.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo msaa;
    msaa.rasterizationSamples = vk::SampleCountFlagBits::e1;

    vk::PipelineDepthStencilStateCreateInfo depth;
    depth.depthTestEnable = false;
    depth.depthWriteEnable = false;

    vk::PipelineColorBlendAttachmentState blend_att;
    blend_att.blendEnable = true;
    blend_att.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    blend_att.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blend_att.colorBlendOp = vk::BlendOp::eAdd;
    blend_att.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blend_att.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blend_att.alphaBlendOp = vk::BlendOp::eAdd;
    blend_att.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    vk::PipelineColorBlendStateCreateInfo blend;
    blend.logicOpEnable = false;
    blend.setAttachments(blend_att);

    const std::array dyn_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dyn_state;
    dyn_state.setDynamicStates(dyn_states);

    vk::PipelineRenderingCreateInfo rendering;
    rendering.setColorAttachmentFormats(color_format);

    vk::GraphicsPipelineCreateInfo ci;
    ci.setStages(stages);
    ci.pVertexInputState = &vertex_input;
    ci.pInputAssemblyState = &input_asm;
    ci.pViewportState = &viewport_state;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &msaa;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dyn_state;
    ci.layout = layout;

    vk::StructureChain chain{ci, rendering};

    auto result = device.createGraphicsPipelineUnique({}, chain.get());
    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error("failed to create UI pipeline: " + vk::to_string(result.result));
    }
    return std::move(result.value);
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

UiRenderer::UiRenderer()
    : device{GET_MODULE(VulkanManageCore).getDevice()}, descset_layout{createDescSetLayout(device)},
      pipeline_layout{createPipelineLayout(device, descset_layout.get())}, sampler{createSampler(device)},
      desc_pool{createDescriptorPool(device)} {
    auto &shader_con = GET_MODULE(ShaderContainer);

    LOG_INFO(logger, "Loading UI shaders...");
    const auto vert = b::embed<"ui.vert.spv">();
    const auto frag = b::embed<"ui.frag.spv">();
    
    if (vert.length() == 0 || frag.length() == 0) {
        throw std::runtime_error("UI shaders not found. Please build shaders first.");
    }
    
    const auto vert_shader = shader_con.registerShader(vert.length(), vert.data());
    const auto frag_shader = shader_con.registerShader(frag.length(), frag.data());
    LOG_INFO(logger, "UI shaders loaded successfully");

    const auto color_format = GET_MODULE(RenderTarget).getSwapchainFormat();
    pipeline = createPipeline(device, pipeline_layout.get(), shader_con.getShader(vert_shader),
                              shader_con.getShader(frag_shader), color_format);
    LOG_INFO(logger, "UI pipeline created");

    // JSON 設定から UI 画像を読み込む
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

                if (!std::filesystem::exists(file)) {
                    LOG_WARNING(logger, "UI image file not found: {}", file);
                    continue;
                }

                try {
                    const std::string ui_name = img.value("name", file); // name がない場合は file 名を使用
                    
                    UiTexture texture;
                    texture.name = ui_name;
                    texture.image = createImageFromStb(file, device);
                    texture.view = createImageView(device, texture.image);
                    texture.pixel_size = vk::Extent2D{texture.image.extent.width, texture.image.extent.height};
                    texture.position = pos;
                    texture.center = center;
                    texture.scale = scale;

                    vk::DescriptorSetAllocateInfo alloc_info;
                    alloc_info.descriptorPool = desc_pool.get();
                    alloc_info.setSetLayouts(descset_layout.get());
                    auto descsets = device.allocateDescriptorSetsUnique(alloc_info);
                    texture.descset = std::move(descsets[0]);

                    vk::DescriptorImageInfo image_info;
                    image_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
                    image_info.imageView = texture.view.get();
                    image_info.sampler = sampler.get();

                    vk::WriteDescriptorSet write;
                    write.dstSet = texture.descset.get();
                    write.dstBinding = 0;
                    write.descriptorCount = 1;
                    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
                    write.pImageInfo = &image_info;

                    device.updateDescriptorSets(write, {});

                    LOG_INFO(logger, "UI texture loaded: {} ({}x{} @ {},{},{}, scale={}, center={},{})", ui_name, 
                            texture.pixel_size.width, texture.pixel_size.height, 
                            texture.position.x, texture.position.y, texture.position.z, texture.scale,
                            texture.center.x, texture.center.y);
                    ui_textures[ui_name] = std::move(texture);
                } catch (const std::exception &e) {
                    LOG_WARNING(logger, "Failed to create UI texture ({}): {}", file, e.what());
                }
            }
        } else {
            LOG_WARNING(logger, "ui_overlay.json not found. UI overlay will be skipped.");
        }
    } catch (const std::exception &e) {
        LOG_WARNING(logger, "UI config load failed: {}", e.what());
    }
    
    LOG_INFO(logger, "UI Renderer initialized with {} textures", ui_textures.size());
}

UiRenderer::~UiRenderer() {}

void UiRenderer::render(vk::CommandBuffer cmd_buf, const UiDrawRequest &request) const {
    if (ui_textures.empty()) {
        LOG_INFO(logger, "UiRenderer::render - no textures to render");
        return;
    }
    
    LOG_INFO(logger, "UiRenderer::render - rendering {} UI textures", ui_textures.size());

    vk::RenderingAttachmentInfo color_att;
    color_att.imageView = request.target_view;
    color_att.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_att.loadOp = vk::AttachmentLoadOp::eLoad; // keep fullscreen result
    color_att.storeOp = vk::AttachmentStoreOp::eStore;

    vk::RenderingInfo render_info;
    render_info.renderArea = vk::Rect2D{{0, 0}, request.target_extent};
    render_info.layerCount = 1;
    render_info.setColorAttachments(color_att);

    cmd_buf.beginRendering(render_info);

    vk::Viewport viewport{0.0f, 0.0f, static_cast<float>(request.target_extent.width),
                          static_cast<float>(request.target_extent.height), 0.0f, 1.0f};
    vk::Rect2D scissor{{0, 0}, request.target_extent};
    cmd_buf.setViewport(0, viewport);
    cmd_buf.setScissor(0, scissor);

    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get());

    for (const auto &[name, tex] : ui_textures) {
        UiPushConstant pc{};
        const glm::vec2 framebuffer_size{static_cast<float>(request.target_extent.width),
                                         static_cast<float>(request.target_extent.height)};
        glm::vec2 tex_size_px{static_cast<float>(tex.pixel_size.width), static_cast<float>(tex.pixel_size.height)};
        glm::vec2 size_norm = (tex_size_px * tex.scale) / framebuffer_size;

        // centerを考慮した位置計算：positionはcenterで指定された点の配置位置
        glm::vec2 offset = tex.center * size_norm;
        glm::vec3 adjusted_pos = tex.position;
        adjusted_pos.x -= offset.x;
        adjusted_pos.y -= offset.y;

        pc.pos = adjusted_pos;  // vec3 with x, y (0-1), z (depth)
        pc.size = size_norm;

        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout.get(), 0,
                                   {tex.descset.get()}, {});
        cmd_buf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eVertex, 0, sizeof(UiPushConstant), &pc);
        cmd_buf.draw(6, 1, 0, 0);
    }

    cmd_buf.endRendering();
}

bool UiRenderer::updateUI(const std::string& name, glm::vec3 position, float scale) {
    auto it = ui_textures.find(name);
    if (it != ui_textures.end()) {
        it->second.position = position;
        if (scale >= 0.0f) {
            it->second.scale = scale;
        }
        LOG_INFO(logger, "UI updated: {} position={},{},{} scale={}", name, position.x, position.y, position.z, it->second.scale);
        return true;
    }
    LOG_WARNING(logger, "UI not found: {}", name);
    return false;
}

bool UiRenderer::updateUIPosition(const std::string& name, glm::vec3 position) {
    auto it = ui_textures.find(name);
    if (it != ui_textures.end()) {
        it->second.position = position;
        return true;
    }
    return false;
}

bool UiRenderer::updateUIScale(const std::string& name, float scale) {
    auto it = ui_textures.find(name);
    if (it != ui_textures.end()) {
        it->second.scale = scale;
        return true;
    }
    return false;
}

bool UiRenderer::updateUICenter(const std::string& name, glm::vec2 center) {
    auto it = ui_textures.find(name);
    if (it != ui_textures.end()) {
        it->second.center = center;
        return true;
    }
    return false;
}

bool UiRenderer::getUI(const std::string& name, glm::vec3& out_pos, float& out_scale) const {
    auto it = ui_textures.find(name);
    if (it != ui_textures.end()) {
        out_pos = it->second.position;
        out_scale = it->second.scale;
        return true;
    }
    return false;
}

} // namespace Pelican