// VO-5a: the extension request table and how it resolves. Pure, no GPU, every
// platform and every build.
#include "../src/core/launchconfig.hpp"
#include "../src/core/vkcore/extensionrequests.hpp"
#include "../src/core/xractivation.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Pelican::VulkanExtensionLevel;
using Pelican::VulkanExtensionNeed;
using Pelican::VulkanExtensionRequests;
using Pelican::VulkanExtensionRow;
using Pelican::resolveVulkanExtensions;
using Pelican::withOpenXrInstanceRequests;
using Names = std::vector<std::string>;
using Outcome = VulkanExtensionRow::Outcome;
constexpr auto device_level = VulkanExtensionLevel::device;
constexpr auto instance_level = VulkanExtensionLevel::instance;
constexpr auto optional = VulkanExtensionNeed::optional;
constexpr auto required = VulkanExtensionNeed::required;

const Names supported{"VK_a", "VK_b", "VK_c"};
} // namespace

TEST_CASE("VO-5a an empty table leaves the base exactly as it was", "[vo5a]") {
    for (const auto &base : {Names{}, Names{"VK_only"},
                             Names{"VK_z", "VK_a", "VK_m", "VK_b", "VK_y"}}) {
        const auto result = resolveVulkanExtensions(device_level, base, {}, supported);
        CHECK(result.enabled == base);
        CHECK(result.rows.empty());
        CHECK_FALSE(result.refusal.has_value());
    }
}

TEST_CASE("VO-5a a supported optional row goes after the base, an unsupported one is skipped",
          "[vo5a]") {
    VulkanExtensionRequests requests;
    requests.requestDevice("VK_b", optional, "feature");
    requests.requestDevice("VK_absent", optional, "feature");
    requests.requestDevice("VK_a", optional, "feature");
    const auto result =
        resolveVulkanExtensions(device_level, {"VK_base"}, requests.device(), supported);
    CHECK(result.enabled == Names{"VK_base", "VK_b", "VK_a"});
    CHECK_FALSE(result.refusal.has_value());
    REQUIRE(result.rows.size() == 3);
    CHECK(result.rows[0].outcome == Outcome::enabled);
    CHECK(result.rows[1].name == "VK_absent");
    CHECK(result.rows[1].outcome == Outcome::skipped_unsupported);
    CHECK(result.rows[2].outcome == Outcome::enabled);
}

TEST_CASE("VO-5a a missing required row refuses by name, level and every requester", "[vo5a]") {
    VulkanExtensionRequests requests;
    requests.requestDevice("VK_absent", optional, "first");
    requests.requestDevice("VK_absent", required, "second");
    requests.requestInstance("VK_gone", required, "third");
    const auto device =
        resolveVulkanExtensions(device_level, {}, requests.device(), supported);
    REQUIRE(device.refusal.has_value());
    CHECK_THAT(*device.refusal, Catch::Matchers::ContainsSubstring(
                                    "vulkan.required_extension_missing@1: device extension "
                                    "VK_absent, requested by first, second"));
    CHECK(device.enabled.empty());
    REQUIRE(device.rows.size() == 1);
    CHECK(device.rows[0].outcome == Outcome::refused_unsupported);

    const auto instance =
        resolveVulkanExtensions(instance_level, {}, requests.instance(), supported);
    REQUIRE(instance.refusal.has_value());
    CHECK_THAT(*instance.refusal,
               Catch::Matchers::ContainsSubstring("instance extension VK_gone, requested by third"));
}

TEST_CASE("VO-5a a row already in the base gives no duplicate", "[vo5a]") {
    VulkanExtensionRequests requests;
    requests.requestDevice("VK_base", optional, "feature");
    requests.requestDevice("VK_base2", required, "feature");
    const auto result = resolveVulkanExtensions(device_level, {"VK_base", "VK_base2"},
                                                requests.device(), Names{});
    CHECK(result.enabled == Names{"VK_base", "VK_base2"});
    CHECK_FALSE(result.refusal.has_value());
    REQUIRE(result.rows.size() == 2);
    CHECK(result.rows[0].outcome == Outcome::enabled);
    CHECK(result.rows[1].outcome == Outcome::enabled);
}

