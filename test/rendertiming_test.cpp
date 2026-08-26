#include "../src/core/vkcore/rendertiming.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
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

TEST_CASE("GPU timing node averages use multiple frames and keep subranges independent",
          "[gpu-timing][average][wp351]") {
    const auto row = [](std::uint64_t frame, double barriers_ms,
                        double body_ms) {
        return GpuTimingNodeRow{
            frame, "flat", 0, 4, "render", "lit", barriers_ms,
            body_ms, true};
    };
    const std::vector<GpuTimingNodeFrame> frames{
        {10, "flat", {row(10, 4.0, 2.0)}},
        {11, "flat", {row(11, 4.0, 8.0)}},
    };

    const auto average = averageGpuTimingNodeFrames(frames);
    REQUIRE(average.frame_count == 2);
    REQUIRE(average.nodes.size() == 1);
    const auto &node = average.nodes.front();
    REQUIRE(node.sample_count == 2);
    REQUIRE(node.body_ms == Catch::Approx(5.0));
    REQUIRE(node.body_ms != Catch::Approx(2.0));
    REQUIRE(node.body_ms != Catch::Approx(8.0));
    REQUIRE(node.barriers_ms == Catch::Approx(4.0));
}

TEST_CASE("GPU timing node averages discard frames older than the fixed window",
          "[gpu-timing][average][window][wp351]") {
    std::vector<GpuTimingNodeFrame> frames;
    frames.reserve(gpu_timing_node_average_window + 1);
    for (std::size_t index = 0;
         index < gpu_timing_node_average_window + 1; ++index) {
        const bool discarded = index == 0;
        frames.push_back(GpuTimingNodeFrame{
            index + 1,
            "flat",
            {GpuTimingNodeRow{
                index + 1, "flat", 0, 1, "compute", "cull",
                discarded ? 6000.0 : 6.0,
                discarded ? 3000.0 : 3.0, true}},
        });
    }

    const auto average = averageGpuTimingNodeFrames(frames);
    REQUIRE(average.frame_count == gpu_timing_node_average_window);
    REQUIRE(average.nodes.front().sample_count ==
            gpu_timing_node_average_window);
    REQUIRE(average.nodes.front().barriers_ms == Catch::Approx(6.0));
    REQUIRE(average.nodes.front().body_ms == Catch::Approx(3.0));
}

TEST_CASE("GPU timing average RPC projection is light and reports disabled state",
          "[gpu-timing][rpc][wp351]") {
    GpuTimingNodeAverageSnapshot snapshot;
    snapshot.frame_count = 3;
    snapshot.nodes.push_back(GpuTimingNodeAverageRow{
        9, "flat", 0, 2, "render", "present", 0.25, 1.5,
        true, 3});
    const auto enabled = gpuTimingNodeAverageStatusJson(
        snapshot, true, true, "enabled");
    REQUIRE(enabled.at("schema") ==
            "pelican.gpu_timing_node_averages");
    REQUIRE(enabled.at("version") == 1);
    REQUIRE(enabled.at("window_size") ==
            gpu_timing_node_average_window);
    REQUIRE(enabled.at("frame_count") == 3);
    REQUIRE(enabled.at("nodes").at(0).at("sample_count") == 3);
    REQUIRE_FALSE(enabled.contains("logical_frame_history"));

    const auto disabled = disabledGpuTimingNodeAverageStatusJson();
    REQUIRE_FALSE(disabled.at("enabled").get<bool>());
    REQUIRE_FALSE(disabled.at("supported").get<bool>());
    REQUIRE(disabled.at("reason") == "feature_not_enabled");
    REQUIRE(disabled.at("frame_count") == 0);
    REQUIRE(disabled.at("nodes").empty());
    REQUIRE_FALSE(disabled.contains("logical_frame_history"));
}

} // namespace Pelican
