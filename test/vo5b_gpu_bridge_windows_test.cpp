// VO-5b: the bridge itself with real Vulkan, real D3D11, a real spoutDX and a real
// receiver, with no player: the protocol's proof, apart from the worker's fence.
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/output/outputbridgecommands.hpp"
#include "../src/core/output/outputspout.hpp"
#include "../src/core/output/outputspoutgpupath.hpp"
#include "../src/core/vkcore/core.hpp"
#include "spout_test_receiver.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Pelican::FastModuleContainer;

namespace {
using namespace std::chrono_literals;
using Pelican::OutputSpoutGpuFailure;
using Pelican::OutputSpoutSentState;

bool expectsGpu() { return std::getenv("PELICAN_VO5B_EXPECT_GPU") != nullptr; }

std::string uniqueName(const std::string &base) {
    return "pelican_vo5b_bridge_" + base + "_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

template <class Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds limit = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

void setFault(const char *value) {
    REQUIRE(_putenv_s("PELICAN_OUTPUT_SPOUT_GPU_FAULT", value) == 0);
}

Pelican::OutputSpoutGpuRequest gpuRequest(Pelican::VulkanManageCore &vulkan) {
    const auto preflight = Pelican::outputSpoutGpuPathForRun(true, false, vulkan);
    WARN(Pelican::outputSpoutGpuPathJson(preflight).dump());
    Pelican::OutputSpoutGpuRequest request;
    if (preflight.state != "available") return request;
    request.device = static_cast<VkDevice>(vulkan.getDevice());
    request.allowed = true;
    request.vulkan_luid = preflight.facts->device_luid.bytes;
    request.max_width = preflight.facts->kmt.max_width;
    request.max_height = preflight.facts->kmt.max_height;
    return request;
}

// One real backend on a real device, with its Vulkan commands.
struct Bridge {
    FastModuleContainer modules;
    Pelican::VulkanManageCore *vulkan = nullptr;
    std::unique_ptr<Pelican::OutputSpoutBackend> backend;
    Pelican::OutputSpoutGpuBackend *gpu = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    std::array<VkImage, Pelican::output_spout_bridge_count> images{};
    vk::UniqueCommandPool pool;
    vk::UniqueCommandBuffer command_buffer;
    vk::UniqueFence fence;
    std::string name;
    bool prepared = false;
    bool bridges = false;

    explicit Bridge(const std::string &base, std::uint32_t width = 4, std::uint32_t height = 4) {
        Pelican::setupLogger();
        auto &launch = GET_MODULE(Pelican::EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent = vk::Extent2D{64, 64};
        launch.output_spout = true;
        if (std::getenv("PELICAN_VO1A_VULKAN_VALIDATION") != nullptr) {
            launch.vulkan_validation = Pelican::VulkanValidationMode::enabled;
        }
        vulkan = &Pelican::TestSupport::requireVulkanDevice("VO-5b");
        if (std::getenv("PELICAN_VO1A_VULKAN_VALIDATION") != nullptr) {
            REQUIRE(vulkan->getVulkanValidationStatus().enabled);
        }
        const auto request = gpuRequest(*vulkan);
        if (!request.allowed) {
            if (expectsGpu()) FAIL("the preflight refused on the machine with the gate");
            SKIP("the preflight refuses on this machine: nothing here ran");
        }
        name = uniqueName(base);
        backend = Pelican::outputSpoutRealBackendFactory(request)();
        const auto opened = backend->open(name);
        REQUIRE(opened.kind == Pelican::OutputSpoutResultKind::ok);
        gpu = backend->gpu();
        REQUIRE(gpu != nullptr);
        const auto ready = gpu->prepare(request, semaphore);
        INFO(ready.reason << ": " << ready.detail);
        REQUIRE(ready.failure == OutputSpoutGpuFailure::none);
        prepared = true;
        const auto created = gpu->createBridges(width, height, images);
        INFO(created.reason << ": " << created.detail);
        REQUIRE(created.failure == OutputSpoutGpuFailure::none);
        bridges = true;
        const auto device = vulkan->getDevice();
        pool = device.createCommandPoolUnique(
            {vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
             vulkan->getGraphicsQueueFamilyIndex()});
        command_buffer = std::move(device.allocateCommandBuffersUnique(
            {pool.get(), vk::CommandBufferLevel::ePrimary, 1}).front());
        fence = device.createFenceUnique({});
    }

    ~Bridge() {
        if (vulkan != nullptr) vulkan->getDevice().waitIdle();
        if (gpu != nullptr && bridges) gpu->destroyBridges();
        if (gpu != nullptr && prepared) gpu->releaseGpu();
        if (backend) backend->close();
    }

    // The production barriers and submit around a clear of the whole image.
    void writeAndSignal(std::size_t index, std::array<std::uint8_t, 4> rgba, std::uint64_t value) {
        const auto cb = command_buffer.get();
        const auto family = vulkan->getGraphicsQueueFamilyIndex();
        cb.reset();
        cb.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        Pelican::recordOutputBridgeAcquire(cb, vk::Image{images[index]}, family);
        const vk::ClearColorValue color{std::array<float, 4>{
            static_cast<float>(rgba[0]) / 255.0f, static_cast<float>(rgba[1]) / 255.0f,
            static_cast<float>(rgba[2]) / 255.0f, static_cast<float>(rgba[3]) / 255.0f}};
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        cb.clearColorImage(vk::Image{images[index]}, vk::ImageLayout::eTransferDstOptimal, color,
                           range);
        Pelican::recordOutputBridgeRelease(cb, vk::Image{images[index]}, family);
        cb.end();
        vulkan->getDevice().resetFences(fence.get());
        Pelican::submitOutputBridgeCommands(vulkan->getGraphicsQueue(), cb, fence.get(),
                                            vk::Semaphore{semaphore}, value);
    }

    void waitVulkan() {
        REQUIRE(vulkan->getDevice().waitForFences(fence.get(), vk::True, 5'000'000'000ull) ==
                vk::Result::eSuccess);
    }
};

std::vector<std::uint8_t> solidBgra(std::array<std::uint8_t, 4> rgba) {
    std::vector<std::uint8_t> bytes;
    for (int index = 0; index < 16; ++index) {
        bytes.push_back(rgba[2]);
        bytes.push_back(rgba[1]);
        bytes.push_back(rgba[0]);
        bytes.push_back(rgba[3]);
    }
    return bytes;
}
} // namespace

TEST_CASE("VO-5b the real bridge is created and torn down, with the real facts", "[vo5b][gpu]") {
    Bridge bridge{"facts", 64, 32};
    CHECK(bridge.semaphore != VK_NULL_HANDLE);
    for (const auto image : bridge.images) CHECK(image != VK_NULL_HANDLE);
    CHECK(bridge.images[0] != bridge.images[1]);
}

// P0-3. D3D11's wait and copy are queued BEFORE Vulkan has even submitted, so no
// fence can hide them: only the imported semaphore can hold D3D11 back.
TEST_CASE("VO-5b D3D11 can't see a frame until the imported semaphore reaches its value",
          "[vo5b][gpu]") {
    Bridge bridge{"semaphore"};
    SpoutTestReceiver receiver{bridge.name};
    REQUIRE(receiver.deviceAvailable());
    std::uint64_t value = 0;
    for (int round = 0; round < 8; ++round) {
        CAPTURE(round);
        const auto index = static_cast<std::size_t>(round % 2);
        const std::array<std::uint8_t, 4> color{
            static_cast<std::uint8_t>(40 + round * 20), static_cast<std::uint8_t>(200 - round * 10),
            static_cast<std::uint8_t>(7 + round), static_cast<std::uint8_t>(90 + round)};
        ++value;
        const auto sent = bridge.gpu->sendBridge(index, value);
        REQUIRE(sent.result.kind == Pelican::OutputSpoutResultKind::ok);
        REQUIRE(sent.completion_enqueued);
        // D3D11 is stopped at its wait: its completion can't pass while Vulkan hasn't
        // even been asked to write. Without the D3D11 wait this reads done.
        const auto blocked = bridge.gpu->waitSent(250ms);
        CHECK(blocked == OutputSpoutSentState::timeout);
        bridge.writeAndSignal(index, color, value);
        // Without the Vulkan signal this stays a timeout.
        const auto finished = bridge.gpu->waitSent(5s);
        CHECK(finished == OutputSpoutSentState::done);
        bridge.waitVulkan();
        // And what D3D11 copied is what Vulkan wrote after D3D11 was queued.
        const auto expected = solidBgra(color);
        const auto received = waitUntil([&] {
            (void)receiver.poll();
            return receiver.pixels() == expected;
        });
        CHECK(received);
    }
}

TEST_CASE("VO-5b 100 values cross from Vulkan to Spout's texture through both bridges",
          "[vo5b][gpu]") {
    Bridge bridge{"values"};
    SpoutTestReceiver receiver{bridge.name};
    REQUIRE(receiver.deviceAvailable());
    int mismatches = 0;
    for (int round = 0; round < 100; ++round) {
        const auto index = static_cast<std::size_t>(round % 2);
        const std::array<std::uint8_t, 4> color{
            static_cast<std::uint8_t>(round * 2 + 1), static_cast<std::uint8_t>(255 - round),
            static_cast<std::uint8_t>((round * 7) % 256), static_cast<std::uint8_t>(30 + round)};
        const auto value = static_cast<std::uint64_t>(round) + 1;
        bridge.writeAndSignal(index, color, value);
        bridge.waitVulkan();
        const auto sent = bridge.gpu->sendBridge(index, value);
        REQUIRE(sent.result.kind == Pelican::OutputSpoutResultKind::ok);
        REQUIRE(bridge.gpu->waitSent(5s) == OutputSpoutSentState::done);
        const auto expected = solidBgra(color);
        if (!waitUntil([&] {
                (void)receiver.poll();
                return receiver.pixels() == expected;
            })) ++mismatches;
    }
    CHECK(mismatches == 0);
}

TEST_CASE("VO-5b a D3D11 wait that nothing signals stays incomplete, whatever the limit",
          "[vo5b][gpu]") {
    // A bridge in this state has work in flight: the test never destroys it early,
    // so it signals at the end, through Vulkan, to let its own teardown be safe.
    Bridge bridge{"unresolved"};
    const auto sent = bridge.gpu->sendBridge(0, 1);
    REQUIRE(sent.completion_enqueued);
    for (int attempt = 0; attempt < 3; ++attempt) {
        CHECK(bridge.gpu->waitSent(100ms) == OutputSpoutSentState::timeout);
    }
    bridge.writeAndSignal(0, {1, 2, 3, 4}, 1);
    CHECK(bridge.gpu->waitSent(5s) == OutputSpoutSentState::done);
    bridge.waitVulkan();
}

TEST_CASE("VO-5b a failed SendTexture keeps its queued wait tracked", "[vo5b][gpu]") {
    setFault("send_false");
    struct Clear { ~Clear() { setFault(""); } } clear;
    Bridge bridge{"send_false"};
    // Real: a real D3D11 wait is queued on a value Vulkan hasn't signalled. The
    // reported failure must not let the bridge go before that wait has passed.
    const auto sent = bridge.gpu->sendBridge(0, 1);
    CHECK(sent.result.kind == Pelican::OutputSpoutResultKind::failed);
    CHECK(sent.completion_enqueued);
    CHECK(bridge.gpu->waitSent(250ms) == OutputSpoutSentState::timeout);
    bridge.writeAndSignal(0, {1, 2, 3, 4}, 1);
    CHECK(bridge.gpu->waitSent(5s) == OutputSpoutSentState::done);
    bridge.waitVulkan();
}

TEST_CASE("VO-5b a real WARP device is refused by the real LUID compare", "[vo5b][gpu]") {
    setFault("warp_device");
    struct Clear { ~Clear() { setFault(""); } } clear;
    FastModuleContainer modules;
    Pelican::setupLogger();
    auto &launch = GET_MODULE(Pelican::EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{64, 64};
    launch.output_spout = true;
    auto &vulkan = Pelican::TestSupport::requireVulkanDevice("VO-5b");
    const auto request = gpuRequest(vulkan);
    if (!request.allowed) {
        if (expectsGpu()) FAIL("the preflight refused on the machine with the gate");
        SKIP("the preflight refuses on this machine: nothing here ran");
    }
    auto backend = Pelican::outputSpoutRealBackendFactory(request)();
    REQUIRE(backend->open(uniqueName("warp")).kind == Pelican::OutputSpoutResultKind::ok);
    VkSemaphore semaphore = VK_NULL_HANDLE;
    const auto step = backend->gpu()->prepare(request, semaphore);
    INFO(step.detail);
    CHECK(step.failure == OutputSpoutGpuFailure::permanent);
    CHECK(step.reason == "sender_device_mismatch");
    backend->close();
}

TEST_CASE("VO-5b a real extent over the import limit stays on CPU, with a real sender",
          "[vo5b][gpu]") {
    FastModuleContainer modules;
    Pelican::setupLogger();
    auto &launch = GET_MODULE(Pelican::EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{64, 64};
    launch.output_spout = true;
    auto &vulkan = Pelican::TestSupport::requireVulkanDevice("VO-5b");
    auto request = gpuRequest(vulkan);
    if (!request.allowed) {
        if (expectsGpu()) FAIL("the preflight refused on the machine with the gate");
        SKIP("the preflight refuses on this machine: nothing here ran");
    }
    const auto too_wide = request.max_width + 1;
    Pelican::OutputSpoutSender::Options options;
    options.name = uniqueName("extent");
    options.factory = Pelican::outputSpoutRealBackendFactory(request);
    options.gpu = request;
    Pelican::OutputSpoutSender sender{std::move(options)};
    const auto reached = waitUntil([&] {
        (void)sender.reserveGpu(too_wide, 4, Pelican::OutputSpoutSourceFormat::bgra8_unorm);
        return sender.status().transport_reason == "extent_unsupported";
    });
    INFO(sender.status().transport_detail);
    CHECK(reached);
    CHECK(sender.status().gpu_fallbacks == 0);
    CHECK(sender.reserveGpu(too_wide, 4, Pelican::OutputSpoutSourceFormat::bgra8_unorm).route ==
          Pelican::OutputSpoutRoute::cpu);
}

TEST_CASE("VO-5b the startup check of the RGBA8 blit, all 256 values of every channel",
          "[vo5b][gpu]") {
    FastModuleContainer modules;
    Pelican::setupLogger();
    auto &launch = GET_MODULE(Pelican::EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{64, 64};
    if (std::getenv("PELICAN_VO1A_VULKAN_VALIDATION") != nullptr) {
        launch.vulkan_validation = Pelican::VulkanValidationMode::enabled;
    }
    auto &vulkan = Pelican::TestSupport::requireVulkanDevice("VO-5b");
    const auto result = Pelican::verifyOutputRgba8Blit(vulkan);
    WARN("rgba8 blit exact: " << result.exact << " " << result.detail);
    // Whichever way it comes out, the route follows it. On this machine it's a gate.
    if (expectsGpu()) CHECK(result.exact);
    CHECK((result.exact == result.detail.empty()));
}
