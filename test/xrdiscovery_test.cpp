#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../src/core/openxr/openxrdiscovery.hpp"
#include "../src/core/vkcore/bootstrap.hpp"
#include "../src/core/xractivation.hpp"

#include <array>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct FakeRuntime {
    std::string failure;
    bool advertise_vulkan_enable2 = true;
    bool advertise_local_floor = false;
    bool advertise_time_conversion = false;
    VkPhysicalDevice selected_physical_device = VK_NULL_HANDLE;
    std::vector<std::string> calls;
};

thread_local FakeRuntime *active_fake = nullptr;

struct FakeScope {
    explicit FakeScope(FakeRuntime &fake) { active_fake = &fake; }
    ~FakeScope() { active_fake = nullptr; }
};

XrInstance fakeInstance() { return reinterpret_cast<XrInstance>(std::uintptr_t{0x101}); }

XrResult XRAPI_CALL fakeDestroyInstance(XrInstance) {
    active_fake->calls.emplace_back("destroy_instance");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeGetSystem(XrInstance, const XrSystemGetInfo *info, XrSystemId *system_id) {
    active_fake->calls.emplace_back("get_system");
    CHECK(info->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY);
    if (active_fake->failure == "get_system") return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    *system_id = 17;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeGetGraphicsRequirements(
    XrInstance, XrSystemId system_id, XrGraphicsRequirementsVulkanKHR *requirements) {
    active_fake->calls.emplace_back("get_graphics_requirements");
    CHECK(system_id == 17);
    if (active_fake->failure == "get_graphics_requirements") return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    requirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    requirements->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 999);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeCreateVulkanInstance(
    XrInstance, const XrVulkanInstanceCreateInfoKHR *, VkInstance *, VkResult *) {
    active_fake->calls.emplace_back("create_vulkan_instance");
    return XR_ERROR_RUNTIME_FAILURE;
}

XrResult XRAPI_CALL fakeGetVulkanGraphicsDevice(
    XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR *, VkPhysicalDevice *physical_device) {
    active_fake->calls.emplace_back("get_vulkan_graphics_device");
    *physical_device = active_fake->selected_physical_device;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeCreateVulkanDevice(
    XrInstance, const XrVulkanDeviceCreateInfoKHR *, VkDevice *, VkResult *) {
    active_fake->calls.emplace_back("create_vulkan_device");
    return XR_ERROR_RUNTIME_FAILURE;
}

PFN_xrVoidFunction fakeFunction(const std::string &name) {
    if (name == "xrDestroyInstance")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeDestroyInstance);
    if (name == "xrGetSystem") return reinterpret_cast<PFN_xrVoidFunction>(&fakeGetSystem);
    if (name == "xrGetVulkanGraphicsRequirements2KHR")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeGetGraphicsRequirements);
    if (name == "xrCreateVulkanInstanceKHR")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeCreateVulkanInstance);
    if (name == "xrGetVulkanGraphicsDevice2KHR")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeGetVulkanGraphicsDevice);
    if (name == "xrCreateVulkanDeviceKHR")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeCreateVulkanDevice);
    return nullptr;
}

