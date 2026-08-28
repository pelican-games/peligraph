#include "rendertiming.hpp"

#include "../log.hpp"
#include "core.hpp"
#include "debugutils.hpp"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace Pelican {

namespace {

constexpr auto log_interval = std::chrono::seconds{1};
constexpr std::uint32_t timestamp_queries_per_node = 4;
using NodeKey = std::tuple<std::uint32_t, std::size_t, std::string,
                           std::string>;

std::uint32_t timestampValidBits(vk::PhysicalDevice physical_device,
                                 std::uint32_t graphics_queue_family) {
    const auto queue_families = physical_device.getQueueFamilyProperties();
    return graphics_queue_family < queue_families.size()
               ? queue_families[graphics_queue_family].timestampValidBits
               : 0;
}

std::string viewLabel(std::string_view graph_variant, std::uint32_t view_index) {
    if (graph_variant == "flat" && view_index == 0) return "flat";
    if (graph_variant == "xr") {
        if (view_index == 0) return "left";
        if (view_index == 1) return "right";
        if (view_index == 2) return "mirror";
    }
    return "view_" + std::to_string(view_index);
}

std::string aggregateKey(const GpuTimingSampleIdentity &identity) {
    return "graph/" + identity.graph_variant + "/view/" +
           std::to_string(identity.view_index) + "/node/" +
           std::to_string(identity.node_ordinal) + ":" + identity.node_kind + ":" +
           identity.node_name + "/" + std::string{gpuTimingSubrangeName(identity.subrange)};
}

template <class Sample>
nlohmann::json sampleJson(const Sample &sample) {
    return nlohmann::json{
        {"logical_frame", sample.identity.logical_frame},
        {"graph_variant", sample.identity.graph_variant},
        {"view_index", sample.identity.view_index},
        {"node_ordinal", sample.identity.node_ordinal},
        {"node_kind", sample.identity.node_kind},
        {"node_name", sample.identity.node_name},
        {"subrange", gpuTimingSubrangeName(sample.identity.subrange)},
        {"identity", makeGpuTimingSampleLabel(sample.identity)},
        {"supported", sample.supported},
        {"reason", sample.reason.empty() ? nlohmann::json(nullptr)
                                         : nlohmann::json(sample.reason)},
        {"ms", sample.ms},
    };
}

std::vector<const GpuTimingSample *> canonicalSamplePointers(
    const GpuTimingHistoryFrame &frame) {
    std::vector<const GpuTimingSample *> result;
    result.reserve(frame.samples.size());
    for (const auto &sample : frame.samples) {
        result.push_back(&sample);
    }
    std::sort(
        result.begin(), result.end(),
        [](const auto *left, const auto *right) {
            return std::tie(left->identity.view_index,
                            left->identity.node_ordinal,
                            left->identity.node_kind,
                            left->identity.node_name,
                            left->identity.subrange) <
                   std::tie(right->identity.view_index,
                            right->identity.node_ordinal,
                            right->identity.node_kind,
                            right->identity.node_name,
                            right->identity.subrange);
        });
    return result;
}

GpuTimingNodeFrame makeGpuTimingNodeFrame(
    const GpuTimingHistoryFrame &frame) {
    std::map<NodeKey, GpuTimingNodeRow> rows;
    for (const auto &sample : frame.samples) {
        const NodeKey key{sample.identity.view_index,
                          sample.identity.node_ordinal,
                          sample.identity.node_kind,
                          sample.identity.node_name};
        auto [found, inserted] = rows.try_emplace(
            key,
            GpuTimingNodeRow{
                frame.logical_frame, frame.graph_variant,
                sample.identity.view_index, sample.identity.node_ordinal,
                sample.identity.node_kind, sample.identity.node_name});
        auto &row = found->second;
        if (sample.identity.subrange == GpuTimingSubrange::barriers) {
            row.barriers_ms = sample.ms;
        } else {
            row.body_ms = sample.ms;
            row.body_supported = sample.supported;
        }
    }

    GpuTimingNodeFrame result{frame.logical_frame, frame.graph_variant, {}};
    result.nodes.reserve(rows.size());
    for (auto &[_, row] : rows) {
        result.nodes.push_back(std::move(row));
    }
    return result;
}

} // namespace

