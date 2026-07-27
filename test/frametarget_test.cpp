#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/vkcore/frametarget.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <type_traits>

using namespace Pelican;

namespace {

class FakeFrameTarget final : public IFrameTarget {
    std::shared_ptr<FrameTargetFrameCleanup> cleanup_;
    std::uint64_t serial_ = 0;
    bool active_ = false;

    static void cleanup(void *owner,
                        std::uint64_t serial) noexcept {
        auto &self =
            *static_cast<FakeFrameTarget *>(owner);
        if (self.active_ && self.serial_ == serial) {
            self.active_ = false;
            ++self.abandoned;
        }
    }

  public:
    std::size_t submitted = 0;
    std::size_t abandoned = 0;
    GpuSubmissionLease target_epoch;

    FakeFrameTarget()
        : cleanup_{
              std::make_shared<
                  FrameTargetFrameCleanup>(
                  this, &FakeFrameTarget::cleanup)} {}
    ~FakeFrameTarget() override {
        cleanup_->detach(this);
    }

    FrameBeginResult beginFrame(
        std::shared_ptr<const RendererRuntimeGeneration>
            runtime_generation,
        GpuSubmissionLease submission_lease,
        FrameBeginMode) override {
        if (active_) {
            throw std::logic_error(
                "fake target already has an active frame");
        }
        active_ = true;
        ++serial_;
        auto frame = makeFrame(
            FrameRenderContext{},
            std::move(runtime_generation),
            std::move(submission_lease), cleanup_,
            serial_, target_epoch);
        FrameBeginResult result;
        result.disposition =
            FrameBeginDisposition::ready;
        result.frame.emplace(std::move(frame));
        return result;
    }

    void recordOutputTransformCopy(
        vk::CommandBuffer, vk::Image, vk::Format,
        vk::Extent2D) override {}

    FrameSubmitResult submit(
        FrameTargetFrame frame) override {
        validateFrameTarget(
            frame, cleanup_, "fake frame target");
        if (!active_) {
            throw std::logic_error(
                "fake target has no active frame");
        }
        (void)consumeFrame(
            std::move(frame), cleanup_, serial_,
            "fake frame target");
        active_ = false;
        ++submitted;
        return {};
    }

    void abandon(FrameTargetFrame frame) noexcept override {
        abandonFrame(std::move(frame));
    }

    FrameTargetCaps caps() const override {
        return {};
    }
    std::vector<std::uint8_t>
    readbackLastFrameRGBA8() override {
        return {};
    }
};

static_assert(
    std::is_move_constructible_v<FrameTargetFrame>);
static_assert(
    std::is_move_assignable_v<FrameTargetFrame>);
static_assert(
    !std::is_copy_constructible_v<FrameTargetFrame>);
static_assert(
    !std::is_copy_assignable_v<FrameTargetFrame>);

} // namespace

TEST_CASE(
    "WP215 a dropped frame token abandons exactly its owning frame",
    "[wp215][frame-target][token][raii]") {
    FakeFrameTarget target;
    {
        auto begun = target.beginFrame(
            nullptr, {}, FrameBeginMode::blocking);
        REQUIRE(begun.frame.has_value());
        auto moved = std::move(*begun.frame);
        REQUIRE(moved.state() ==
                FrameTargetFrameState::recording);
    }
    REQUIRE(target.abandoned == 1);
    REQUIRE(target.submitted == 0);

    auto next = target.beginFrame(
        nullptr, {}, FrameBeginMode::blocking);
    auto submitted = std::move(*next.frame);
    target.submit(std::move(submitted));
    REQUIRE(target.abandoned == 1);
    REQUIRE(target.submitted == 1);
    REQUIRE_THROWS_WITH(
        target.submit(std::move(submitted)),
        "fake frame target received an already-consumed frame token");
}

TEST_CASE(
    "WP215 a frame token retains its renderer generation until consumption",
    "[wp215][frame-target][token][lifetime]") {
    FakeFrameTarget target;
    auto generation =
        std::make_shared<RendererRuntimeGeneration>();
    std::weak_ptr<const RendererRuntimeGeneration>
        observed = generation;
    auto begun = target.beginFrame(
        generation, {}, FrameBeginMode::blocking);
    generation.reset();
    REQUIRE_FALSE(observed.expired());

    target.submit(std::move(*begun.frame));
    REQUIRE(observed.expired());
}

TEST_CASE(
    "WP215 another target cannot consume a frame token",
    "[wp215][frame-target][token][ownership]") {
    FakeFrameTarget owner;
    FakeFrameTarget other;
    auto owner_frame = owner.beginFrame(
        nullptr, {}, FrameBeginMode::blocking);
    auto other_frame = other.beginFrame(
        nullptr, {}, FrameBeginMode::blocking);

    REQUIRE_THROWS_WITH(
        other.submit(std::move(*owner_frame.frame)),
        "fake frame target received a frame token owned by another target");
    REQUIRE(owner.abandoned == 1);
    REQUIRE(other.submitted == 0);
    other.abandon(std::move(*other_frame.frame));
    REQUIRE(other.abandoned == 1);
}

TEST_CASE(
    "WP216 a frame token explicitly retains its target epoch",
    "[wp216][frame-target][epoch][lifetime]") {
    FakeFrameTarget target;
    auto epoch = std::make_shared<const int>(29);
    std::weak_ptr<const int> observed = epoch;
    target.target_epoch = epoch;
    auto begun = target.beginFrame(
        nullptr, {}, FrameBeginMode::blocking);
    target.target_epoch.reset();
    epoch.reset();
    REQUIRE_FALSE(observed.expired());

    target.submit(std::move(*begun.frame));
    REQUIRE(observed.expired());
}
