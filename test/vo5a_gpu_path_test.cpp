// VO-5a: the GPU path decision over a fake capability source, on every platform.
#include "../src/core/output/outputspoutgpupath.hpp"
#include "../src/core/vkcore/extensionrequests.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <vulkan/vulkan_core.h>

#include <string>
#include <vector>

namespace {
using Pelican::OutputSpoutGpuPathContext;
using Pelican::OutputSpoutGpuPathReport;
using Pelican::OutputSpoutHandleKind;
using Pelican::OutputSpoutImportQuery;
using Pelican::evaluateOutputSpoutGpuPath;
using Pelican::outputSpoutGpuPathJson;

// Starts all good. A test breaks what it's about, and it counts its calls.
class FakeSource final : public Pelican::OutputSpoutGpuCapabilitySource {
  public:
    std::vector<std::string> enabled{"VK_KHR_external_memory_win32",
                                     "VK_KHR_external_semaphore_win32",
                                     "VK_KHR_win32_keyed_mutex"};
    OutputSpoutImportQuery nt{"VK_SUCCESS", false, true, true, 16384, 16384};
    OutputSpoutImportQuery kmt{"VK_SUCCESS", false, true, true, 16384, 16384};
    Pelican::OutputSpoutFormatQuery format{true, true};
    bool timeline = true;
    bool semaphore = true;
    Pelican::OutputSpoutLuid luid{true, {1, 2, 3, 4, 5, 6, 7, 8}};
    Pelican::OutputSpoutAdapterQuery adapter{true, {1, 2, 3, 4, 5, 6, 7, 8}, "Fake GPU", ""};
    mutable int calls = 0;

    std::vector<std::string> enabledDeviceExtensions() const override {
        ++calls;
        return enabled;
    }
    OutputSpoutImportQuery queryImport(OutputSpoutHandleKind kind) const override {
        ++calls;
        return kind == OutputSpoutHandleKind::nt ? nt : kmt;
    }
    Pelican::OutputSpoutFormatQuery queryFormat() const override {
        ++calls;
        return format;
    }
    bool timelineSemaphoreEnabled() const override {
        ++calls;
        return timeline;
    }
    bool semaphoreImportable() const override {
        ++calls;
        return semaphore;
    }
    Pelican::OutputSpoutLuid deviceLuid() const override {
        ++calls;
        return luid;
    }
    Pelican::OutputSpoutAdapterQuery defaultAdapter() const override {
        ++calls;
        return adapter;
    }
};

constexpr OutputSpoutGpuPathContext armed{true, true, true};

OutputSpoutImportQuery notSupported() {
    return {"VK_ERROR_FORMAT_NOT_SUPPORTED", false, false, false, 0, 0};
}

OutputSpoutGpuPathReport evaluate(const FakeSource &source) {
    return evaluateOutputSpoutGpuPath(armed, &source);
}

void checkUnavailable(const OutputSpoutGpuPathReport &report, const std::string &reason) {
    INFO(report.detail);
    CHECK(report.state == "unavailable");
    CHECK(report.reason == reason);
    CHECK_FALSE(report.detail.empty());
}
} // namespace

TEST_CASE("VO-5a all good is available", "[vo5a]") {
    const FakeSource source;
    const auto report = evaluate(source);
    CHECK(report.state == "available");
    CHECK(report.reason == "none");
    CHECK(report.detail.empty());
    REQUIRE(report.facts.has_value());
}

TEST_CASE("VO-5a reasons that stop before the source never call it", "[vo5a]") {
    const FakeSource source;
    checkUnavailable(evaluateOutputSpoutGpuPath({false, true, true}, &source),
                     "platform_unsupported");
    CHECK(source.calls == 0);
    checkUnavailable(evaluateOutputSpoutGpuPath({true, false, true}, &source),
                     "build_without_spout");
    CHECK(source.calls == 0);
    checkUnavailable(evaluateOutputSpoutGpuPath({true, true, false}, &source), "not_requested");
    CHECK(source.calls == 0);
    // Not Windows comes first, whatever else is true.
    checkUnavailable(evaluateOutputSpoutGpuPath({false, false, false}, &source),
                     "platform_unsupported");
    CHECK(source.calls == 0);
}