std::string_view gpuTimingSubrangeName(GpuTimingSubrange subrange) noexcept {
    switch (subrange) {
    case GpuTimingSubrange::barriers: return "barriers";
    case GpuTimingSubrange::body: return "body";
    }
    return "unknown";
}

std::string makeGpuTimingSampleLabel(const GpuTimingSampleIdentity &identity) {
    return makeFrameGraphDebugLabel(FrameGraphDebugLabelIdentity{
               identity.logical_frame, identity.graph_variant, identity.view_index,
               identity.node_ordinal, identity.node_kind, identity.node_name}) +
           "/" + std::string{gpuTimingSubrangeName(identity.subrange)};
}

nlohmann::json gpuTimingAttributionContractJson() {
    return nlohmann::json{
        {"schema", "pelican.gpu_timing_attribution"},
        {"version", 1},
        {"subrange_order", {"barriers", "body"}},
        {"rows",
         {
             {{"node_kind", "render"}, {"operation", "frame_graph"},
              {"barriers", "compiled incoming barriers"},
              {"body", "node-owned image transitions and render work"},
              {"body_support", "supported"}},
             {{"node_kind", "compute"}, {"operation", "frame_graph"},
              {"barriers", "compiled incoming barriers"},
              {"body", "node-owned resource transitions and dispatch"},
              {"body_support", "supported"}},
             {{"node_kind", "anchor"}, {"operation", "frame_graph"},
              {"barriers", "compiled incoming barriers"},
              {"body", "anchor-owned work such as sprite draw"},
              {"body_support", "unsupported with zero ms when no work is bound"}},
             {{"node_kind", "snapshot_copy"}, {"operation", "frame_graph"},
              {"barriers", "compiled incoming barriers"},
              {"body", "copy-owned image transitions and copy"},
              {"body_support", "supported"}},
             {{"node_kind", "output_transform"}, {"operation", "frame_graph"},
              {"barriers", "compiled incoming barriers"},
              {"body", "output-transform transitions and draw"},
              {"body_support", "supported"}},
             {{"node_kind", "mirror"}, {"operation", "xr_mirror_intermediate"},
              {"barriers", "pre-copy display/intermediate layout transitions"},
              {"body", "engine-owned left-eye copy and final layout transition"},
              {"body_support", "supported"}},
             {{"node_kind", "mirror"}, {"operation", "desktop_mirror"},
              {"barriers", "empty; swapchain acquire transition is outside the node"},
              {"body", "clear, output transform, and screen UI"},
              {"body_support", "supported when the best-effort sink presents"}},
         }},
    };
}

nlohmann::json disabledGpuTimingStatusJson() {
    return nlohmann::json{
        {"schema_version", 2},
        {"enabled", false},
        {"supported", false},
        {"reason", "feature_not_enabled"},
        {"history_capacity", gpu_timing_history_capacity},
        {"history_count", 0},
        {"dropped_samples", 0},
        {"logical_frame_averages",
         nlohmann::json::array()},
        {"logical_frame_history",
         nlohmann::json::array()},
        {"logical_frame_total_sum_views_ms", 0.0},
        {"views", nlohmann::json::array()},
        {"nodes", nlohmann::json::array()},
        {"query_pool", {{"frame_slots", 0}, {"range_slots", 0},
                         {"max_nodes", 0}, {"query_capacity", 0},
                         {"create_count", 0}, {"pending_ranges", 0}}},
    };
}

GpuTimingPublishedSnapshot publishLatestGpuTimingSnapshot(
    const std::deque<GpuTimingHistoryFrame> &history) {
    GpuTimingPublishedSnapshot result;
    if (history.empty()) return result;

    // This loop intentionally contains one frame.  The measured visit count
    // belongs to the traversal itself, so widening first to history.begin()
    // truthfully reports an eager full-history publisher.
    const auto first = std::prev(history.end());
    for (auto frame = first; frame != history.end(); ++frame) {
        ++result.history_frame_visits;
        result.latest_nodes = makeGpuTimingNodeFrame(*frame).nodes;

        std::map<std::uint32_t, GpuTimingViewRow> rows;
        const auto ordered_samples =
            canonicalSamplePointers(*frame);
        const std::span<const GpuTimingSample *const> sample_span{
            ordered_samples};
        for (const auto *sample : sample_span) {
            auto [found, inserted] = rows.try_emplace(
                sample->identity.view_index,
                GpuTimingViewRow{
                    frame->logical_frame, frame->graph_variant,
                    sample->identity.view_index,
                    viewLabel(frame->graph_variant,
                              sample->identity.view_index)});
            auto &row = found->second;
            if (sample->identity.subrange ==
                GpuTimingSubrange::barriers) {
                row.barriers_ms += sample->ms;
            } else {
                row.body_ms += sample->ms;
            }
            row.total_ms += sample->ms;
        }
        result.latest_views.clear();
        result.latest_views.reserve(rows.size());
        for (const auto &[_, row] : rows) {
            result.latest_views.push_back(row);
        }
    }
    return result;
}

