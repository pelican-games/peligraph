#pragma once

#include "container.hpp"
#include <chrono>
#include <cstddef>
#include <mutex>

namespace Pelican {

struct StartupSnapshot {
    double config_ms = 0.0;
    double vulkan_ms = 0.0;
    double shaders_ms = 0.0;
    double models_ms = 0.0;
    double total_ms = 0.0;
    std::size_t shader_cache_hits = 0;
    std::size_t shader_cache_requests = 0;
    bool complete = false;
};

DECLARE_MODULE(StartupMetrics) {
    using Clock = std::chrono::steady_clock;

    mutable std::mutex mutex;
    Clock::time_point start{};
    StartupSnapshot values;

  public:
    void begin();
    void addConfig(double milliseconds);
    void addVulkan(double milliseconds);
    void addShader(double milliseconds, bool cache_hit);
    void addModels(double milliseconds);
    void finishAndLog();
    StartupSnapshot snapshot() const;
};

class StartupPhaseTimer {
    using Clock = std::chrono::steady_clock;

    void (StartupMetrics::*record)(double);
    Clock::time_point start = Clock::now();

  public:
    explicit StartupPhaseTimer(void (StartupMetrics::*record_fn)(double)) : record{record_fn} {}
    ~StartupPhaseTimer();
};

} // namespace Pelican
