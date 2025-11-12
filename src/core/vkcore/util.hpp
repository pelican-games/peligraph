#pragma once

#include "../container.hpp"
#include "buf.hpp"
#include "cmdbuf.hpp"
#include "image.hpp"

namespace Pelican {

class VulkanUtils {
    DependencyContainer &con;
    vk::Device device;
    std::array<CommandBufWrapper, 8> genpurpose_cmd_bufs;
    uint32_t genpurpose_cmd_bufs_index;

  public:
    VulkanUtils(DependencyContainer &con);

    template <class F> void executeOneTimeCmd(F &&f) {
        const auto &cmd_buf = genpurpose_cmd_bufs[genpurpose_cmd_bufs_index];
        cmd_buf.recordBegin();
        f(*cmd_buf);
        cmd_buf.recordEndSubmit({}, {});
        genpurpose_cmd_bufs_index++;
        genpurpose_cmd_bufs_index %= genpurpose_cmd_bufs.size();
    }

    struct ChangeImageLayoutInfo {
        vk::PipelineStageFlags src_stage, dst_stage;
        vk::AccessFlags src_access, dst_access;
    };
    void changeImageLayout(const ImageWrapper &image, vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                           const ChangeImageLayoutInfo &info);

    struct ImageTransferInfo {
        vk::ImageLayout old_layout, new_layout;
        vk::PipelineStageFlags dst_stage;
        vk::AccessFlags dst_access;
    };
    void safeTransferMemoryToImage(const ImageWrapper &image, const void *src, vk::DeviceSize bytes_num,
                                   const ImageTransferInfo &info);
};

} // namespace Pelican