GpuTimingStatusProjection projectGpuTimingStatus(
    const std::deque<GpuTimingHistoryFrame> &history,
    std::span<const GpuTimingViewRow> latest_views) {
    GpuTimingStatusProjection result{
        .logical_frame_averages = nlohmann::json::array(),
        .logical_frame_history = nlohmann::json::array(),
        .views = nlohmann::json::array(),
        .nodes = nlohmann::json::array(),
    };

    for (const auto &frame : history) {
        const auto ordered_samples =
            canonicalSamplePointers(frame);
        const std::span<const GpuTimingSample *const> sample_span{
            ordered_samples};
        for (const auto *sample : sample_span) {
            result.nodes.push_back(sampleJson(*sample));
        }
    }

    struct LogicalFrameAggregate {
        double total_ms = 0.0;
        double min_ms = std::numeric_limits<double>::max();
        double max_ms = 0.0;
        std::uint64_t frame_count = 0;
    };
    std::map<std::string, LogicalFrameAggregate>
        logical_frame_aggregates;
    for (const auto &frame : history) {
        double total_ms = 0.0;
        std::uint64_t supported_samples = 0;
        const auto ordered_samples =
            canonicalSamplePointers(frame);
        const std::span<const GpuTimingSample *const> sample_span{
            ordered_samples};
        for (const auto *sample : sample_span) {
            if (!sample->supported) continue;
            total_ms += sample->ms;
            ++supported_samples;
        }
        result.logical_frame_history.push_back({
            {"logical_frame", frame.logical_frame},
            {"graph_variant", frame.graph_variant},
            {"total_ms", total_ms},
            {"supported_sample_count", supported_samples},
        });
        auto &aggregate =
            logical_frame_aggregates[frame.graph_variant];
        aggregate.total_ms += total_ms;
        aggregate.min_ms =
            std::min(aggregate.min_ms, total_ms);
        aggregate.max_ms =
            std::max(aggregate.max_ms, total_ms);
        ++aggregate.frame_count;
    }
    for (const auto &[graph_variant, aggregate] :
         logical_frame_aggregates) {
        result.logical_frame_averages.push_back({
            {"graph_variant", graph_variant},
            {"frame_count", aggregate.frame_count},
            {"average_total_ms",
             aggregate.total_ms /
                 static_cast<double>(aggregate.frame_count)},
            {"min_total_ms", aggregate.min_ms},
            {"max_total_ms", aggregate.max_ms},
        });
    }

    for (const auto &row : latest_views) {
        result.logical_frame_total_sum_views_ms +=
            row.total_ms;
        result.views.push_back({
            {"logical_frame", row.logical_frame},
            {"graph_variant", row.graph_variant},
            {"view_index", row.view_index},
            {"label", row.label},
            {"barriers_ms", row.barriers_ms},
            {"body_ms", row.body_ms},
            {"total_ms", row.total_ms},
        });
    }
    return result;
}

