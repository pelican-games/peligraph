#include "renderingpass.hpp"

#include <stdexcept>

namespace Pelican {

PassAttachmentOperations defaultPassAttachmentOperations(
    RenderPassType pass_type, PassAttachmentAspect aspect) {
    switch (aspect) {
    case PassAttachmentAspect::color:
        return PassAttachmentOperations{
            pass_type == RenderPassType::ui ||
                    pass_type == RenderPassType::imgui
                ? vk::AttachmentLoadOp::eLoad
                : vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore,
        };
    case PassAttachmentAspect::depth:
        return PassAttachmentOperations{
            vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eDontCare,
        };
    }
    throw std::logic_error("unknown pass attachment aspect");
}

} // namespace Pelican
