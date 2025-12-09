#pragma once
#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../resourcecontainer.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/image.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(RenderTargetContainer) {

    vk::Device device;

    struct InternalRenderTarget {
        std::string name;
        ImageWrapper image;
        vk::UniqueImageView image_view;
    };
    ResourceContainer<GlobalRenderTargetId, InternalRenderTarget> render_targets;

    std::unordered_map<std::string, GlobalRenderTargetId> name_to_id;

  public:
    RenderTargetContainer();
    ~RenderTargetContainer();

    GlobalRenderTargetId registerRenderTarget(const std::string& name, vk::Extent2D extent, vk::Format format, vk::ImageUsageFlags usage, vma::MemoryUsage memUsage);
    GlobalRenderTargetId getRenderTargetIdByName(const std::string &name) const;

    // 追加: 外部アクセス用アクセサ
    const InternalRenderTarget& get(GlobalRenderTargetId id) const { return render_targets.get(id); }
    InternalRenderTarget& get(GlobalRenderTargetId id) { return render_targets.get(id); }
};

} // namespace Pelican