GpuTimingNodeAverageSnapshot averageGpuTimingNodeFrames(
    std::span<const GpuTimingNodeFrame> frames, std::size_t window) {
    if (window == 0) {
        throw std::invalid_argument(
            "GPU timing node average window must be non-zero");
    }

    struct Aggregate {
        GpuTimingNodeAverageRow row;
        double barriers_ms_total = 0.0;
        double body_ms_total = 0.0;
    };

    GpuTimingNodeAverageSnapshot result;
    result.frame_count = std::min(window, frames.size());
    const auto first = frames.end() -
                       static_cast<std::ptrdiff_t>(result.frame_count);
    std::map<NodeKey, Aggregate> aggregates;
    for (auto frame = first; frame != frames.end(); ++frame) {
        for (const auto &node : frame->nodes) {
            const NodeKey key{node.view_index, node.node_ordinal,
                              node.node_kind, node.node_name};
            auto [found, inserted] = aggregates.try_emplace(key);
            auto &aggregate = found->second;
            if (inserted) {
                aggregate.row = GpuTimingNodeAverageRow{
                    node.logical_frame, node.graph_variant, node.view_index,
                    node.node_ordinal, node.node_kind, node.node_name,
                    0.0, 0.0, node.body_supported, 0};
            } else {
                aggregate.row.logical_frame = node.logical_frame;
                aggregate.row.graph_variant = node.graph_variant;
                aggregate.row.body_supported =
                    aggregate.row.body_supported && node.body_supported;
            }
            aggregate.barriers_ms_total += node.barriers_ms;
            aggregate.body_ms_total += node.body_ms;
            ++aggregate.row.sample_count;
        }
    }

    result.nodes.reserve(aggregates.size());
    for (auto &[_, aggregate] : aggregates) {
        const auto divisor =
            static_cast<double>(aggregate.row.sample_count);
        aggregate.row.barriers_ms =
            aggregate.barriers_ms_total / divisor;
        aggregate.row.body_ms = aggregate.body_ms_total / divisor;
        result.nodes.push_back(std::move(aggregate.row));
    }
    return result;
}

nlohmann::json gpuTimingNodeAverageStatusJson(
    const GpuTimingNodeAverageSnapshot &snapshot, bool enabled,
    bool supported, std::string_view reason) {
    auto nodes = nlohmann::json::array();
    for (const auto &row : snapshot.nodes) {
        nodes.push_back({
            {"logical_frame", row.logical_frame},
            {"graph_variant", row.graph_variant},
            {"view_index", row.view_index},
            {"node_ordinal", row.node_ordinal},
            {"node_kind", row.node_kind},
            {"node_name", row.node_name},
            {"barriers_ms", row.barriers_ms},
            {"body_ms", row.body_ms},
            {"body_supported", row.body_supported},
            {"sample_count", row.sample_count},
        });
    }
    return nlohmann::json{
        {"schema", "pelican.gpu_timing_node_averages"},
        {"version", 1},
        {"enabled", enabled},
        {"supported", supported},
        {"reason", std::string{reason}},
        {"window_size", gpu_timing_node_average_window},
        {"frame_count", snapshot.frame_count},
        {"nodes", std::move(nodes)},
    };
}

nlohmann::json disabledGpuTimingNodeAverageStatusJson() {
    return gpuTimingNodeAverageStatusJson(
        {}, false, false, "feature_not_enabled");
}

RenderTiming::RenderTiming()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      timestamp_period_ns{GET_MODULE(VulkanManageCore).getPhysDevice()
                              .getProperties()
                              .limits.timestampPeriod},
      timestamp_valid_bits{timestampValidBits(
          GET_MODULE(VulkanManageCore).getPhysDevice(),
          GET_MODULE(VulkanManageCore).getGraphicsQueueFamilyIndex())},
      gpu_timestamps_supported{timestamp_valid_bits > 0},
      last_log_time{std::chrono::steady_clock::now()} {
    publishSnapshot();
}

RenderTiming::~RenderTiming() = default;

vk::UniqueQueryPool RenderTiming::createTimestampQueryPool(std::uint32_t query_count) const {
    vk::QueryPoolCreateInfo create_info;
    create_info.queryType = vk::QueryType::eTimestamp;
    create_info.queryCount = query_count;
    return device.createQueryPoolUnique(create_info);
}

void RenderTiming::configureGpuQueries(std::uint32_t frame_slots,
                                       std::uint32_t range_slots,
                                       std::uint32_t max_nodes) {
    if (frame_slots == 0 || range_slots == 0 || max_nodes == 0) {
        throw std::runtime_error("RenderTiming query dimensions must be non-zero");
    }
    if (frame_slots <= configured_frame_slots && range_slots <= configured_range_slots &&
        max_nodes <= configured_max_nodes) {
        return;
    }
    if (active_gpu_range) {
        throw std::runtime_error("RenderTiming cannot grow the query ring while a range is active");
    }
    collectGpuResults(true);

    configured_frame_slots = std::max(configured_frame_slots, frame_slots);
    configured_range_slots = std::max(configured_range_slots, range_slots);
    configured_max_nodes = std::max(configured_max_nodes, max_nodes);
    queries_per_range = configured_max_nodes * timestamp_queries_per_node;

    const auto capacity64 = static_cast<std::uint64_t>(configured_frame_slots) *
                            configured_range_slots * queries_per_range;
    if (capacity64 > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("RenderTiming query capacity exceeds Vulkan uint32 range");
    }
    pending_gpu_ranges.clear();
    pending_gpu_ranges.resize(static_cast<std::size_t>(configured_frame_slots) *
                              configured_range_slots);
    if (gpu_timestamps_supported) {
        query_pool = createTimestampQueryPool(static_cast<std::uint32_t>(capacity64));
        ++query_pool_create_count;
    }
    publishSnapshot();
}

