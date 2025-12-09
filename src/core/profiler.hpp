#pragma once

#include <string>
#include <chrono>
#include <unordered_map>

namespace Pelican {

class Profiler {
public:
    static Profiler& Get();

    void Start(const char* zone_name);
    void End(const char* zone_name);

private:
    Profiler() = default;
    std::unordered_map<std::string, std::chrono::high_resolution_clock::time_point> start_times;
};

void TimeProfilerStart(const char* zone_name);
void TimeProfilerEnd(const char* zone_name);

} // namespace Pelican
