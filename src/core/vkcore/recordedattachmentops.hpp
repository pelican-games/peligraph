#pragma once

#include "../renderingpass/renderingpass.hpp"
#include <cstddef>
#include <vulkan/vulkan.hpp>

namespace Pelican {

// RC-3a2: the Vulkan load / store ops the recording hands beginRendering:
// for a pass's color attachment `index` and its depth attachment (a pass not
// fused into a scope), and for a fused scope's resolved operations. Defined
// with the recording (render_pass_frame_setup.cpp); the mapping is
// vulkanimagevocabulary's.
struct RecordedAttachmentOps {
    vk::AttachmentLoadOp load_op;
    vk::AttachmentStoreOp store_op;

    bool operator==(const RecordedAttachmentOps &) const = default;
};
RecordedAttachmentOps recordedAttachmentOps(
    PassAttachmentOperations operations);
RecordedAttachmentOps recordedColorAttachmentOps(
    const PassDefinition &pass_def, std::size_t index);
RecordedAttachmentOps recordedDepthAttachmentOps(
    const PassDefinition &pass_def);

} // namespace Pelican