void RenderTiming::beginGpuRange(vk::CommandBuffer cmd_buf, std::uint32_t frame_slot,
                                 std::uint32_t range_slot,
                                 GpuTimingRangeIdentity identity,
                                 std::vector<GpuTimingNodeDescriptor> nodes) {
    if (identity.view_index == 0 && !nodes.empty() && nodes.front().node_kind != "mirror") {
        last_frame_node_names.clear();
        last_frame_node_names.reserve(nodes.size());
        for (const auto &node : nodes) last_frame_node_names.push_back(node.node_name);
        last_frame_query_count = gpu_timestamps_supported
                                     ? static_cast<std::uint32_t>(nodes.size()) *
                                           timestamp_queries_per_node
                                     : 0;
    }
    if (!gpu_timestamps_supported || nodes.empty()) return;
    if (!query_pool) {
        throw std::runtime_error("RenderTiming query ring was not configured before recording");
    }
    if (active_gpu_range) {
        throw std::runtime_error("RenderTiming beginGpuRange called while another range is active");
    }
    if (frame_slot >= configured_frame_slots || range_slot >= configured_range_slots ||
        nodes.size() > configured_max_nodes) {
        throw std::runtime_error("RenderTiming recording range exceeds configured query capacity");
    }

    const auto pending_index = static_cast<std::size_t>(frame_slot) *
                                   configured_range_slots +
                               range_slot;
    if (pending_gpu_ranges[pending_index]) {
        if (!collectGpuRange(pending_index, true)) {
            throw std::runtime_error("RenderTiming failed to reclaim an in-flight query range");
        }
        publishSnapshot();
    }
    const auto first_query = static_cast<std::uint32_t>(pending_index) * queries_per_range;
    cmd_buf.resetQueryPool(query_pool.get(), first_query, queries_per_range);
    active_gpu_range = ActiveGpuRange{
        pending_index, first_query, std::move(identity), std::move(nodes)};
}

void RenderTiming::writeNodeSubrangeStart(vk::CommandBuffer cmd_buf,
                                          std::uint32_t node_index,
                                          GpuTimingSubrange subrange) {
    if (!active_gpu_range) return;
    if (node_index >= active_gpu_range->nodes.size()) {
        throw std::runtime_error("RenderTiming node subrange start index out of range");
    }
    const auto subrange_index = static_cast<std::uint32_t>(subrange);
    const auto query = active_gpu_range->first_query +
                       node_index * timestamp_queries_per_node + subrange_index * 2;
    cmd_buf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, query_pool.get(), query);
}

void RenderTiming::writeNodeSubrangeEnd(vk::CommandBuffer cmd_buf,
                                        std::uint32_t node_index,
                                        GpuTimingSubrange subrange) {
    if (!active_gpu_range) return;
    if (node_index >= active_gpu_range->nodes.size()) {
        throw std::runtime_error("RenderTiming node subrange end index out of range");
    }
    const auto subrange_index = static_cast<std::uint32_t>(subrange);
    const auto query = active_gpu_range->first_query +
                       node_index * timestamp_queries_per_node + subrange_index * 2 + 1;
    cmd_buf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, query_pool.get(), query);
}

void RenderTiming::endGpuRange() {
    if (!active_gpu_range) return;
    const auto query_count = static_cast<std::uint32_t>(active_gpu_range->nodes.size()) *
                             timestamp_queries_per_node;
    pending_gpu_ranges[active_gpu_range->pending_index] = PendingGpuRange{
        active_gpu_range->first_query, query_count,
        std::move(active_gpu_range->identity), std::move(active_gpu_range->nodes)};
    active_gpu_range.reset();
}

