#pragma once

#include "../container.hpp"
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct CpuFrameDurations {
    double update_ms = 0.0;
    double render_ms = 0.0;
    double present_wait_ms = 0.0;
};

DECLARE_MODULE(RenderTiming) {
    struct PendingGpuFrame {
        vk::UniqueQueryPool pool;
        std::vector<std::string> pass_names;
        uint32_t query_count = 0;
    };

    struct ActiveGpuFrame {
        vk::UniqueQueryPool pool;
        std::vector<std::string> pass_names;
    };

    struct GpuAggregate {
        double total_ms = 0.0;
        uint64_t samples = 0;
    };

    vk::Device device;
    double timestamp_period_ns = 1.0;
    bool gpu_timestamps_supported = true;
    std::vector<PendingGpuFrame> pending_gpu_frames;
    std::optional<ActiveGpuFrame> active_gpu_frame;

    uint64_t cpu_frame_count = 0;
    double cpu_update_ms_total = 0.0;
    double cpu_render_ms_total = 0.0;
    double cpu_present_wait_ms_total = 0.0;
    std::unordered_map<std::string, GpuAggregate> gpu_pass_totals;
    std::chrono::steady_clock::time_point last_log_time;

    vk::UniqueQueryPool createTimestampQueryPool(uint32_t query_count) const;
    void collectGpuResults(bool wait);
    void addGpuSample(const std::string &pass_name, uint64_t start_tick, uint64_t end_tick);
    std::string formatGpuAverages() const;
    void logAndReset();

  public:
    RenderTiming();
    ~RenderTiming();

    void beginGpuFrame(vk::CommandBuffer cmd_buf, const std::vector<std::string> &pass_names);
    void writePassStart(vk::CommandBuffer cmd_buf, uint32_t pass_index);
    void writePassEnd(vk::CommandBuffer cmd_buf, uint32_t pass_index);
    void endGpuFrame();
    void recordCpuFrame(CpuFrameDurations durations);
    void flush();
};

} // namespace Pelican
