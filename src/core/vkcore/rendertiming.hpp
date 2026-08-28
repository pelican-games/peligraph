#pragma once

#include "../container.hpp"
#include <chrono>
#include <cstdint>
#include <deque>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

inline constexpr std::size_t gpu_timing_history_capacity = 120;
inline constexpr std::size_t gpu_timing_node_average_window = 30;

struct CpuFrameDurations {
    double update_ms = 0.0;
    double render_ms = 0.0;
    double present_wait_ms = 0.0;
};

enum class GpuTimingSubrange : std::uint8_t {
    barriers,
    body,
};

std::string_view gpuTimingSubrangeName(GpuTimingSubrange subrange) noexcept;

struct GpuTimingRangeIdentity {
    std::uint64_t logical_frame = 0;
    std::string graph_variant;
    std::uint32_t view_index = 0;
};

struct GpuTimingNodeDescriptor {
    std::size_t node_ordinal = 0;
    std::string node_kind;
    std::string node_name;
    bool body_supported = true;
};

struct GpuTimingSampleIdentity {
    std::uint64_t logical_frame = 0;
    std::string graph_variant;
    std::uint32_t view_index = 0;
    std::size_t node_ordinal = 0;
    std::string node_kind;
    std::string node_name;
    GpuTimingSubrange subrange = GpuTimingSubrange::barriers;
};

struct GpuTimingSample {
    GpuTimingSampleIdentity identity;
    bool supported = true;
    std::string reason;
    double ms = 0.0;
};

struct GpuTimingHistoryFrame {
    std::uint64_t logical_frame = 0;
    std::string graph_variant;
    std::vector<GpuTimingSample> samples;
};

struct GpuTimingViewRow {
    std::uint64_t logical_frame = 0;
    std::string graph_variant;
    std::uint32_t view_index = 0;
    std::string label;
    double barriers_ms = 0.0;
    double body_ms = 0.0;
    double total_ms = 0.0;
};

struct GpuTimingNodeRow {
    std::uint64_t logical_frame = 0;
    std::string graph_variant;
    std::uint32_t view_index = 0;
    std::size_t node_ordinal = 0;
    std::string node_kind;
    std::string node_name;
    double barriers_ms = 0.0;
    double body_ms = 0.0;
    bool body_supported = true;
};

struct GpuTimingNodeFrame {
    std::uint64_t logical_frame = 0;
    std::string graph_variant;
    std::vector<GpuTimingNodeRow> nodes;
};

struct GpuTimingPublishedSnapshot {
    std::vector<GpuTimingViewRow> latest_views;
    std::vector<GpuTimingNodeRow> latest_nodes;
    std::size_t history_frame_visits = 0;
};

struct GpuTimingStatusProjection {
    nlohmann::json logical_frame_averages;
    nlohmann::json logical_frame_history;
    nlohmann::json views;
    nlohmann::json nodes;
    double logical_frame_total_sum_views_ms = 0.0;
};

GpuTimingPublishedSnapshot publishLatestGpuTimingSnapshot(
    const std::deque<GpuTimingHistoryFrame> &history);
GpuTimingStatusProjection projectGpuTimingStatus(
    const std::deque<GpuTimingHistoryFrame> &history,
    std::span<const GpuTimingViewRow> latest_views);

struct GpuTimingNodeAverageRow {
    std::uint64_t logical_frame = 0;
    std::string graph_variant;
    std::uint32_t view_index = 0;
    std::size_t node_ordinal = 0;
    std::string node_kind;
    std::string node_name;
    double barriers_ms = 0.0;
    double body_ms = 0.0;
    bool body_supported = true;
    std::size_t sample_count = 0;
};

struct GpuTimingNodeAverageSnapshot {
    std::size_t frame_count = 0;
    std::vector<GpuTimingNodeAverageRow> nodes;
};

GpuTimingNodeAverageSnapshot averageGpuTimingNodeFrames(
    std::span<const GpuTimingNodeFrame> frames,
    std::size_t window = gpu_timing_node_average_window);
nlohmann::json gpuTimingNodeAverageStatusJson(
    const GpuTimingNodeAverageSnapshot &snapshot, bool enabled,
    bool supported, std::string_view reason);
nlohmann::json disabledGpuTimingNodeAverageStatusJson();

std::string makeGpuTimingSampleLabel(const GpuTimingSampleIdentity &identity);
nlohmann::json gpuTimingAttributionContractJson();
nlohmann::json disabledGpuTimingStatusJson();

