#include "uicontainer.hpp"

#include "atlasassetresource.hpp"
#include "../ui/module.hpp"

#include <stdexcept>

namespace Pelican {

UIContainer::UIContainer() {
    const auto &source_pages = GET_MODULE(ui::UiModule).pages();
    auto &atlas = GET_MODULE(AtlasAssetResource);
    shared_pages.reserve(source_pages.size() + 1);
    shared_pages.push_back(0); // shared white page
    for (const auto &source : source_pages) {
        const auto shared = atlas.registerPage(
            {.stable_name = source.stable_name,
             .image_path = source.image_path,
             .embedded_resource = source.embedded_resource,
             .size = {source.size.x, source.size.y}});
        shared_pages.push_back(shared);
    }
}

UIContainer::~UIContainer() = default;

vk::DescriptorSet UIContainer::descriptor(std::uint16_t page, ui::Sampler sampler) const {
    if (page >= shared_pages.size()) throw std::runtime_error("UI texture page is not registered");
    return GET_MODULE(AtlasAssetResource)
        .descriptor(shared_pages[page], sampler == ui::Sampler::Nearest
                                            ? asset::SamplerKey::nearest
                                            : asset::SamplerKey::linear);
}

vk::DescriptorSetLayout UIContainer::descriptorSetLayout() const {
    return GET_MODULE(AtlasAssetResource).descriptorSetLayout();
}

} // namespace Pelican
