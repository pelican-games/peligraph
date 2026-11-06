// VO-5a: the real capability source. Its Vulkan calls read through recording
// functions, then real Vulkan, real DXGI and a real spoutDX device.
// PELICAN_VO5A_EXPECT_AVAILABLE=1 turns on a machine-specific acceptance gate;
// the other checks are API invariants and hold on any machine.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <SpoutDX.h>
#include <dxgi.h>

#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/output/outputspoutgpusource.hpp"
#include "../src/core/vkcore/core.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Pelican::FastModuleContainer;

namespace {
using Pelican::OutputSpoutHandleKind;

struct ImportCall {
    VkStructureType info_type{};
    VkFormat format{};
    VkImageType type{};
    VkImageTiling tiling{};
    VkImageUsageFlags usage{};
    VkImageCreateFlags flags{};
    VkStructureType external_info_type{};
    VkExternalMemoryHandleTypeFlagBits handle{};
    VkStructureType properties_type{};
    VkStructureType external_properties_type{};
};

// What the recording functions saw and what they answer.
struct Recording {
    VkPhysicalDevice device_seen = VK_NULL_HANDLE;
    std::vector<ImportCall> imports;
    VkResult nt_result = VK_SUCCESS;
    VkResult kmt_result = VK_SUCCESS;
    VkExternalMemoryFeatureFlags features = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    VkExternalSemaphoreHandleTypeFlagBits semaphore_handle{};
    VkStructureType semaphore_info_type{};
    VkStructureType semaphore_type_type{};
    VkSemaphoreType semaphore_type = VK_SEMAPHORE_TYPE_BINARY;
    VkStructureType semaphore_properties_type{};
    VkFormat format_asked{};
};
Recording *recording = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL recordImageFormat(VkPhysicalDevice device,
                                                 const VkPhysicalDeviceImageFormatInfo2 *info,
                                                 VkImageFormatProperties2 *properties) {
    recording->device_seen = device;
    ImportCall call;
    call.info_type = info->sType;
    call.format = info->format;
    call.type = info->type;
    call.tiling = info->tiling;
    call.usage = info->usage;
    call.flags = info->flags;
    const auto *external =
        static_cast<const VkPhysicalDeviceExternalImageFormatInfo *>(info->pNext);
    if (external != nullptr) {
        call.external_info_type = external->sType;
        call.handle = external->handleType;
    }
    call.properties_type = properties->sType;
    auto *external_properties =
        static_cast<VkExternalImageFormatProperties *>(properties->pNext);
    if (external_properties != nullptr) {
        call.external_properties_type = external_properties->sType;
        // Written on failure too: the source must not read it unless it succeeded.
        external_properties->externalMemoryProperties.externalMemoryFeatures = recording->features;
    }
    properties->imageFormatProperties.maxExtent = {1234, 5678, 1};
    recording->imports.push_back(call);
    return call.handle == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT
               ? recording->nt_result
               : recording->kmt_result;
}

VKAPI_ATTR void VKAPI_CALL recordFormat(VkPhysicalDevice, VkFormat format,
                                        VkFormatProperties *properties) {
    recording->format_asked = format;
    properties->optimalTilingFeatures = VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    // Linear tiling claims more: the source must read the optimal column.
    properties->linearTilingFeatures = VK_FORMAT_FEATURE_BLIT_DST_BIT;
}

VKAPI_ATTR void VKAPI_CALL recordSemaphore(VkPhysicalDevice,
                                           const VkPhysicalDeviceExternalSemaphoreInfo *info,
                                           VkExternalSemaphoreProperties *properties) {
    recording->semaphore_info_type = info->sType;
    recording->semaphore_handle = info->handleType;
    recording->semaphore_properties_type = properties->sType;
    const auto *type = static_cast<const VkSemaphoreTypeCreateInfo *>(info->pNext);
    if (type != nullptr) {
        recording->semaphore_type_type = type->sType;
        recording->semaphore_type = type->semaphoreType;
    }
    properties->externalSemaphoreFeatures = VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
}

VKAPI_ATTR void VKAPI_CALL recordProperties(VkPhysicalDevice, VkPhysicalDeviceProperties2 *properties) {
    auto *id = static_cast<VkPhysicalDeviceIDProperties *>(properties->pNext);
    REQUIRE(id != nullptr);
    CHECK(id->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
    id->deviceLUIDValid = VK_TRUE;
    for (std::uint8_t index = 0; index < VK_LUID_SIZE; ++index) id->deviceLUID[index] = index + 1;
}

Pelican::OutputSpoutVulkanQueries recordingQueries() {
    return {recordImageFormat, recordFormat, recordSemaphore, recordProperties};
}

VkPhysicalDevice fakeDevice() {
    return reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(0x1234));
}

bool hasName(std::span<const std::string> names, const char *name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

Pelican::OutputSpoutLuid readVulkanLuid(VkPhysicalDevice device) {
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(device, &properties);
    Pelican::OutputSpoutLuid luid;
    luid.valid = id.deviceLUIDValid == VK_TRUE;
    std::memcpy(luid.bytes.data(), id.deviceLUID, luid.bytes.size());
    return luid;
}
} // namespace

TEST_CASE("VO-5a the real source sends the right chains, usage, semaphore type and handle kinds",
          "[vo5a]") {
    Recording seen;
    recording = &seen;
    const auto source = Pelican::makeOutputSpoutGpuCapabilitySource(
        fakeDevice(), {"VK_name"}, true, recordingQueries());
    CHECK(source->enabledDeviceExtensions() == std::vector<std::string>{"VK_name"});
    CHECK(source->timelineSemaphoreEnabled());

    const auto nt = source->queryImport(OutputSpoutHandleKind::nt);
    const auto kmt = source->queryImport(OutputSpoutHandleKind::kmt);
    REQUIRE(seen.imports.size() == 2);
    CHECK(seen.device_seen == fakeDevice());
    CHECK(seen.imports[0].handle == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT);
    CHECK(seen.imports[1].handle == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT);
    for (const auto &call : seen.imports) {
        CHECK(call.info_type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2);
        CHECK(call.format == VK_FORMAT_B8G8R8A8_UNORM);
        CHECK(call.type == VK_IMAGE_TYPE_2D);
        CHECK(call.tiling == VK_IMAGE_TILING_OPTIMAL);
        CHECK(call.usage == VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        CHECK(call.flags == 0);
        CHECK(call.external_info_type ==
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
        CHECK(call.properties_type == VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2);
        CHECK(call.external_properties_type ==
              VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
    }
    CHECK(nt.result == "VK_SUCCESS");
    CHECK(nt.importable);
    CHECK_FALSE(nt.dedicated_only);
    CHECK(nt.max_width == 1234);
    CHECK(nt.max_height == 5678);
    CHECK(kmt.importable);

    const auto format = source->queryFormat();
    CHECK(seen.format_asked == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(format.transfer_dst);
    CHECK_FALSE(format.blit_dst);

    CHECK(source->semaphoreImportable());
    CHECK(seen.semaphore_info_type == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO);
    CHECK(seen.semaphore_handle == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT);
    CHECK(seen.semaphore_type_type == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
    CHECK(seen.semaphore_type == VK_SEMAPHORE_TYPE_TIMELINE);
    CHECK(seen.semaphore_properties_type == VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES);

    const auto luid = source->deviceLuid();
    CHECK(luid.valid);
    CHECK(luid.bytes == std::array<std::uint8_t, 8>{1, 2, 3, 4, 5, 6, 7, 8});
    recording = nullptr;
}

TEST_CASE("VO-5a the real source reads features only on success and tells failures apart",
          "[vo5a]") {
    Recording seen;
    recording = &seen;
    seen.features = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    seen.nt_result = VK_ERROR_FORMAT_NOT_SUPPORTED;
    seen.kmt_result = VK_SUCCESS;
    const auto source = Pelican::makeOutputSpoutGpuCapabilitySource(
        fakeDevice(), {}, true, recordingQueries());
    const auto nt = source->queryImport(OutputSpoutHandleKind::nt);
    CHECK(nt.result == "VK_ERROR_FORMAT_NOT_SUPPORTED");
    CHECK_FALSE(nt.importable);
    CHECK_FALSE(nt.query_failed);
    CHECK(nt.max_width == 0);
    // The other handle's own good result stands.
    CHECK(source->queryImport(OutputSpoutHandleKind::kmt).importable);

    seen.nt_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    const auto failed = source->queryImport(OutputSpoutHandleKind::nt);
    CHECK(failed.result == "VK_ERROR_OUT_OF_HOST_MEMORY");
    CHECK(failed.query_failed);
    CHECK_FALSE(failed.importable);

    seen.nt_result = VK_ERROR_DEVICE_LOST;
    CHECK(source->queryImport(OutputSpoutHandleKind::nt).result == "VkResult(-4)");
    recording = nullptr;
}

TEST_CASE("VO-5a --output-spout reaches vkCreateDevice on this device", "[vo5a][gpu]") {
    Pelican::setupLogger();
    Pelican::vulkanCreationRecordForTesting() = {};
    Pelican::FastModuleContainer modules;
    auto &launch_config = GET_MODULE(Pelican::EngineLaunchConfig);
    launch_config.headless = true;
    launch_config.headless_extent = vk::Extent2D{64, 64};
    launch_config.output_spout = true;
    auto &vulkan = Pelican::TestSupport::requireVulkanDevice("VO-5a");

    const auto &report = vulkan.getVulkanExtensionReport();
    const auto &boundary = Pelican::vulkanCreationRecordForTesting().device;
    REQUIRE(boundary.size() == 1);
    CHECK(boundary.back() == report.device.enabled);
    const auto enabled = vulkan.getEnabledDeviceExtensions();
    CHECK(std::vector<std::string>(enabled.begin(), enabled.end()) == boundary.back());
    CHECK(report.device_created_by == "engine");
    REQUIRE(report.device.rows.size() == 3);

    const auto device = static_cast<VkDevice>(vulkan.getDevice());
    for (const auto &row : report.device.rows) {
        CAPTURE(row.name);
        CHECK((row.outcome == Pelican::VulkanExtensionRow::Outcome::enabled) ==
              hasName(boundary.back(), row.name.c_str()));
    }
    if (report.device.rows[0].outcome == Pelican::VulkanExtensionRow::Outcome::enabled) {
        // Only an extension really enabled at vkCreateDevice gives this.
        CHECK(vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR") != nullptr);
    }
    if (std::getenv("PELICAN_VO5A_EXPECT_AVAILABLE") != nullptr) {
        CHECK(report.device.rows[0].outcome == Pelican::VulkanExtensionRow::Outcome::enabled);
        CHECK(report.device.rows[1].outcome == Pelican::VulkanExtensionRow::Outcome::enabled);
        CHECK(report.device.rows[2].outcome == Pelican::VulkanExtensionRow::Outcome::enabled);
    }
}

TEST_CASE("VO-5a without the flag the device has no rows and none of the three", "[vo5a][gpu]") {
    Pelican::setupLogger();
    Pelican::vulkanCreationRecordForTesting() = {};
    Pelican::FastModuleContainer modules;
    auto &launch_config = GET_MODULE(Pelican::EngineLaunchConfig);
    launch_config.headless = true;
    launch_config.headless_extent = vk::Extent2D{64, 64};
    auto &vulkan = Pelican::TestSupport::requireVulkanDevice("VO-5a");

    const auto &report = vulkan.getVulkanExtensionReport();
    CHECK(report.device.rows.empty());
    CHECK(report.instance.rows.empty());
    const auto &boundary = Pelican::vulkanCreationRecordForTesting().device;
    REQUIRE(boundary.size() == 1);
    CHECK_FALSE(hasName(boundary.back(), "VK_KHR_external_memory_win32"));
    CHECK_FALSE(hasName(boundary.back(), "VK_KHR_external_semaphore_win32"));
    CHECK_FALSE(hasName(boundary.back(), "VK_KHR_win32_keyed_mutex"));
    const auto address = vkGetDeviceProcAddr(static_cast<VkDevice>(vulkan.getDevice()),
                                             "vkGetMemoryWin32HandleKHR");
    WARN("vkGetDeviceProcAddr with the extension off gives "
         << (address == nullptr ? "null" : "non-null"));
    // What this driver does, under the machine's gate: the loader doesn't promise it everywhere.
    if (std::getenv("PELICAN_VO5A_EXPECT_AVAILABLE") != nullptr) CHECK(address == nullptr);
}

TEST_CASE("VO-5a the real facts on this machine", "[vo5a][gpu]") {
    Pelican::setupLogger();
    Pelican::FastModuleContainer modules;
    auto &launch_config = GET_MODULE(Pelican::EngineLaunchConfig);
    launch_config.headless = true;
    launch_config.headless_extent = vk::Extent2D{64, 64};
    launch_config.output_spout = true;
    auto &vulkan = Pelican::TestSupport::requireVulkanDevice("VO-5a");
    const auto physical_device = static_cast<VkPhysicalDevice>(vulkan.getPhysDevice());

    const auto report = Pelican::outputSpoutGpuPathForRun(true, vulkan);
    const auto json = Pelican::outputSpoutGpuPathJson(report);
    WARN(json.dump(2));
    REQUIRE(report.facts.has_value());
    const auto &facts = *report.facts;

    // API invariants, on any machine.
    const auto luid = readVulkanLuid(physical_device);
    CHECK(facts.device_luid.valid == luid.valid);
    CHECK(facts.device_luid.bytes == luid.bytes);
    const auto usable_sync = facts.sync_keyed_mutex || facts.sync_external_semaphore;
    const auto preflight = facts.external_memory_enabled && facts.device_luid.valid &&
                           facts.adapter.ok && facts.device_luid.bytes == facts.adapter.luid &&
                           (facts.nt.importable || facts.kmt.importable) &&
                           facts.format.transfer_dst && usable_sync;
    CHECK((report.state == "available") == preflight);
    CHECK((report.reason == "none") == preflight);

    // The adapter the check reads is the adapter a default spoutDX really takes.
    spoutDX sender;
    (void)sender.OpenDirectX11();
    if (sender.GetDX11Device() == nullptr) {
        SKIP("no D3D11 device in this session: the adapter check did not run");
    }
    IDXGIDevice *dxgi_device = nullptr;
    REQUIRE(SUCCEEDED(sender.GetDX11Device()->QueryInterface(
        __uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device))));
    IDXGIAdapter *adapter = nullptr;
    REQUIRE(SUCCEEDED(dxgi_device->GetAdapter(&adapter)));
    DXGI_ADAPTER_DESC description{};
    REQUIRE(SUCCEEDED(adapter->GetDesc(&description)));
    std::array<std::uint8_t, 8> sender_luid{};
    std::memcpy(sender_luid.data(), &description.AdapterLuid, sender_luid.size());
    adapter->Release();
    dxgi_device->Release();
    sender.CloseDirectX11();
    REQUIRE(facts.adapter.ok);
    CHECK(facts.adapter.luid == sender_luid);

    // Observations of the RTX 5080 here, a named gate. Not driver requirements.
    if (std::getenv("PELICAN_VO5A_EXPECT_AVAILABLE") != nullptr) {
        CHECK(report.state == "available");
        CHECK(report.reason == "none");
        CHECK(facts.external_memory_enabled);
        CHECK(facts.external_semaphore_enabled);
        CHECK(facts.keyed_mutex_enabled);
        CHECK(facts.nt.importable);
        CHECK(facts.nt.dedicated_only);
        CHECK(facts.kmt.importable);
        CHECK(facts.kmt.dedicated_only);
        CHECK(facts.format.transfer_dst);
        CHECK(facts.format.blit_dst);
        CHECK(facts.sync_keyed_mutex);
        CHECK(facts.sync_external_semaphore);
        CHECK(facts.device_luid.bytes == facts.adapter.luid);
        CHECK(facts.adapter.name == "NVIDIA GeForce RTX 5080");
    }
}
