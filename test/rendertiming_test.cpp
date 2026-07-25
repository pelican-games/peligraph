#include "../src/core/vkcore/rendertiming.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {
namespace {

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures";
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream stream{path};
    if (!stream) throw std::runtime_error("failed to open fixture: " + path.string());
    return nlohmann::json::parse(stream);
}

} // namespace

TEST_CASE("GPU timing identity is the command-label identity plus ordered subrange",
          "[gpu-timing][identity]") {
    const std::vector<std::pair<GpuTimingSubrange, std::string>> subranges{
        {GpuTimingSubrange::barriers, "barriers"},
        {GpuTimingSubrange::body, "body"},
    };
    for (const auto &[subrange, name] : subranges) {
        const GpuTimingSampleIdentity identity{
            42, "xr", 1, 7, "compute", "cull#xr", subrange};
        REQUIRE(makeGpuTimingSampleLabel(identity) ==
                "frame/42/graph/xr/view/1/node/7:compute:cull#xr/" + name);
    }
}

TEST_CASE("GPU timing attribution table is an exact fixture",
          "[gpu-timing][attribution]") {
    const auto expected = readJson(fixtureRoot() / "gpu_timing_attribution.json");
    REQUIRE(gpuTimingAttributionContractJson() == expected);

    const auto &rows = expected.at("rows");
    REQUIRE(rows.size() == 7);
    REQUIRE(rows.at(0).at("node_kind") == "render");
    REQUIRE(rows.at(1).at("node_kind") == "compute");
    REQUIRE(rows.at(2).at("node_kind") == "anchor");
    REQUIRE(rows.at(4).at("node_kind") == "output_transform");
    REQUIRE(rows.at(5).at("operation") == "xr_mirror_intermediate");
    REQUIRE(rows.at(6).at("operation") == "desktop_mirror");
}

TEST_CASE("disabled GPU timing status keeps the additive v2 schema",
          "[gpu-timing][status]") {
    const auto status = disabledGpuTimingStatusJson();
    REQUIRE(status.at("schema_version") == 2);
    REQUIRE_FALSE(status.at("enabled").get<bool>());
    REQUIRE_FALSE(status.at("supported").get<bool>());
    REQUIRE(status.at("reason") == "feature_not_enabled");
    REQUIRE(status.at("history_capacity") == 120);
    REQUIRE(status.at("history_count") == 0);
    REQUIRE(status.at("dropped_samples") == 0);
    REQUIRE(status.at(
                "logical_frame_averages")
                .empty());
    REQUIRE(status.at(
                "logical_frame_history")
                .empty());
    REQUIRE(status.at("views").empty());
    REQUIRE(status.at("nodes").empty());
}

} // namespace Pelican
