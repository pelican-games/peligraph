#pragma once

#include "outputcompilefacts.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct RendererRuntimeGeneration;

struct FrameRenderContext {
    vk::CommandBuffer cmd_buf;
    vk::Image color_image;
    vk::ImageView color_attachment, depth_attachment;
    // A view-family context exposes the full-array attachment above and the
    // compatible per-layer views here for mixed sequential/multiview scopes.
    std::vector<vk::ImageView> color_layer_attachments;
    std::vector<vk::ImageView> depth_layer_attachments;
    std::uint32_t color_base_array_layer = 0;
    std::uint32_t color_array_layers = 1;
    vk::Image depth_image;
    std::uint32_t depth_base_array_layer = 0;
    std::uint32_t depth_array_layers = 1;
    vk::Format depth_format = vk::Format::eUndefined;
    vk::ImageLayout depth_copy_layout =
        vk::ImageLayout::eUndefined;
    vk::ImageLayout depth_required_layout =
        vk::ImageLayout::eUndefined;
    vk::Extent2D extent;
    vk::Semaphore image_prepared_semaphore;
    vk::ImageLayout required_layout;
    uint32_t in_flight_frame_index = 0;
};

struct FrameTargetCaps {
    OutputCompileFacts compile_facts;
};

// Type-erased ownership root for every GPU object referenced by one queue
// submission. A frame target captures the lease after a successful submit and
// releases it only after the corresponding fence has completed.
using GpuSubmissionLease = std::shared_ptr<const void>;

class FrameTargetFrameCleanup {
  public:
    using AbandonCallback =
        void (*)(void *, std::uint64_t) noexcept;

  private:
    std::atomic<void *> owner_;
    AbandonCallback abandon_;

  public:
    FrameTargetFrameCleanup(
        void *owner, AbandonCallback abandon) noexcept
        : owner_{owner}, abandon_{abandon} {}

    void abandon(std::uint64_t serial) noexcept {
        if (const auto owner =
                owner_.load(std::memory_order_acquire);
            owner != nullptr && abandon_ != nullptr) {
            abandon_(owner, serial);
        }
    }

    void detach(void *owner) noexcept {
        auto expected = owner;
        (void)owner_.compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
};

enum class FrameTargetFrameState {
    empty,
    recording,
};

class FrameTargetFrame {
    friend class IFrameTarget;

    FrameRenderContext context_;
    std::shared_ptr<const RendererRuntimeGeneration>
        runtime_generation_;
    GpuSubmissionLease submission_lease_;
    std::shared_ptr<FrameTargetFrameCleanup> cleanup_;
    std::uint64_t serial_ = 0;
    FrameTargetFrameState state_ =
        FrameTargetFrameState::empty;

    FrameTargetFrame(
        FrameRenderContext context,
        std::shared_ptr<const RendererRuntimeGeneration>
            runtime_generation,
        GpuSubmissionLease submission_lease,
        std::shared_ptr<FrameTargetFrameCleanup> cleanup,
        std::uint64_t serial);
    void abandonIfActive() noexcept;

  public:
    FrameTargetFrame() = delete;
    ~FrameTargetFrame();
    FrameTargetFrame(const FrameTargetFrame &) = delete;
    FrameTargetFrame &operator=(
        const FrameTargetFrame &) = delete;
    FrameTargetFrame(FrameTargetFrame &&other) noexcept;
    FrameTargetFrame &operator=(
        FrameTargetFrame &&other) noexcept;

    const FrameRenderContext &context() const noexcept {
        return context_;
    }
    FrameRenderContext &context() noexcept {
        return context_;
    }
    std::shared_ptr<const RendererRuntimeGeneration>
    runtimeGeneration() const noexcept {
        return runtime_generation_;
    }
    FrameTargetFrameState state() const noexcept {
        return state_;
    }
};

enum class FrameBeginMode {
    blocking,
    nonblocking,
};

enum class FrameBeginDisposition {
    ready,
    unavailable,
    device_rebuild_required,
    fatal,
};

enum class FrameUnavailableReason {
    none,
    zero_extent,
    acquire_not_ready,
    output_out_of_date,
    surface_stale,
};

struct FrameBeginResult {
    FrameBeginDisposition disposition =
        FrameBeginDisposition::unavailable;
    FrameUnavailableReason reason =
        FrameUnavailableReason::none;
    std::optional<FrameTargetFrame> frame;
};

enum class FrameSubmitDisposition {
    submitted,
    presented,
    output_stale,
};

struct FrameSubmitResult {
    FrameSubmitDisposition disposition =
        FrameSubmitDisposition::submitted;
};

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
  protected:
    struct ConsumedFrame {
        FrameRenderContext context;
        std::shared_ptr<const RendererRuntimeGeneration>
            runtime_generation;
        GpuSubmissionLease submission_lease;
        std::uint64_t serial = 0;
    };

    static FrameTargetFrame makeFrame(
        FrameRenderContext context,
        std::shared_ptr<const RendererRuntimeGeneration>
            runtime_generation,
        GpuSubmissionLease submission_lease,
        const std::shared_ptr<FrameTargetFrameCleanup>
            &cleanup,
        std::uint64_t serial);
    static void validateFrameTarget(
        const FrameTargetFrame &frame,
        const std::shared_ptr<FrameTargetFrameCleanup>
            &expected_cleanup,
        std::string_view target_name);
    static ConsumedFrame consumeFrame(
        FrameTargetFrame &&frame,
        const std::shared_ptr<FrameTargetFrameCleanup>
            &expected_cleanup,
        std::uint64_t expected_serial,
        std::string_view target_name);
    static void abandonFrame(
        FrameTargetFrame &&frame) noexcept;

  public:
    virtual ~IFrameTarget() = default;
    virtual FrameBeginResult beginFrame(
        std::shared_ptr<const RendererRuntimeGeneration>
            runtime_generation,
        GpuSubmissionLease submission_lease,
        FrameBeginMode mode) = 0;
    virtual void recordOutputTransformCopy(vk::CommandBuffer cmd_buf, vk::Image source,
                                           vk::Format source_format, vk::Extent2D source_extent) = 0;
    virtual FrameSubmitResult submit(
        FrameTargetFrame frame) = 0;
    virtual void abandon(FrameTargetFrame frame) noexcept = 0;
    virtual FrameTargetCaps caps() const = 0;
    virtual std::vector<uint8_t> readbackLastFrameRGBA8() = 0;
};

} // namespace Pelican
