#include "rendertiming.hpp"

#include "../log.hpp"
#include "core.hpp"
#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

constexpr auto log_interval = std::chrono::seconds{1};

bool timestampsSupported(vk::PhysicalDevice physical_device, uint32_t graphics_queue_family) {
    const auto queue_families = physical_device.getQueueFamilyProperties();
    return graphics_queue_family < queue_families.size() &&
           queue_families[graphics_queue_family].timestampValidBits > 0;
}

} // namespace

RenderTiming::RenderTiming()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      timestamp_period_ns{GET_MODULE(VulkanManageCore).getPhysDevice()
                              .getProperties()
                              .limits.timestampPeriod},
      gpu_timestamps_supported{timestampsSupported(GET_MODULE(VulkanManageCore).getPhysDevice(),
                                                   GET_MODULE(VulkanManageCore).getGraphicsQueueFamilyIndex())},
      last_log_time{std::chrono::steady_clock::now()} {}

RenderTiming::~RenderTiming() = default;

vk::UniqueQueryPool RenderTiming::createTimestampQueryPool(uint32_t query_count) const {
    vk::QueryPoolCreateInfo create_info;
    create_info.queryType = vk::QueryType::eTimestamp;
    create_info.queryCount = query_count;
    return device.createQueryPoolUnique(create_info);
}

void RenderTiming::beginGpuFrame(vk::CommandBuffer cmd_buf, const std::vector<std::string> &pass_names) {
    last_frame_node_names = pass_names;
    last_frame_query_count = 0;
    if (!gpu_timestamps_supported || pass_names.empty()) {
        return;
    }
    if (active_gpu_frame.has_value()) {
        throw std::runtime_error("RenderTiming beginGpuFrame called while another GPU frame is active");
    }

    const auto query_count = static_cast<uint32_t>(pass_names.size() * 2);
    last_frame_query_count = query_count;
    auto pool = createTimestampQueryPool(query_count);
    cmd_buf.resetQueryPool(pool.get(), 0, query_count);
    active_gpu_frame = ActiveGpuFrame{std::move(pool), pass_names};
}

void RenderTiming::writePassStart(vk::CommandBuffer cmd_buf, uint32_t pass_index) {
    if (!active_gpu_frame.has_value()) {
        return;
    }
    if (pass_index >= active_gpu_frame->pass_names.size()) {
        throw std::runtime_error("RenderTiming pass start index out of range");
    }
    cmd_buf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, active_gpu_frame->pool.get(),
                           pass_index * 2);
}

void RenderTiming::writePassEnd(vk::CommandBuffer cmd_buf, uint32_t pass_index) {
    if (!active_gpu_frame.has_value()) {
        return;
    }
    if (pass_index >= active_gpu_frame->pass_names.size()) {
        throw std::runtime_error("RenderTiming pass end index out of range");
    }
    cmd_buf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, active_gpu_frame->pool.get(),
                           pass_index * 2 + 1);
}

void RenderTiming::endGpuFrame() {
    if (!active_gpu_frame.has_value()) {
        return;
    }

    const auto query_count = static_cast<uint32_t>(active_gpu_frame->pass_names.size() * 2);
    pending_gpu_frames.push_back(PendingGpuFrame{
        std::move(active_gpu_frame->pool),
        std::move(active_gpu_frame->pass_names),
        query_count,
    });
    active_gpu_frame.reset();
}

void RenderTiming::addGpuSample(const std::string &pass_name, uint64_t start_tick, uint64_t end_tick) {
    if (end_tick < start_tick) {
        return;
    }
    auto &aggregate = gpu_pass_totals[pass_name];
    aggregate.total_ms += static_cast<double>(end_tick - start_tick) * timestamp_period_ns / 1'000'000.0;
    aggregate.samples += 1;
}

void RenderTiming::collectGpuResults(bool wait) {
    auto frame = pending_gpu_frames.begin();
    while (frame != pending_gpu_frames.end()) {
        std::vector<uint64_t> results(static_cast<size_t>(frame->query_count) * 2, 0);
        auto flags = vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability;
        if (wait) {
            flags |= vk::QueryResultFlagBits::eWait;
        }
        const auto result = device.getQueryPoolResults(frame->pool.get(), 0, frame->query_count,
                                                       results.size() * sizeof(uint64_t),
                                                       results.data(), sizeof(uint64_t) * 2, flags);
        if (result == vk::Result::eNotReady) {
            ++frame;
            continue;
        }
        if (result != vk::Result::eSuccess) {
            throw std::runtime_error("vkGetQueryPoolResults failed: " + vk::to_string(result));
        }

        bool available = true;
        for (uint32_t i = 0; i < frame->query_count; ++i) {
            if (results[static_cast<size_t>(i) * 2 + 1] == 0) {
                available = false;
                break;
            }
        }
        if (!available) {
            ++frame;
            continue;
        }

        for (size_t pass_index = 0; pass_index < frame->pass_names.size(); ++pass_index) {
            const auto start_tick = results[pass_index * 4];
            const auto end_tick = results[pass_index * 4 + 2];
            addGpuSample(frame->pass_names[pass_index], start_tick, end_tick);
        }
        frame = pending_gpu_frames.erase(frame);
    }
}

std::string RenderTiming::formatGpuAverages() const {
    if (!gpu_timestamps_supported) {
        return "unsupported";
    }
    if (gpu_pass_totals.empty()) {
        return "pending";
    }

    std::vector<std::string> pass_names;
    pass_names.reserve(gpu_pass_totals.size());
    for (const auto &[name, _] : gpu_pass_totals) {
        pass_names.push_back(name);
    }
    std::sort(pass_names.begin(), pass_names.end());

    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3);
    bool first = true;
    for (const auto &name : pass_names) {
        const auto &aggregate = gpu_pass_totals.at(name);
        if (!first) {
            stream << ",";
        }
        stream << name << "=" << (aggregate.total_ms / static_cast<double>(aggregate.samples));
        first = false;
    }
    return stream.str();
}

void RenderTiming::logAndReset() {
    if (cpu_frame_count == 0) {
        return;
    }

    LOG_INFO(logger,
             "pelican frame metrics frames={} cpu_update_ms_avg={:.3f} cpu_render_ms_avg={:.3f} "
             "cpu_present_wait_ms_avg={:.3f} gpu_ms_avg={}",
             cpu_frame_count,
             cpu_update_ms_total / static_cast<double>(cpu_frame_count),
             cpu_render_ms_total / static_cast<double>(cpu_frame_count),
             cpu_present_wait_ms_total / static_cast<double>(cpu_frame_count),
             formatGpuAverages());

    cpu_frame_count = 0;
    cpu_update_ms_total = 0.0;
    cpu_render_ms_total = 0.0;
    cpu_present_wait_ms_total = 0.0;
    gpu_pass_totals.clear();
    last_log_time = std::chrono::steady_clock::now();
}

void RenderTiming::recordCpuFrame(CpuFrameDurations durations) {
    cpu_frame_count += 1;
    cpu_update_ms_total += durations.update_ms;
    cpu_render_ms_total += durations.render_ms;
    cpu_present_wait_ms_total += durations.present_wait_ms;
    collectGpuResults(false);

    if (std::chrono::steady_clock::now() - last_log_time >= log_interval) {
        logAndReset();
    }
}

void RenderTiming::flush() {
    collectGpuResults(true);
    logAndReset();
}

} // namespace Pelican