void RenderTiming::cancelGpuRange() {
    if (!active_gpu_range) return;
    dropped_samples += active_gpu_range->nodes.size() * 2;
    active_gpu_range.reset();
}

void RenderTiming::addGpuSample(GpuTimingSample sample) {
    auto frame = std::find_if(gpu_history.begin(), gpu_history.end(), [&](const auto &candidate) {
        return candidate.logical_frame == sample.identity.logical_frame &&
               candidate.graph_variant == sample.identity.graph_variant;
    });
    if (frame == gpu_history.end()) {
        if (gpu_history.size() == gpu_timing_history_capacity) gpu_history.pop_front();
        gpu_history.push_back(GpuTimingHistoryFrame{
            sample.identity.logical_frame, sample.identity.graph_variant, {}});
        frame = std::prev(gpu_history.end());
    }
    if (sample.supported) {
        auto &aggregate = gpu_series_totals[aggregateKey(sample.identity)];
        aggregate.total_ms += sample.ms;
        ++aggregate.samples;
    }
    frame->samples.push_back(std::move(sample));
}

bool RenderTiming::collectGpuRange(std::size_t pending_index, bool wait) {
    auto &pending = pending_gpu_ranges.at(pending_index);
    if (!pending) return true;

    std::vector<std::uint64_t> results(static_cast<std::size_t>(pending->query_count) * 2, 0);
    auto flags = vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability;
    if (wait) flags |= vk::QueryResultFlagBits::eWait;
    const auto result = device.getQueryPoolResults(
        query_pool.get(), pending->first_query, pending->query_count,
        results.size() * sizeof(std::uint64_t), results.data(),
        sizeof(std::uint64_t) * 2, flags);
    if (result == vk::Result::eNotReady) return false;
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error("vkGetQueryPoolResults failed: " + vk::to_string(result));
    }
    for (std::uint32_t query = 0; query < pending->query_count; ++query) {
        if (results[static_cast<std::size_t>(query) * 2 + 1] == 0) return false;
    }

    const auto timestamp_delta = [&](std::uint64_t start, std::uint64_t end) {
        if (timestamp_valid_bits >= 64) return end - start;
        const auto mask = (std::uint64_t{1} << timestamp_valid_bits) - 1;
        return (end - start) & mask;
    };
    for (std::size_t node_index = 0; node_index < pending->nodes.size(); ++node_index) {
        const auto &node = pending->nodes[node_index];
        for (const auto subrange : {GpuTimingSubrange::barriers,
                                    GpuTimingSubrange::body}) {
            const auto subrange_index = static_cast<std::size_t>(subrange);
            const auto query = node_index * timestamp_queries_per_node +
                               subrange_index * 2;
            const auto start_tick = results[query * 2];
            const auto end_tick = results[(query + 1) * 2];
            const bool supported = subrange != GpuTimingSubrange::body ||
                                   node.body_supported;
            addGpuSample(GpuTimingSample{
                GpuTimingSampleIdentity{
                    pending->identity.logical_frame, pending->identity.graph_variant,
                    pending->identity.view_index, node.node_ordinal, node.node_kind,
                    node.node_name, subrange},
                supported,
                supported ? std::string{} : std::string{"no_gpu_work"},
                supported ? static_cast<double>(timestamp_delta(start_tick, end_tick)) *
                                timestamp_period_ns / 1'000'000.0
                          : 0.0,
            });
        }
    }
    pending.reset();
    return true;
}

void RenderTiming::collectGpuResults(bool wait) {
    bool changed = false;
    for (std::size_t i = 0; i < pending_gpu_ranges.size(); ++i) {
        const bool had_pending = pending_gpu_ranges[i].has_value();
        if (collectGpuRange(i, wait) && had_pending) changed = true;
    }
    if (changed) publishSnapshot();
}

GpuTimingNodeFrame RenderTiming::makeNodeFrame(
    const GpuTimingHistoryFrame &frame) const {
    return makeGpuTimingNodeFrame(frame);
}

void RenderTiming::publishSnapshot() {
    auto snapshot =
        publishLatestGpuTimingSnapshot(gpu_history);
    published_latest_views =
        std::move(snapshot.latest_views);
    published_latest_nodes =
        std::move(snapshot.latest_nodes);
    last_snapshot_history_frame_visits =
        snapshot.history_frame_visits;
}