TEST_CASE("VO-5a external_memory_not_enabled", "[vo5a]") {
    FakeSource source;
    source.enabled = {"VK_KHR_external_semaphore_win32", "VK_KHR_win32_keyed_mutex"};
    checkUnavailable(evaluate(source), "external_memory_not_enabled");
}

TEST_CASE("VO-5a device_luid_invalid", "[vo5a]") {
    FakeSource source;
    source.luid.valid = false;
    checkUnavailable(evaluate(source), "device_luid_invalid");
}

TEST_CASE("VO-5a dxgi_unavailable", "[vo5a]") {
    FakeSource source;
    source.adapter = {false, {}, "", "CreateDXGIFactory1 failed"};
    const auto report = evaluate(source);
    checkUnavailable(report, "dxgi_unavailable");
    CHECK(report.detail == "CreateDXGIFactory1 failed");
}

TEST_CASE("VO-5a luid_mismatch for a difference in any byte, not in the name", "[vo5a]") {
    SECTION("byte 0") {
        FakeSource source;
        source.adapter.luid[0] = 99;
        checkUnavailable(evaluate(source), "luid_mismatch");
    }
    SECTION("byte 7") {
        FakeSource source;
        source.adapter.luid[7] = 99;
        checkUnavailable(evaluate(source), "luid_mismatch");
    }
}

TEST_CASE("VO-5a format_not_importable, even with all three extensions enabled", "[vo5a]") {
    FakeSource source;
    source.nt = notSupported();
    source.kmt = notSupported();
    const auto report = evaluate(source);
    checkUnavailable(report, "format_not_importable");
    REQUIRE(report.facts.has_value());
    CHECK(report.facts->external_memory_enabled);
    CHECK(report.facts->external_semaphore_enabled);
    CHECK(report.facts->keyed_mutex_enabled);
}

TEST_CASE("VO-5a capability_query_failed carries the VkResult", "[vo5a]") {
    FakeSource source;
    source.nt = {"VK_ERROR_OUT_OF_HOST_MEMORY", true, false, false, 0, 0};
    source.kmt = notSupported();
    const auto report = evaluate(source);
    checkUnavailable(report, "capability_query_failed");
    CHECK_THAT(report.detail, Catch::Matchers::ContainsSubstring("VK_ERROR_OUT_OF_HOST_MEMORY"));
}

TEST_CASE("VO-5a a good handle isn't lost to the other handle's result", "[vo5a]") {
    SECTION("NT only") {
        FakeSource source;
        source.kmt = notSupported();
        CHECK(evaluate(source).state == "available");
    }
    SECTION("KMT only") {
        FakeSource source;
        source.nt = notSupported();
        CHECK(evaluate(source).state == "available");
    }
    SECTION("KMT good, NT's query failed") {
        FakeSource source;
        source.nt = {"VK_ERROR_OUT_OF_DEVICE_MEMORY", true, false, false, 0, 0};
        CHECK(evaluate(source).state == "available");
    }
}

TEST_CASE("VO-5a format_no_transfer_dst, and blit_dst is only reported", "[vo5a]") {
    SECTION("no transfer_dst") {
        FakeSource source;
        source.format = {false, true};
        checkUnavailable(evaluate(source), "format_no_transfer_dst");
    }
    SECTION("no blit_dst stays available and says so") {
        FakeSource source;
        source.format = {true, false};
        const auto report = evaluate(source);
        CHECK(report.state == "available");
        CHECK(outputSpoutGpuPathJson(report).at("format").at("blit_dst") == false);
    }
}

TEST_CASE("VO-5a dedicated_only alone stays available and is reported per handle", "[vo5a]") {
    FakeSource source;
    source.nt.dedicated_only = true;
    source.kmt.dedicated_only = false;
    const auto report = evaluate(source);
    CHECK(report.state == "available");
    const auto json = outputSpoutGpuPathJson(report);
    CHECK(json.at("handles").at("nt").at("dedicated_only") == true);
    CHECK(json.at("handles").at("kmt").at("dedicated_only") == false);
}

