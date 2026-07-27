#include "../src/project/gpudrawtiming.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Pelican {
namespace {

nlohmann::json loadFixture() {
    const auto path =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "test" / "fixtures" /
        "gpu_draw_timing_observation.json";
    std::ifstream stream{path};
    if (!stream) {
        throw std::runtime_error(
            "failed to open GPU draw timing fixture: " +
            path.string());
    }
    return nlohmann::json::parse(stream);
}

} // namespace

TEST_CASE(
    "GPU draw timing observation round-trips a strict typed contract",
    "[gpu-draw][timing][profile]") {
    const auto fixture = loadFixture();
    const auto observation =
        compileGpuDrawTimingObservation(fixture);

    REQUIRE(observation.workload.candidate_records == 1024);
    REQUIRE(observation.workload.visible_records == 192);
    REQUIRE(observation.workload.segment_records == 8);
    REQUIRE(observation.gpu_path.sample_count == 60);
    REQUIRE(
        nlohmann::json::parse(
            gpuDrawTimingObservationToJson(
                observation)
                .dump()) ==
        fixture);

    auto unknown = fixture;
    unknown["workload"]["entity_count"] = 1024;
    REQUIRE_THROWS_WITH(
        compileGpuDrawTimingObservation(unknown),
        "GPU draw timing workload has unknown key 'entity_count'");

    auto overlapping = fixture;
    overlapping["workload"]["visible_records"] = 1200;
    REQUIRE(
        compileGpuDrawTimingObservation(
            overlapping)
            .workload.visible_records ==
        1200);

    auto invalid = fixture;
    invalid["workload"]["visible_records"] = 2049;
    REQUIRE_THROWS_WITH(
        compileGpuDrawTimingObservation(invalid),
        "GPU draw timing visible_records exceeds output capacity");
}

TEST_CASE(
    "GPU draw break-even decision requires stable measured gain",
    "[gpu-draw][timing][break-even]") {
    const auto observation =
        compileGpuDrawTimingObservation(loadFixture());
    const GpuDrawBreakEvenPolicy policy{
        .minimum_gain_percent = 5.0,
        .minimum_sample_count = 30,
    };

    const auto gpu =
        evaluateGpuDrawBreakEven(observation, policy);
    REQUIRE(
        gpu.selection ==
        GpuDrawTimingSelection::gpu_culling);
    REQUIRE(gpu.metric == "frame_gpu_ms");
    REQUIRE(gpu.measured_gain_percent > 5.0);

    auto slower = observation;
    slower.gpu_path.frame_gpu_ms = 1.4;
    const auto cpu =
        evaluateGpuDrawBreakEven(slower, policy);
    REQUIRE(
        cpu.selection ==
        GpuDrawTimingSelection::cpu_draw_queue);

    auto short_run = observation;
    short_run.gpu_path.sample_count = 8;
    const auto inconclusive =
        evaluateGpuDrawBreakEven(short_run, policy);
    REQUIRE(
        inconclusive.selection ==
        GpuDrawTimingSelection::inconclusive);
    REQUIRE(
        inconclusive.reason.find(
            "insufficient samples") !=
        std::string::npos);

    REQUIRE(
        gpuDrawBreakEvenPolicyToJson(policy).at("metric") ==
        "frame_gpu_ms");
    REQUIRE(
        gpuDrawBreakEvenDecisionToJson(gpu).at("selection") ==
        "gpu_culling");
}

} // namespace Pelican
