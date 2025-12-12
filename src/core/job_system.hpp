#pragma once

#include <vector>
#include <thread>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace Pelican {

class JobSystem {
public:
    static JobSystem& Get() {
        static JobSystem instance;
        return instance;
    }

    // Initialize with thread count (0 = auto)
    void init(int thread_count = 0);
    
    // Schedule a job
    void schedule(std::function<void()> job);
    
    // Wait for all currently scheduled jobs to complete
    void wait();

    // Cleanup (join threads)
    void cleanup();

    ~JobSystem();

private:
    JobSystem() = default;

    std::vector<std::thread> workers;
    std::queue<std::function<void()>> jobs;
    std::mutex queue_mutex;
    std::condition_variable condition;
    
    bool stop = false;
    std::atomic<int> active_jobs{0};
    std::mutex wait_mutex;
    std::condition_variable wait_condition;
};

} // namespace Pelican