XrResult XRAPI_CALL fakeGetInstanceProcAddr(
    XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
    CHECK(instance == fakeInstance());
    active_fake->calls.emplace_back(std::string{"resolve:"} + name);
    if (active_fake->failure == std::string{"resolve:"} + name) {
        *function = nullptr;
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    *function = fakeFunction(name);
    return *function == nullptr ? XR_ERROR_FUNCTION_UNSUPPORTED : XR_SUCCESS;
}

XrResult XRAPI_CALL fakeEnumerateExtensions(
    const char *, uint32_t capacity, uint32_t *count, XrExtensionProperties *properties) {
    active_fake->calls.emplace_back(capacity == 0 ? "enumerate_extensions_count"
                                                   : "enumerate_extensions_values");
    if (active_fake->failure == "enumerate_extensions") return XR_ERROR_RUNTIME_UNAVAILABLE;
    *count = 1U + (active_fake->advertise_local_floor ? 1U : 0U) +
             (active_fake->advertise_time_conversion ? 1U : 0U);
    if (capacity != 0) {
        const auto *name = active_fake->advertise_vulkan_enable2
                               ? XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME
                               : "XR_EXT_fixture_only";
        std::snprintf(properties[0].extensionName, sizeof(properties[0].extensionName), "%s", name);
        properties[0].extensionVersion = 1;
        if (active_fake->advertise_local_floor) {
            std::snprintf(properties[1].extensionName,
                          sizeof(properties[1].extensionName), "%s",
                          XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
            properties[1].extensionVersion = 1;
        }
        if (active_fake->advertise_time_conversion) {
            const auto index = active_fake->advertise_local_floor ? 2U : 1U;
            std::snprintf(properties[index].extensionName,
                          sizeof(properties[index].extensionName), "%s",
                          XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
            properties[index].extensionVersion = 1;
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeCreateInstance(const XrInstanceCreateInfo *info, XrInstance *instance) {
    active_fake->calls.emplace_back("create_instance");
    CHECK(XR_VERSION_MAJOR(info->applicationInfo.apiVersion) == 1);
    CHECK(XR_VERSION_MINOR(info->applicationInfo.apiVersion) == 0);
    CHECK(info->enabledExtensionCount ==
          1U + (active_fake->advertise_local_floor ? 1U : 0U) +
              (active_fake->advertise_time_conversion ? 1U : 0U));
    CHECK(std::string{info->enabledExtensionNames[0]} == XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    if (active_fake->advertise_local_floor) {
        CHECK(std::string{info->enabledExtensionNames[1]} ==
              XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
    }
    if (active_fake->advertise_time_conversion) {
        const auto index = active_fake->advertise_local_floor ? 2U : 1U;
        CHECK(std::string{info->enabledExtensionNames[index]} ==
              XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    }
    if (active_fake->failure == "create_instance") return XR_ERROR_RUNTIME_UNAVAILABLE;
    *instance = fakeInstance();
    return XR_SUCCESS;
}

Pelican::OpenXr::XrApi fakeApi() {
    return {
        .get_instance_proc_addr = &fakeGetInstanceProcAddr,
        .enumerate_instance_extension_properties = &fakeEnumerateExtensions,
        .create_instance = &fakeCreateInstance,
    };
}

} // namespace

TEST_CASE("OpenXR discovery resolves core and Vulkan commands through the injected table",
          "[openxr][discovery]") {
    FakeRuntime fake;
    FakeScope scope{fake};
    Pelican::OpenXr::DiscoveryRuntime runtime{fakeApi()};

    const auto result = runtime.discover();
    REQUIRE(result.availability == Pelican::XrDiscoveryAvailability::available);
    CHECK(result.detail.empty());
    CHECK(runtime.getInstance() == fakeInstance());
    CHECK(runtime.getSystemId() == 17);

    const std::vector<std::string> expected{
        "enumerate_extensions_count",
        "enumerate_extensions_values",
        "create_instance",
        "resolve:xrDestroyInstance",
        "resolve:xrGetSystem",
        "resolve:xrGetVulkanGraphicsRequirements2KHR",
        "resolve:xrCreateVulkanInstanceKHR",
        "resolve:xrGetVulkanGraphicsDevice2KHR",
        "resolve:xrCreateVulkanDeviceKHR",
        "get_system",
        "get_graphics_requirements",
    };
    CHECK(fake.calls == expected);
}

TEST_CASE("OpenXR discovery reports every pre-session failure stage",
          "[openxr][discovery]") {
    struct FailureCase {
        const char *failure;
        Pelican::XrDiscoveryAvailability expected;
    };
    constexpr std::array cases{
        FailureCase{"enumerate_extensions", Pelican::XrDiscoveryAvailability::runtime_unavailable},
        FailureCase{"create_instance", Pelican::XrDiscoveryAvailability::runtime_unavailable},
        FailureCase{"resolve:xrDestroyInstance", Pelican::XrDiscoveryAvailability::runtime_unavailable},
        FailureCase{"resolve:xrGetSystem", Pelican::XrDiscoveryAvailability::system_unavailable},
        FailureCase{"resolve:xrGetVulkanGraphicsRequirements2KHR",
                    Pelican::XrDiscoveryAvailability::graphics_binding_unavailable},
        FailureCase{"resolve:xrCreateVulkanInstanceKHR",
                    Pelican::XrDiscoveryAvailability::graphics_binding_unavailable},
        FailureCase{"resolve:xrGetVulkanGraphicsDevice2KHR",
                    Pelican::XrDiscoveryAvailability::graphics_binding_unavailable},
        FailureCase{"resolve:xrCreateVulkanDeviceKHR",
                    Pelican::XrDiscoveryAvailability::graphics_binding_unavailable},
        FailureCase{"get_system", Pelican::XrDiscoveryAvailability::system_unavailable},
        FailureCase{"get_graphics_requirements",
                    Pelican::XrDiscoveryAvailability::graphics_binding_unavailable},
    };

    for (const auto &test_case : cases) {
        DYNAMIC_SECTION(test_case.failure) {
            FakeRuntime fake{.failure = test_case.failure};
            FakeScope scope{fake};
            Pelican::OpenXr::DiscoveryRuntime runtime{fakeApi()};
            const auto result = runtime.discover();
            CHECK(result.availability == test_case.expected);
            CHECK_FALSE(result.detail.empty());
            CHECK(runtime.getInstance() == XR_NULL_HANDLE);
            CHECK(runtime.getSystemId() == XR_NULL_SYSTEM_ID);
        }
    }

    SECTION("required extension is absent") {
        FakeRuntime fake{.advertise_vulkan_enable2 = false};
        FakeScope scope{fake};
        Pelican::OpenXr::DiscoveryRuntime runtime{fakeApi()};
        const auto result = runtime.discover();
        CHECK(result.availability ==
              Pelican::XrDiscoveryAvailability::graphics_binding_unavailable);
        CHECK(result.detail.find("XR_KHR_vulkan_enable2") != std::string::npos);
        CHECK(runtime.getInstance() == XR_NULL_HANDLE);
    }
}

TEST_CASE("OpenXR runtime physical-device result overrides the engine heuristic token",
          "[openxr][discovery]") {
    // These opaque VkPhysicalDevice values are compared only.  Neither token
    // is submitted to Vulkan, which keeps the protocol fake on its side of the
    // Vulkan boundary.
    const auto heuristic_device =
        reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{0x201});
    const auto runtime_device =
        reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{0x202});
    FakeRuntime fake{.selected_physical_device = runtime_device};
    FakeScope scope{fake};
    Pelican::OpenXr::DiscoveryRuntime runtime{fakeApi()};
    REQUIRE(runtime.discover().availability == Pelican::XrDiscoveryAvailability::available);

    const auto selected = runtime.getVulkanGraphicsDevice(VK_NULL_HANDLE);
    CHECK(selected == runtime_device);
    CHECK(selected != heuristic_device);
    CHECK(fake.calls.back() == "get_vulkan_graphics_device");
}

TEST_CASE("OpenXR discovery enables the local-floor extension when a 1.0 runtime advertises it",
          "[openxr][discovery][reference-space]") {
    FakeRuntime fake{.advertise_local_floor = true};
    FakeScope scope{fake};
    Pelican::OpenXr::DiscoveryRuntime runtime{fakeApi()};
    CHECK(runtime.discover().availability ==
          Pelican::XrDiscoveryAvailability::available);
}

TEST_CASE("OpenXR discovery conditionally enables Win32 performance-counter conversion",
          "[openxr][discovery][timing]") {
    FakeRuntime absent;
    {
        FakeScope scope{absent};
        Pelican::OpenXr::DiscoveryRuntime runtime{fakeApi()};
        REQUIRE(runtime.discover().availability ==
                Pelican::XrDiscoveryAvailability::available);
        CHECK_FALSE(runtime.win32TimeConversionEnabled());
    }

    FakeRuntime advertised{.advertise_time_conversion = true};
    FakeScope scope{advertised};
    Pelican::OpenXr::DiscoveryRuntime runtime{fakeApi()};
    REQUIRE(runtime.discover().availability ==
            Pelican::XrDiscoveryAvailability::available);
    CHECK(runtime.win32TimeConversionEnabled());
}

TEST_CASE("Vulkan required extensions merge uniquely and report missing names",
          "[openxr][vulkan][bootstrap]") {
    constexpr std::array window_extensions{
        "VK_KHR_surface", "VK_KHR_win32_surface", "VK_KHR_surface"};
    constexpr std::array engine_extensions{"VK_KHR_surface", "VK_EXT_engine_fixture"};
    std::vector<std::string> merged;
    Pelican::appendUniqueVulkanExtensions(merged, window_extensions);
    Pelican::appendUniqueVulkanExtensions(merged, engine_extensions);
    CHECK(merged == std::vector<std::string>{
                        "VK_KHR_surface", "VK_KHR_win32_surface", "VK_EXT_engine_fixture"});

    const std::vector<std::string> supported{"VK_KHR_surface", "VK_KHR_win32_surface"};
    const auto missing = Pelican::firstMissingVulkanExtension(merged, supported);
    REQUIRE(missing);
    CHECK(*missing == "VK_EXT_engine_fixture");

    SECTION("auto falls back once before resources") {
        Pelican::EngineLaunchConfig config;
        config.xr_requested_mode = Pelican::XrMode::auto_mode;
        config.xr_mode = Pelican::XrMode::on;
        config.xr_active = true;
        const auto info = Pelican::resolveXrBootstrapFailure(
            config, "required Vulkan instance extension missing: " + *missing);
        CHECK_FALSE(config.xr_active);
        CHECK(config.xr_mode == Pelican::XrMode::off);
        CHECK(info.find(*missing) != std::string::npos);
        CHECK_THROWS_AS(Pelican::resolveXrBootstrapFailure(config, *missing), std::logic_error);
    }

    SECTION("explicit on is a hard error") {
        Pelican::EngineLaunchConfig config;
        config.xr_requested_mode = Pelican::XrMode::on;
        config.xr_mode = Pelican::XrMode::on;
        config.xr_active = true;
        CHECK_THROWS_WITH(Pelican::resolveXrBootstrapFailure(config, *missing),
                          Catch::Matchers::ContainsSubstring("--xr on") &&
                              Catch::Matchers::ContainsSubstring(*missing));
    }
}
