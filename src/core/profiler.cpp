#include "profiler.hpp"
#ifndef PELICAN_NO_LOG
#include "log.hpp"
#endif
#include <iostream>

namespace Pelican {

Profiler& Profiler::Get() {
    static Profiler instance;
    return instance;
}

void Profiler::Start(const char* zone_name) {
    start_times[zone_name] = std::chrono::high_resolution_clock::now();
}

void Profiler::End(const char* zone_name) {
    auto it = start_times.find(zone_name);
    if (it != start_times.end()) {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - it->second).count();
        
#ifndef PELICAN_NO_LOG
        if (logger) {
            LOG_INFO(logger, "Profiler: {} took {} us", zone_name, duration);
        } else {
            std::cout << "Profiler: " << zone_name << " took " << duration << " us" << std::endl;
        }
#else
        std::cout << "Profiler: " << zone_name << " took " << duration << " us" << std::endl;
#endif
        
        start_times.erase(it);
    } else {
#ifndef PELICAN_NO_LOG
        if (logger) {
            LOG_WARNING(logger, "Profiler: End called for {} without Start", zone_name);
        } else {
            std::cout << "Profiler: End called for " << zone_name << " without Start" << std::endl;
        }
#else
        std::cout << "Profiler: End called for " << zone_name << " without Start" << std::endl;
#endif
    }
}

void TimeProfilerStart(const char* zone_name) {
    Profiler::Get().Start(zone_name);
}

void TimeProfilerEnd(const char* zone_name) {
    Profiler::Get().End(zone_name);
}

} // namespace Pelican