TEST_CASE("VO-5a instance and device rows never mix", "[vo5a]") {
    VulkanExtensionRequests requests;
    requests.requestInstance("VK_a", required, "instance_side");
    requests.requestDevice("VK_b", optional, "device_side");
    REQUIRE(requests.instance().size() == 1);
    REQUIRE(requests.device().size() == 1);
    CHECK(resolveVulkanExtensions(device_level, {}, requests.device(), supported).enabled ==
          Names{"VK_b"});
    CHECK(resolveVulkanExtensions(instance_level, {}, requests.instance(), supported).enabled ==
          Names{"VK_a"});
}

TEST_CASE("VO-5a two requests for one name merge: first position, required wins, each requester once",
          "[vo5a]") {
    const auto check = [](const VulkanExtensionRequests &requests) {
        REQUIRE(requests.device().size() == 2);
        CHECK(requests.device()[0].name == "VK_a");
        CHECK(requests.device()[0].need == required);
        CHECK(requests.device()[0].requested_by == Names{"one", "two"});
        CHECK(requests.device()[1].name == "VK_b");
    };
    SECTION("optional then required") {
        VulkanExtensionRequests requests;
        requests.requestDevice("VK_a", optional, "one");
        requests.requestDevice("VK_b", optional, "two");
        requests.requestDevice("VK_a", required, "two");
        requests.requestDevice("VK_a", optional, "one");
        check(requests);
    }
    SECTION("required then optional") {
        VulkanExtensionRequests requests;
        requests.requestDevice("VK_a", required, "one");
        requests.requestDevice("VK_b", optional, "two");
        requests.requestDevice("VK_a", optional, "two");
        check(requests);
    }
}

TEST_CASE("VO-5a an empty name is a programming error", "[vo5a]") {
    VulkanExtensionRequests requests;
    CHECK_THROWS_AS(requests.requestDevice("", optional, "feature"), std::logic_error);
    CHECK_THROWS_AS(requests.requestInstance("", required, "feature"), std::logic_error);
}

TEST_CASE("VO-5a XR's rows come first in their own order, in a copy", "[vo5a]") {
    VulkanExtensionRequests table;
    table.requestInstance("VK_feature", optional, "feature");
    table.requestInstance("VK_shared", optional, "feature");
    table.requestDevice("VK_device_row", optional, "feature");
    const Names xr_list{"VK_z", "VK_shared", "VK_a"};
    const auto merged = withOpenXrInstanceRequests(table, xr_list);

    REQUIRE(merged.instance().size() == 4);
    CHECK(merged.instance()[0].name == "VK_z");
    CHECK(merged.instance()[1].name == "VK_shared");
    CHECK(merged.instance()[1].need == required);
    CHECK(merged.instance()[1].requested_by == Names{"openxr", "feature"});
    CHECK(merged.instance()[2].name == "VK_a");
    CHECK(merged.instance()[3].name == "VK_feature");
    CHECK(merged.instance()[3].need == optional);
    REQUIRE(merged.device().size() == 1);
    CHECK(merged.device()[0].name == "VK_device_row");

    // The table the flat retry gets has no "openxr" row.
    REQUIRE(table.instance().size() == 2);
    for (const auto &row : table.instance()) {
        CHECK(std::find(row.requested_by.begin(), row.requested_by.end(), "openxr") ==
              row.requested_by.end());
    }
    CHECK(table.instance()[1].need == optional);

    SECTION("a missing optional row doesn't refuse") {
        const Names all{"VK_z", "VK_shared", "VK_a"};
        const auto result =
            resolveVulkanExtensions(instance_level, {}, merged.instance(), all);
        CHECK_FALSE(result.refusal.has_value());
        CHECK(result.enabled == xr_list);
    }
    SECTION("a missing openxr row refuses by name and takes XR's fallback rules") {
        const Names without_shared{"VK_z", "VK_a", "VK_feature"};
        const auto result =
            resolveVulkanExtensions(instance_level, {}, merged.instance(), without_shared);
        REQUIRE(result.refusal.has_value());
        CHECK_THAT(*result.refusal, Catch::Matchers::ContainsSubstring(
                                        "VK_shared, requested by openxr, feature"));

        Pelican::EngineLaunchConfig automatic;
        automatic.xr_requested_mode = Pelican::XrMode::auto_mode;
        automatic.xr_mode = Pelican::XrMode::on;
        automatic.xr_active = true;
        const auto info = Pelican::resolveXrBootstrapFailure(automatic, *result.refusal);
        CHECK_FALSE(automatic.xr_active);
        CHECK_THAT(info, Catch::Matchers::ContainsSubstring("VK_shared"));

        Pelican::EngineLaunchConfig explicit_on;
        explicit_on.xr_requested_mode = Pelican::XrMode::on;
        explicit_on.xr_mode = Pelican::XrMode::on;
        explicit_on.xr_active = true;
        CHECK_THROWS_WITH(Pelican::resolveXrBootstrapFailure(explicit_on, *result.refusal),
                          Catch::Matchers::ContainsSubstring("--xr on") &&
                              Catch::Matchers::ContainsSubstring("VK_shared"));
    }
}

