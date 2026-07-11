#include "startup.hpp"
#include "log.hpp"

namespace Pelican {

namespace {
double elapsedMs(std::chrono::steady_clock::time_point start,
                 std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>{end - start}.count();
}
} // namespace

void StartupMetrics::begin() {
    std::lock_guard lock{mutex};
    values = {};
    start = Clock::now();
}

void StartupMetrics::addConfig(double milliseconds) {
    std::lock_guard lock{mutex};
    values.config_ms += milliseconds;
}

void StartupMetrics::addVulkan(double milliseconds) {
    std::lock_guard lock{mutex};
    values.vulkan_ms += milliseconds;
}

void StartupMetrics::addShader(double milliseconds, bool cache_hit) {
    std::lock_guard lock{mutex};
    values.shaders_ms += milliseconds;
    ++values.shader_cache_requests;
    values.shader_cache_hits += cache_hit ? 1u : 0u;
}

void StartupMetrics::addModels(double milliseconds) {
    std::lock_guard lock{mutex};
    values.models_ms += milliseconds;
}

void StartupMetrics::finishAndLog() {
    StartupSnapshot result;
    {
        std::lock_guard lock{mutex};
        if (!values.complete) {
            values.total_ms = elapsedMs(start, Clock::now());
            values.complete = true;
        }
        result = values;
    }
    LOG_INFO(logger,
             "startup: config {:.1f}ms / vulkan {:.1f}ms / shaders {:.1f}ms (cache hit {}/{}) / "
             "models {:.1f}ms / total {:.1f}ms",
             result.config_ms, result.vulkan_ms, result.shaders_ms, result.shader_cache_hits,
             result.shader_cache_requests, result.models_ms, result.total_ms);
}

StartupSnapshot StartupMetrics::snapshot() const {
    std::lock_guard lock{mutex};
    return values;
}

StartupPhaseTimer::~StartupPhaseTimer() {
    const auto milliseconds = elapsedMs(start, Clock::now());
    (GET_MODULE(StartupMetrics).*record)(milliseconds);
}

} // namespace Pelican