DECLARE_MODULE(RenderTiming) {
    struct PendingGpuRange {
        std::uint32_t first_query = 0;
        std::uint32_t query_count = 0;
        GpuTimingRangeIdentity identity;
        std::vector<GpuTimingNodeDescriptor> nodes;
    };

    struct ActiveGpuRange {
        std::size_t pending_index = 0;
        std::uint32_t first_query = 0;
        GpuTimingRangeIdentity identity;
        std::vector<GpuTimingNodeDescriptor> nodes;
    };

    struct GpuAggregate {
        double total_ms = 0.0;
        std::uint64_t samples = 0;
    };

    vk::Device device;
    double timestamp_period_ns = 1.0;
    std::uint32_t timestamp_valid_bits = 0;
    bool gpu_timestamps_supported = true;
    vk::UniqueQueryPool query_pool;
    std::uint32_t configured_frame_slots = 0;
    std::uint32_t configured_range_slots = 0;
    std::uint32_t configured_max_nodes = 0;
    std::uint32_t queries_per_range = 0;
    std::vector<std::optional<PendingGpuRange>> pending_gpu_ranges;
    std::optional<ActiveGpuRange> active_gpu_range;
    std::uint64_t query_pool_create_count = 0;
    std::uint64_t dropped_samples = 0;

    std::deque<GpuTimingHistoryFrame> gpu_history;
    std::vector<GpuTimingViewRow> published_latest_views;
    std::vector<GpuTimingNodeRow> published_latest_nodes;
    std::size_t last_snapshot_history_frame_visits = 0;

    std::uint64_t cpu_frame_count = 0;
    double cpu_update_ms_total = 0.0;
    double cpu_render_ms_total = 0.0;
    double cpu_present_wait_ms_total = 0.0;
    std::unordered_map<std::string, GpuAggregate> gpu_series_totals;
    std::chrono::steady_clock::time_point last_log_time;
    std::vector<std::string> last_frame_node_names;
    std::uint32_t last_frame_query_count = 0;

    vk::UniqueQueryPool createTimestampQueryPool(std::uint32_t query_count) const;
    bool collectGpuRange(std::size_t pending_index, bool wait);
    void collectGpuResults(bool wait);
    void addGpuSample(GpuTimingSample sample);
    GpuTimingNodeFrame makeNodeFrame(
        const GpuTimingHistoryFrame &frame) const;
    void publishSnapshot();
    std::string formatGpuAverages() const;
    void logAndReset();

  public:
    RenderTiming();
    ~RenderTiming();

    // One pool owns fixed disjoint ranges addressed by
    // (in-flight frame slot, recording range slot). Calling this again with
    // equal or smaller limits is a no-op; graph growth drains and recreates it.
    void configureGpuQueries(std::uint32_t frame_slots, std::uint32_t range_slots,
                             std::uint32_t max_nodes);
    void beginGpuRange(vk::CommandBuffer cmd_buf, std::uint32_t frame_slot,
                       std::uint32_t range_slot, GpuTimingRangeIdentity identity,
                       std::vector<GpuTimingNodeDescriptor> nodes);
    void writeNodeSubrangeStart(vk::CommandBuffer cmd_buf, std::uint32_t node_index,
                                GpuTimingSubrange subrange);
    void writeNodeSubrangeEnd(vk::CommandBuffer cmd_buf, std::uint32_t node_index,
                              GpuTimingSubrange subrange);
    void endGpuRange();
    void cancelGpuRange();
    void recordCpuFrame(CpuFrameDurations durations);
    void flush();

    nlohmann::json statusJson() const;
    nlohmann::json nodeAverageJson() const;
    bool timestampsSupported() const noexcept { return gpu_timestamps_supported; }
    std::string_view supportReason() const noexcept {
        return gpu_timestamps_supported ? "enabled"
                                        : "graphics_queue_timestamps_unsupported";
    }
    const std::vector<GpuTimingNodeRow> &latestNodeRows() const noexcept {
        return published_latest_nodes;
    }
    const std::vector<std::string> &lastFrameNodeNamesForTesting() const {
        return last_frame_node_names;
    }
    std::uint32_t lastFrameQueryCountForTesting() const { return last_frame_query_count; }
    std::uint64_t queryPoolCreateCountForTesting() const { return query_pool_create_count; }
    std::size_t historyFrameCountForTesting() const noexcept {
        return gpu_history.size();
    }
    std::size_t lastSnapshotHistoryFrameVisitsForTesting() const noexcept {
        return last_snapshot_history_frame_visits;
    }
    std::size_t pendingRangeCountForTesting() const;
    bool allGpuQueriesCollectedForTesting() const {
        return pendingRangeCountForTesting() == 0 && !active_gpu_range.has_value();
    }
};

} // namespace Pelican
