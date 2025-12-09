#include "rendertargetcontainer.hpp"
#include "../vkcore/core.hpp"

namespace Pelican {

static vk::UniqueImageView createImageView(vk::Device device, const ImageWrapper &image) {
    vk::ImageViewCreateInfo ci;
    ci.image = image.image.get();
    ci.viewType = vk::ImageViewType::e2D;
    ci.format = image.format;
    ci.components = {
        vk::ComponentSwizzle::eR,
        vk::ComponentSwizzle::eG,
        vk::ComponentSwizzle::eB,
        vk::ComponentSwizzle::eA,
    };
    
    // フォーマットに応じてアスペクトマスクを設定
    if (image.format == vk::Format::eD32Sfloat || 
        image.format == vk::Format::eD16Unorm ||
        image.format == vk::Format::eD24UnormS8Uint ||
        image.format == vk::Format::eD32SfloatS8Uint) {
        ci.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
    } else {
        ci.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    }
    
    ci.subresourceRange.baseMipLevel = 0;
    ci.subresourceRange.levelCount = 1;
    ci.subresourceRange.baseArrayLayer = 0;
    ci.subresourceRange.layerCount = 1;
    return device.createImageViewUnique(ci);
}

RenderTargetContainer::RenderTargetContainer() :device{GET_MODULE(VulkanManageCore).getDevice()} {}

RenderTargetContainer::~RenderTargetContainer() {}

GlobalRenderTargetId RenderTargetContainer::registerRenderTarget(
    const std::string& name,
    vk::Extent2D extent,
    vk::Format format,
    vk::ImageUsageFlags usage,
    vma::MemoryUsage memUsage
) {
    // 既に同名が登録済みならそれを返す
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        return it->second;
    }

    const auto &vkcore = GET_MODULE(VulkanManageCore);

    // 2Dターゲットの Image を作成
    vk::Extent3D extent3D{extent.width, extent.height, 1};
    ImageWrapper image = vkcore.allocImage(
        extent3D,
        format,
        usage,
        memUsage,
        {} // createFlags
    );

    // ImageView を作成
    auto image_view = createImageView(device, image);

    // 初期レイアウト遷移を実行
    // 使用用途に応じた初期レイアウトに遷移
    vk::ImageLayout initial_layout = vk::ImageLayout::eUndefined;
    if (usage & vk::ImageUsageFlagBits::eColorAttachment) {
        initial_layout = vk::ImageLayout::eColorAttachmentOptimal;
    } else if (usage & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        initial_layout = vk::ImageLayout::eDepthAttachmentOptimal;
    }

    // 登録
    GlobalRenderTargetId id = render_targets.reg(InternalRenderTarget{
        .name = name,
        .image = std::move(image),
        .image_view = std::move(image_view),
    });

    name_to_id.emplace(name, id);
    return id;
}

GlobalRenderTargetId RenderTargetContainer::getRenderTargetIdByName(const std::string &name) const {
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        return it->second;
    }
    return GlobalRenderTargetId{-1};
}

} // namespace Pelican