TEST_CASE("VO-5a no_usable_sync, and either way of syncing is enough", "[vo5a]") {
    SECTION("keyed mutex only") {
        FakeSource source;
        source.semaphore = false;
        const auto report = evaluate(source);
        CHECK(report.state == "available");
        CHECK(report.facts->sync_keyed_mutex);
        CHECK_FALSE(report.facts->sync_external_semaphore);
    }
    SECTION("semaphore only") {
        FakeSource source;
        source.enabled = {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"};
        const auto report = evaluate(source);
        CHECK(report.state == "available");
        CHECK_FALSE(report.facts->sync_keyed_mutex);
        CHECK(report.facts->sync_external_semaphore);
    }
    SECTION("an importable semaphore with its extension off is no sync") {
        FakeSource source;
        source.enabled = {"VK_KHR_external_memory_win32"};
        checkUnavailable(evaluate(source), "no_usable_sync");
    }
    SECTION("an importable semaphore with the timeline feature off is no sync") {
        FakeSource source;
        source.enabled = {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"};
        source.timeline = false;
        checkUnavailable(evaluate(source), "no_usable_sync");
    }
    SECTION("neither") {
        FakeSource source;
        source.enabled = {"VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"};
        source.semaphore = false;
        checkUnavailable(evaluate(source), "no_usable_sync");
    }
}

TEST_CASE("VO-5a two breaks give the earlier reason", "[vo5a]") {
    FakeSource source;
    source.nt = notSupported();
    source.kmt = notSupported();
    source.format = {false, false};
    source.adapter.luid[3] = 99;
    checkUnavailable(evaluate(source), "luid_mismatch");
    source.adapter.luid[3] = 4;
    checkUnavailable(evaluate(source), "format_not_importable");
    source.nt.importable = true;
    checkUnavailable(evaluate(source), "format_no_transfer_dst");
}

TEST_CASE("VO-5a an unavailable report still has every fact", "[vo5a]") {
    FakeSource source;
    source.enabled = {};
    source.semaphore = false;
    const auto report = evaluate(source);
    checkUnavailable(report, "external_memory_not_enabled");
    const auto json = outputSpoutGpuPathJson(report);
    INFO(json.dump());
    CHECK(json.at("extensions").at("VK_KHR_external_memory_win32") == false);
    CHECK(json.at("handles").at("nt").at("result") == "VK_SUCCESS");
    CHECK(json.at("handles").at("kmt").at("max_width") == 16384);
    CHECK(json.at("format").at("dxgi") == "DXGI_FORMAT_B8G8R8A8_UNORM");
    CHECK(json.at("format").at("vulkan") == "VK_FORMAT_B8G8R8A8_UNORM");
    CHECK(json.at("format").at("tiling") == "optimal");
    CHECK(json.at("format").at("transfer_dst") == true);
    CHECK(json.at("sync").at("keyed_mutex") == false);
    CHECK(json.at("sync").at("external_semaphore") == false);
    CHECK(json.at("luid").at("vulkan_valid") == true);
    CHECK(json.at("luid").at("vulkan") == "0102030405060708");
    CHECK(json.at("luid").at("dxgi") == "0102030405060708");
    CHECK(json.at("luid").at("dxgi_adapter") == "Fake GPU");
}

TEST_CASE("VO-5a a default report is not_checked, with null facts", "[vo5a]") {
    const auto json = outputSpoutGpuPathJson(OutputSpoutGpuPathReport{});
    INFO(json.dump());
    CHECK(json.at("state") == "unavailable");
    CHECK(json.at("reason") == "not_checked");
    for (const auto *field : {"extensions", "format", "handles", "sync", "luid"}) {
        CAPTURE(field);
        CHECK(json.at(field).is_null());
    }
    // An unasked source doesn't become a false fact.
    const auto not_requested =
        outputSpoutGpuPathJson(evaluateOutputSpoutGpuPath({true, true, false}, nullptr));
    CHECK(not_requested.at("reason") == "not_requested");
    CHECK(not_requested.at("handles").is_null());
}

TEST_CASE("VO-5a the handle bits and the import features read the right Vulkan constants",
          "[vo5a]") {
    CHECK(Pelican::outputSpoutHandleTypeBit(OutputSpoutHandleKind::nt) ==
          static_cast<std::uint32_t>(VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT));
    CHECK(Pelican::outputSpoutHandleTypeBit(OutputSpoutHandleKind::kmt) ==
          static_cast<std::uint32_t>(VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT));
    const auto exportable =
        Pelican::outputSpoutImportFeatures(VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
    CHECK_FALSE(exportable.importable);
    CHECK_FALSE(exportable.dedicated_only);
    const auto both = Pelican::outputSpoutImportFeatures(
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
    CHECK(both.importable);
    CHECK(both.dedicated_only);
}
