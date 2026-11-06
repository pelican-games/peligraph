// VO-5a: what really reaches the Vulkan create calls. Real Vulkan on both paths;
// the XR path runs through a fake OpenXR runtime that calls real Vulkan, the
// real runtime isn't needed, and it can refuse the device like a real one could.
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/extensionrequests.hpp"
#if PELICAN_WITH_OPENXR
#include "../src/core/openxr/openxrdiscovery.hpp"
#endif
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using Pelican::FastModuleContainer;

namespace {
using Pelican::VulkanExtensionNeed;
using Pelican::VulkanExtensionRequests;
using Pelican::VulkanExtensionRow;
using Names = std::vector<std::string>;
constexpr const char *absent_name = "VK_EXT_pelican_vo5a_absent_fixture";

bool has(const Names &names, const std::string &name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// Clears the record and the extra rows, before and after a test.
struct ExtraRows {
    explicit ExtraRows(std::optional<VulkanExtensionRequests> requests = std::nullopt) {
        Pelican::vulkanCreationRecordForTesting() = {};
        Pelican::setExtraVulkanExtensionRequestsForTesting(std::move(requests));
    }
    ~ExtraRows() {
        Pelican::setExtraVulkanExtensionRequestsForTesting(std::nullopt);
        Pelican::vulkanCreationRecordForTesting() = {};
    }
};

void headlessConfig() {
    auto &launch_config = GET_MODULE(Pelican::EngineLaunchConfig);
    launch_config.headless = true;
    launch_config.headless_extent = vk::Extent2D{64, 64};
}
} // namespace

TEST_CASE("VO-5a flat: the exact names at the create calls are the report's", "[vo5a][gpu]") {
    Pelican::setupLogger();
    ExtraRows extra;
    Pelican::FastModuleContainer modules;
    headlessConfig();
    auto &vulkan = Pelican::TestSupport::requireVulkanDevice("VO-5a");
    const auto &report = vulkan.getVulkanExtensionReport();
    const auto &record = Pelican::vulkanCreationRecordForTesting();
    REQUIRE(record.instance.size() == 1);
    REQUIRE(record.device.size() == 1);
    CHECK(record.instance.back() == report.instance.enabled);
    CHECK(record.device.back() == report.device.enabled);
    CHECK(report.instance.rows.empty());
    CHECK(report.device.rows.empty());
    CHECK(report.device_created_by == "engine");
}

TEST_CASE("VO-5a flat: an unsupported optional row is skipped, a supported one is passed last",
          "[vo5a][gpu]") {
    Pelican::setupLogger();
    Names base_instance;
    {
        ExtraRows extra;
        Pelican::FastModuleContainer modules;
        headlessConfig();
        base_instance = Pelican::TestSupport::requireVulkanDevice("VO-5a")
                            .getVulkanExtensionReport()
                            .instance.enabled;
    }
    std::optional<std::string> supported_row;
    for (const auto &extension : vk::enumerateInstanceExtensionProperties()) {
        const std::string name = extension.extensionName.data();
        if (!has(base_instance, name)) {
            supported_row = name;
            break;
        }
    }
    REQUIRE(supported_row.has_value());

    VulkanExtensionRequests requests;
    requests.requestInstance(absent_name, VulkanExtensionNeed::optional, "test");
    requests.requestInstance(*supported_row, VulkanExtensionNeed::optional, "test");
    requests.requestDevice(absent_name, VulkanExtensionNeed::optional, "test");
    ExtraRows extra{requests};
    Pelican::FastModuleContainer modules;
    headlessConfig();
    auto &vulkan = Pelican::TestSupport::requireVulkanDevice("VO-5a");
    const auto &report = vulkan.getVulkanExtensionReport();
    const auto &record = Pelican::vulkanCreationRecordForTesting();

    auto expected = base_instance;
    expected.push_back(*supported_row);
    REQUIRE(record.instance.size() == 1);
    CHECK(record.instance.back() == expected);
    REQUIRE(report.instance.rows.size() == 2);
    CHECK(report.instance.rows[0].outcome == VulkanExtensionRow::Outcome::skipped_unsupported);
    CHECK(report.instance.rows[1].outcome == VulkanExtensionRow::Outcome::enabled);
    REQUIRE(record.device.size() == 1);
    CHECK_FALSE(has(record.device.back(), absent_name));
    REQUIRE(report.device.rows.size() == 1);
    CHECK(report.device.rows[0].outcome == VulkanExtensionRow::Outcome::skipped_unsupported);
}

TEST_CASE("VO-5a flat: a missing required row refuses by name at the real constructor",
          "[vo5a][gpu]") {
    Pelican::setupLogger();
    SECTION("device") {
        VulkanExtensionRequests requests;
        requests.requestDevice(absent_name, VulkanExtensionNeed::required, "test_requester");
        ExtraRows extra{requests};
        Pelican::FastModuleContainer modules;
        headlessConfig();
        CHECK_THROWS_WITH(GET_MODULE(Pelican::VulkanManageCore),
                          Catch::Matchers::ContainsSubstring(
                              "vulkan.required_extension_missing@1: device extension") &&
                              Catch::Matchers::ContainsSubstring(absent_name) &&
                              Catch::Matchers::ContainsSubstring("test_requester"));
        CHECK(Pelican::vulkanCreationRecordForTesting().device.empty());
    }
    SECTION("instance") {
        VulkanExtensionRequests requests;
        requests.requestInstance(absent_name, VulkanExtensionNeed::required, "test_requester");
        ExtraRows extra{requests};
        Pelican::FastModuleContainer modules;
        headlessConfig();
        CHECK_THROWS_WITH(GET_MODULE(Pelican::VulkanManageCore),
                          Catch::Matchers::ContainsSubstring(
                              "vulkan.required_extension_missing@1: instance extension") &&
                              Catch::Matchers::ContainsSubstring(absent_name));
        CHECK(Pelican::vulkanCreationRecordForTesting().instance.empty());
    }
}

#if PELICAN_WITH_OPENXR
namespace {

// A fake runtime over real Vulkan.
struct FakeXr {
    bool reject_device = false;
    std::vector<Names> instance_names;
    std::vector<Names> device_names;
};
FakeXr *active_xr = nullptr;

XrInstance fakeXrInstance() { return reinterpret_cast<XrInstance>(std::uintptr_t{0x505}); }

Names readNames(std::uint32_t count, const char *const *names) {
    Names result;
    for (std::uint32_t index = 0; index < count; ++index) result.emplace_back(names[index]);
    return result;
}

XrResult XRAPI_CALL fakeDestroyInstance(XrInstance) { return XR_SUCCESS; }

XrResult XRAPI_CALL fakeGetSystem(XrInstance, const XrSystemGetInfo *, XrSystemId *system_id) {
    *system_id = 17;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeGetRequirements(XrInstance, XrSystemId,
                                        XrGraphicsRequirementsVulkanKHR *requirements) {
    requirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    requirements->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 999);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeCreateVulkanInstance(XrInstance, const XrVulkanInstanceCreateInfoKHR *info,
                                             VkInstance *instance, VkResult *result) {
    active_xr->instance_names.push_back(readNames(
        info->vulkanCreateInfo->enabledExtensionCount,
        info->vulkanCreateInfo->ppEnabledExtensionNames));
    *result = vkCreateInstance(info->vulkanCreateInfo, nullptr, instance);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeGetGraphicsDevice(XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR *info,
                                          VkPhysicalDevice *physical_device) {
    std::uint32_t count = 1;
    const auto result = vkEnumeratePhysicalDevices(info->vulkanInstance, &count, physical_device);
    return result == VK_SUCCESS || result == VK_INCOMPLETE ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}

XrResult XRAPI_CALL fakeCreateVulkanDevice(XrInstance, const XrVulkanDeviceCreateInfoKHR *info,
                                           VkDevice *device, VkResult *result) {
    active_xr->device_names.push_back(readNames(
        info->vulkanCreateInfo->enabledExtensionCount,
        info->vulkanCreateInfo->ppEnabledExtensionNames));
    if (active_xr->reject_device) return XR_ERROR_RUNTIME_FAILURE;
    *result = vkCreateDevice(info->vulkanPhysicalDevice, info->vulkanCreateInfo, nullptr, device);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeGetProcAddr(XrInstance, const char *name, PFN_xrVoidFunction *function) {
    const std::string text{name};
    if (text == "xrDestroyInstance") *function = reinterpret_cast<PFN_xrVoidFunction>(&fakeDestroyInstance);
    else if (text == "xrGetSystem") *function = reinterpret_cast<PFN_xrVoidFunction>(&fakeGetSystem);
    else if (text == "xrGetVulkanGraphicsRequirements2KHR") *function = reinterpret_cast<PFN_xrVoidFunction>(&fakeGetRequirements);
    else if (text == "xrCreateVulkanInstanceKHR") *function = reinterpret_cast<PFN_xrVoidFunction>(&fakeCreateVulkanInstance);
    else if (text == "xrGetVulkanGraphicsDevice2KHR") *function = reinterpret_cast<PFN_xrVoidFunction>(&fakeGetGraphicsDevice);
    else if (text == "xrCreateVulkanDeviceKHR") *function = reinterpret_cast<PFN_xrVoidFunction>(&fakeCreateVulkanDevice);
    else *function = nullptr;
    return *function == nullptr ? XR_ERROR_FUNCTION_UNSUPPORTED : XR_SUCCESS;
}

XrResult XRAPI_CALL fakeEnumerate(const char *, std::uint32_t capacity, std::uint32_t *count,
                                  XrExtensionProperties *properties) {
    *count = 1;
    if (capacity != 0) {
        std::snprintf(properties[0].extensionName, sizeof(properties[0].extensionName), "%s",
                      XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        properties[0].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeCreateInstance(const XrInstanceCreateInfo *, XrInstance *instance) {
    *instance = fakeXrInstance();
    return XR_SUCCESS;
}

// Puts the fake in the module's place, as if discovery had found a runtime.
struct FakeDiscovery {
    FakeXr fake;
    FakeDiscovery() {
        active_xr = &fake;
        using Pelican::OpenXr::DiscoveryRuntime;
        DiscoveryRuntime::__get().emplace(Pelican::OpenXr::XrApi{
            .get_instance_proc_addr = &fakeGetProcAddr,
            .enumerate_instance_extension_properties = &fakeEnumerate,
            .create_instance = &fakeCreateInstance,
        });
        DiscoveryRuntime::__ready().store(true);
        REQUIRE(DiscoveryRuntime::__get()->discover().availability ==
                Pelican::XrDiscoveryAvailability::available);
    }
    ~FakeDiscovery() {
        using Pelican::OpenXr::DiscoveryRuntime;
        DiscoveryRuntime::__ready().store(false);
        DiscoveryRuntime::__get().reset();
        active_xr = nullptr;
    }
};

void xrConfig(Pelican::XrMode requested) {
    headlessConfig();
    auto &launch_config = GET_MODULE(Pelican::EngineLaunchConfig);
    launch_config.xr_requested_mode = requested;
    launch_config.xr_mode = Pelican::XrMode::on;
    launch_config.xr_active = true;
    // Headless, XR's instance list is empty unless debug utils are asked for,
    // and an empty list would leave nothing for these cases to look at.
    launch_config.gpu_labels = true;
#if PELICAN_WITH_SPOUT
    launch_config.output_spout = true;
#endif
}

void checkSpoutRowsIn(const Names &names) {
#if PELICAN_WITH_SPOUT
    CHECK(has(names, "VK_KHR_external_memory_win32"));
    CHECK(has(names, "VK_KHR_external_semaphore_win32"));
    CHECK(has(names, "VK_KHR_win32_keyed_mutex"));
#else
    (void)names;
#endif
}

bool debugUtilsSupported() {
    for (const auto &extension : vk::enumerateInstanceExtensionProperties()) {
        if (std::string{extension.extensionName.data()} == VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
            return true;
    }
    return false;
}

bool anyOpenXrRow(const std::vector<VulkanExtensionRow> &rows) {
    return std::any_of(rows.begin(), rows.end(), [](const VulkanExtensionRow &row) {
        return std::find(row.requested_by.begin(), row.requested_by.end(), "openxr") !=
               row.requested_by.end();
    });
}

} // namespace

TEST_CASE("VO-5a XR: the runtime gets the exact names, XR's rows are required, and the report says so",
          "[vo5a][gpu]") {
    Pelican::setupLogger();
    VulkanExtensionRequests requests;
    requests.requestDevice(absent_name, VulkanExtensionNeed::optional, "test");
    ExtraRows extra{requests};
    FakeDiscovery discovery;
    Pelican::FastModuleContainer modules;
    xrConfig(Pelican::XrMode::auto_mode);
    auto &vulkan = Pelican::TestSupport::requireVulkanDevice("VO-5a");

    // An unsupported optional row doesn't cause a fallback.
    CHECK(GET_MODULE(Pelican::EngineLaunchConfig).xr_active);
    const auto &report = vulkan.getVulkanExtensionReport();
    const auto &record = Pelican::vulkanCreationRecordForTesting();
    CHECK(report.device_created_by == "openxr");
    REQUIRE(discovery.fake.instance_names.size() == 1);
    REQUIRE(discovery.fake.device_names.size() == 1);
    REQUIRE(record.instance.size() == 1);
    REQUIRE(record.device.size() == 1);
    CHECK(discovery.fake.instance_names.back() == record.instance.back());
    CHECK(discovery.fake.device_names.back() == record.device.back());
    CHECK(record.instance.back() == report.instance.enabled);
    CHECK(record.device.back() == report.device.enabled);
    checkSpoutRowsIn(record.device.back());
    CHECK_FALSE(has(record.device.back(), absent_name));
    for (const auto &row : report.instance.rows) {
        CHECK(row.need == VulkanExtensionNeed::required);
        CHECK(row.requested_by == Names{"openxr"});
    }
    CHECK(report.instance.rows.size() == report.instance.enabled.size());
    if (debugUtilsSupported()) {
        REQUIRE_FALSE(report.instance.rows.empty());
        CHECK(has(report.instance.enabled, VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
        CHECK(anyOpenXrRow(report.instance.rows));
    }
    const auto json = Pelican::vulkanExtensionReportJson(report);
    CHECK(json.at("list_scope") == "engine_requested");
    CHECK(json.at("device_created_by") == "openxr");
}

TEST_CASE("VO-5a XR: a runtime rejecting supported rows falls back once, and the flat report has nothing of XR's",
          "[vo5a][gpu]") {
    Pelican::setupLogger();
    ExtraRows extra;
    FakeDiscovery discovery;
    discovery.fake.reject_device = true;
    Pelican::FastModuleContainer modules;
    xrConfig(Pelican::XrMode::auto_mode);
    auto &vulkan = Pelican::TestSupport::requireVulkanDevice("VO-5a");

    CHECK_FALSE(GET_MODULE(Pelican::EngineLaunchConfig).xr_active);
    // The runtime was asked once, with the rows; no retry with fewer.
    REQUIRE(discovery.fake.device_names.size() == 1);
    checkSpoutRowsIn(discovery.fake.device_names.back());

    const auto &record = Pelican::vulkanCreationRecordForTesting();
    REQUIRE(record.instance.size() == 2);
    REQUIRE(record.device.size() == 2);
    const auto &report = vulkan.getVulkanExtensionReport();
    CHECK(report.device_created_by == "engine");
    CHECK(record.instance.back() == report.instance.enabled);
    CHECK(record.device.back() == report.device.enabled);
    // The XR attempt did have its rows, and they're gone.
    if (debugUtilsSupported()) {
        CHECK(has(discovery.fake.instance_names.back(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
    }
    CHECK(report.instance.rows.empty());
    CHECK_FALSE(anyOpenXrRow(report.instance.rows));
    CHECK_FALSE(anyOpenXrRow(report.device.rows));
    checkSpoutRowsIn(record.device.back());
}

TEST_CASE("VO-5a XR: with --xr on a rejected device is an error, not a fallback", "[vo5a][gpu]") {
    Pelican::setupLogger();
    ExtraRows extra;
    FakeDiscovery discovery;
    discovery.fake.reject_device = true;
    Pelican::FastModuleContainer modules;
    xrConfig(Pelican::XrMode::on);
    CHECK_THROWS_WITH(GET_MODULE(Pelican::VulkanManageCore),
                      Catch::Matchers::ContainsSubstring("--xr on"));
    CHECK(discovery.fake.device_names.size() == 1);
    CHECK(Pelican::vulkanCreationRecordForTesting().device.size() == 1);
}

TEST_CASE("VO-5a XR: a required row missing in XR falls back under auto, and the flat path refuses it too",
          "[vo5a][gpu]") {
    Pelican::setupLogger();
    VulkanExtensionRequests requests;
    requests.requestDevice(absent_name, VulkanExtensionNeed::required, "test_requester");
    ExtraRows extra{requests};
    FakeDiscovery discovery;
    Pelican::FastModuleContainer modules;
    xrConfig(Pelican::XrMode::auto_mode);
    CHECK_THROWS_WITH(GET_MODULE(Pelican::VulkanManageCore),
                      Catch::Matchers::ContainsSubstring("device extension") &&
                          Catch::Matchers::ContainsSubstring(absent_name));
    // It never reached a create call, on either path.
    CHECK(discovery.fake.device_names.empty());
    CHECK(Pelican::vulkanCreationRecordForTesting().device.empty());
    // XR gave up, then flat refused.
    CHECK_FALSE(GET_MODULE(Pelican::EngineLaunchConfig).xr_active);
    CHECK(Pelican::vulkanCreationRecordForTesting().instance.size() == 2);
}
#endif
