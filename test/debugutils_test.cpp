#include "../src/core/vkcore/debugutils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Pelican {
namespace {

std::vector<std::string> *active_trace = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL fakeSetObjectName(VkDevice, const VkDebugUtilsObjectNameInfoEXT *info) {
    if (active_trace != nullptr) {
        active_trace->push_back("object:" + std::string{info->pObjectName});
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fakeBeginCommandLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT *label) {
    if (active_trace != nullptr) {
        active_trace->push_back("begin:" + std::string{label->pLabelName});
    }
}

VKAPI_ATTR void VKAPI_CALL fakeEndCommandLabel(VkCommandBuffer) {
    if (active_trace != nullptr)
        active_trace->push_back("end");
}

VKAPI_ATTR void VKAPI_CALL fakeBeginQueueLabel(VkQueue, const VkDebugUtilsLabelEXT *label) {
    if (active_trace != nullptr) {
        active_trace->push_back("queue_begin:" + std::string{label->pLabelName});
    }
}

VKAPI_ATTR void VKAPI_CALL fakeEndQueueLabel(VkQueue) {
    if (active_trace != nullptr)
        active_trace->push_back("queue_end");
}

DebugUtilsFunctions fakeFunctions() {
    return {
        fakeSetObjectName, fakeBeginCommandLabel, fakeEndCommandLabel, fakeBeginQueueLabel, fakeEndQueueLabel,
    };
}

vk::CommandBuffer fakeCommandBuffer() {
    return vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(static_cast<std::uintptr_t>(1))};
}

vk::Device fakeDevice() { return vk::Device{reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(2))}; }

} // namespace

TEST_CASE("debug utils extension selection is optional and deterministic", "[debug-utils]") {
    const std::vector<std::string> supported{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };

    const auto off = selectDebugUtilsExtension(false, supported);
    REQUIRE(off.available);
    REQUIRE_FALSE(off.enabled);
    REQUIRE(off.reason == "disabled_by_launch_option");

    const auto on = selectDebugUtilsExtension(true, supported);
    REQUIRE(on.available);
    REQUIRE(on.enabled);
    REQUIRE(on.reason == "enabled");

    const auto unavailable = selectDebugUtilsExtension(true, std::span<const std::string>{supported.data(), 1});
    REQUIRE_FALSE(unavailable.available);
    REQUIRE_FALSE(unavailable.enabled);
    REQUIRE(unavailable.reason == "VK_EXT_debug_utils_unavailable");
}

TEST_CASE("frame graph labels preserve plan order and nested barriers and body", "[debug-utils][frame-plan]") {
    std::vector<std::string> trace;
    active_trace = &trace;
    const DebugUtilsDispatch debug_utils{fakeDevice(), {true, true, true, "enabled"}, fakeFunctions()};
    const auto command_buffer = fakeCommandBuffer();
    const std::vector<std::pair<std::string, std::string>> nodes{
        {"render", "opaque"},
        {"compute", "cull"},
        {"anchor", "__anchor_post_ldr"},
        {"snapshot_copy", "scene_snapshot"},
        {"output_transform", "output_transform"},
        {"mirror", "output_transform"},
    };

    for (std::size_t ordinal = 0; ordinal < nodes.size(); ++ordinal) {
        const auto &[kind, name] = nodes[ordinal];
        recordDebugLabeledNode(
            debug_utils, command_buffer, FrameGraphDebugLabelIdentity{42, "xr", 1, ordinal, kind, name},
            [&] { trace.push_back("barrier-work:" + name); }, [&] { trace.push_back("body-work:" + name); });
    }

    REQUIRE(trace.size() == nodes.size() * 8);
    for (std::size_t ordinal = 0; ordinal < nodes.size(); ++ordinal) {
        const auto offset = ordinal * 8;
        const auto &[kind, name] = nodes[ordinal];
        REQUIRE(trace[offset] ==
                "begin:frame/42/graph/xr/view/1/node/" + std::to_string(ordinal) + ":" + kind + ":" + name);
        REQUIRE(trace[offset + 1] == "begin:barriers");
        REQUIRE(trace[offset + 2] == "barrier-work:" + name);
        REQUIRE(trace[offset + 3] == "end");
        REQUIRE(trace[offset + 4] == "begin:body");
        REQUIRE(trace[offset + 5] == "body-work:" + name);
        REQUIRE(trace[offset + 6] == "end");
        REQUIRE(trace[offset + 7] == "end");
    }
    active_trace = nullptr;
}

TEST_CASE("disabled debug utils are a full no-op while node work still runs", "[debug-utils][no-op]") {
    std::vector<std::string> trace;
    active_trace = &trace;
    const DebugUtilsDispatch debug_utils{
        fakeDevice(), {true, false, false, "VK_EXT_debug_utils_unavailable"}, fakeFunctions()};

    recordDebugLabeledNode(
        debug_utils, fakeCommandBuffer(), FrameGraphDebugLabelIdentity{1, "flat", 0, 0, "render", "opaque"},
        [&] { trace.push_back("barriers"); }, [&] { trace.push_back("body"); });

    REQUIRE(trace == std::vector<std::string>{"barriers", "body"});
    active_trace = nullptr;
}

} // namespace Pelican