TEST_CASE("VO-5a registration: nothing without the flag, three optional device rows with it",
          "[vo5a]") {
    const auto off = Pelican::collectVulkanExtensionRequests(false);
    CHECK(off.instance().empty());
    CHECK(off.device().empty());

    const auto on = Pelican::collectVulkanExtensionRequests(true);
    CHECK(on.instance().empty());
#if PELICAN_WITH_SPOUT
    REQUIRE(on.device().size() == 3);
    CHECK(on.device()[0].name == "VK_KHR_external_memory_win32");
    CHECK(on.device()[1].name == "VK_KHR_external_semaphore_win32");
    CHECK(on.device()[2].name == "VK_KHR_win32_keyed_mutex");
    for (const auto &row : on.device()) {
        CHECK(row.need == optional);
        CHECK(row.requested_by == Names{"output.spout"});
    }
#else
    CHECK(on.device().empty());
#endif
}

TEST_CASE("VO-5a the report's JSON says what it is and has every row", "[vo5a]") {
    Pelican::VulkanExtensionReport report;
    report.device_created_by = "openxr";
    report.instance.enabled = {"VK_i"};
    report.device.enabled = {"VK_a"};
    report.device.rows = {
        {"VK_a", required, {"one", "two"}, Outcome::enabled},
        {"VK_b", optional, {"three"}, Outcome::skipped_unsupported},
        {"VK_c", required, {"four"}, Outcome::refused_unsupported},
    };
    const auto json = Pelican::vulkanExtensionReportJson(report);
    INFO(json.dump());
    CHECK(json.at("list_scope") == "engine_requested");
    CHECK(json.at("device_created_by") == "openxr");
    CHECK(json.at("instance").at("enabled") == nlohmann::json::array({"VK_i"}));
    CHECK(json.at("instance").at("rows").empty());
    const auto &rows = json.at("device").at("rows");
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].at("name") == "VK_a");
    CHECK(rows[0].at("need") == "required");
    CHECK(rows[0].at("requested_by") == nlohmann::json::array({"one", "two"}));
    CHECK(rows[0].at("outcome") == "enabled");
    CHECK(rows[1].at("need") == "optional");
    CHECK(rows[1].at("outcome") == "skipped_unsupported");
    CHECK(rows[2].at("outcome") == "refused_unsupported");
}
