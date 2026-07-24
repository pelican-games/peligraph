#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct FrameRenderContext;

struct FrameTargetCaps {
    vk::Format color_format;
    vk::Extent2D extent;
    bool presents;
    bool capture_available;
    std::string color_path;
};

// Type-erased ownership root for every GPU object referenced by one queue
// submission. A frame target captures the lease after a successful submit and
// releases it only after the corresponding fence has completed.
using GpuSubmissionLease = std::shared_ptr<const void>;

template <std::size_t SlotCount>
class GpuSubmissionLeaseSlots {
    static_assert(SlotCount > 0);
    std::array<GpuSubmissionLease, SlotCount> slots_;

  public:
    void submitted(std::size_t slot,
                   GpuSubmissionLease lease) noexcept {
        assert(slot < SlotCount);
        assert(slots_[slot] == nullptr);
        slots_[slot] = std::move(lease);
    }

    void complete(std::size_t slot) noexcept {
        assert(slot < SlotCount);
        slots_[slot].reset();
    }

    void completeAll() noexcept {
        for (auto &lease : slots_) lease.reset();
    }

    std::size_t outstandingForTesting() const noexcept {
        return static_cast<std::size_t>(
            std::count_if(
                slots_.begin(), slots_.end(),
                [](const auto &lease) {
                    return lease != nullptr;
                }));
    }
};

class IFrameTarget {
  public:
    virtual ~IFrameTarget() = default;
    virtual FrameRenderContext render_begin() = 0;
    // Optional sinks use a zero-wait begin.  false means the frame is dropped;
    // it must not be treated as a rendering failure.
    virtual bool try_render_begin(FrameRenderContext &context) = 0;
    virtual void recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                           vk::Format source_format, vk::Extent2D source_extent) = 0;
    virtual void render_end(GpuSubmissionLease lease) = 0;
    virtual FrameTargetCaps caps() const = 0;
    virtual bool consumeExtentChanged() = 0;
    // Optional/nonblocking presenters can leave a window swapchain stale.
    // Recovery is an explicit, potentially blocking operation performed
    // outside the latency-critical acquire path.
    virtual bool recoverSurfaceIfStale() = 0;
    virtual std::vector<uint8_t> readbackLastFrameRGBA8() = 0;
};

} // namespace Pelican