nlohmann::json RenderTiming::nodeAverageJson() const {
    const auto frame_count =
        std::min(gpu_timing_node_average_window, gpu_history.size());
    std::vector<GpuTimingNodeFrame> node_frames;
    node_frames.reserve(frame_count);
    const auto first = gpu_history.end() -
                       static_cast<std::ptrdiff_t>(frame_count);
    for (auto frame = first; frame != gpu_history.end(); ++frame) {
        node_frames.push_back(makeNodeFrame(*frame));
    }
    return gpuTimingNodeAverageStatusJson(
        averageGpuTimingNodeFrames(node_frames), true,
        gpu_timestamps_supported, supportReason());
}

nlohmann::json RenderTiming::statusJson() const {
    auto projection = projectGpuTimingStatus(
        gpu_history,
        std::span<const GpuTimingViewRow>{published_latest_views});
    const auto query_capacity =
        static_cast<std::uint64_t>(configured_frame_slots) *
        configured_range_slots * queries_per_range;
    return nlohmann::json{
        {"schema_version", 2},
        {"enabled", true},
        {"supported", gpu_timestamps_supported},
        {"reason", gpu_timestamps_supported
                       ? "enabled"
                       : "graphics_queue_timestamps_unsupported"},
        {"history_capacity", gpu_timing_history_capacity},
        {"history_count", gpu_history.size()},
        {"dropped_samples", dropped_samples},
        {"logical_frame_averages",
         std::move(projection.logical_frame_averages)},
        {"logical_frame_history",
         std::move(projection.logical_frame_history)},
        {"logical_frame_total_sum_views_ms",
         projection.logical_frame_total_sum_views_ms},
        {"views", std::move(projection.views)},
        {"nodes", std::move(projection.nodes)},
        {"query_pool",
         {{"frame_slots", configured_frame_slots},
          {"range_slots", configured_range_slots},
          {"max_nodes", configured_max_nodes},
          {"query_capacity", query_capacity},
          {"create_count", query_pool_create_count},
          {"pending_ranges", pendingRangeCountForTesting()}}},
    };
}

std::string RenderTiming::formatGpuAverages() const {
    if (!gpu_timestamps_supported) return "unsupported";
    if (gpu_series_totals.empty()) return "pending";

    std::vector<std::string> keys;
    keys.reserve(gpu_series_totals.size());
    for (const auto &[key, _] : gpu_series_totals) keys.push_back(key);
    std::sort(keys.begin(), keys.end());

    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3);
    bool first = true;
    for (const auto &key : keys) {
        const auto &aggregate = gpu_series_totals.at(key);
        if (!first) stream << ",";
        stream << key << "="
               << (aggregate.total_ms / static_cast<double>(aggregate.samples));
        first = false;
    }
    return stream.str();
}

void RenderTiming::logAndReset() {
    if (cpu_frame_count == 0) return;
    LOG_INFO(logger,
             "pelican frame metrics frames={} cpu_update_ms_avg={:.3f} "
             "cpu_render_ms_avg={:.3f} cpu_present_wait_ms_avg={:.3f} gpu_ms_avg={}",
             cpu_frame_count,
             cpu_update_ms_total / static_cast<double>(cpu_frame_count),
             cpu_render_ms_total / static_cast<double>(cpu_frame_count),
             cpu_present_wait_ms_total / static_cast<double>(cpu_frame_count),
             formatGpuAverages());
    cpu_frame_count = 0;
    cpu_update_ms_total = 0.0;
    cpu_render_ms_total = 0.0;
    cpu_present_wait_ms_total = 0.0;
    gpu_series_totals.clear();
    last_log_time = std::chrono::steady_clock::now();
}

void RenderTiming::recordCpuFrame(CpuFrameDurations durations) {
    ++cpu_frame_count;
    cpu_update_ms_total += durations.update_ms;
    cpu_render_ms_total += durations.render_ms;
    cpu_present_wait_ms_total += durations.present_wait_ms;
    collectGpuResults(false);
    if (std::chrono::steady_clock::now() - last_log_time >= log_interval) logAndReset();
}

void RenderTiming::flush() {
    collectGpuResults(true);
    logAndReset();
}

std::size_t RenderTiming::pendingRangeCountForTesting() const {
    return static_cast<std::size_t>(std::count_if(
        pending_gpu_ranges.begin(), pending_gpu_ranges.end(),
        [](const auto &pending) { return pending.has_value(); }));
}

} // namespace Pelican
