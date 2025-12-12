#include "job_system.hpp"
#include <iostream>

namespace Pelican {

JobSystem::~JobSystem() {
    cleanup();
}

void JobSystem::init(int thread_count) {
    if (!workers.empty()) return; // Already initialized

    if (thread_count <= 0) {
        thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 1;
        // Reserve one thread for main thread if possible, or just use all logical cores?
        // Usually, for job system, we want to convert main thread to worker or have N-1 workers.
        // For simplicity, let's create N-1 workers if N > 1.
        if (thread_count > 1) thread_count--;
    }

    for (int i = 0; i < thread_count; ++i) {
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    condition.wait(lock, [this] { return stop || !jobs.empty(); });
                    
                    if (stop && jobs.empty()) return;
                    
                    job = std::move(jobs.front());
                    jobs.pop();
                }
                
                try {
                    job();
                } catch (const std::exception& e) {
                   std::cerr << "JobSystem Exception: " << e.what() << std::endl;
                } catch (...) {
                   std::cerr << "JobSystem Unknown Exception" << std::endl;
                }
                
                active_jobs--;
                wait_condition.notify_all();
            }
        });
    }
}

void JobSystem::schedule(std::function<void()> job) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        jobs.push(std::move(job));
        active_jobs++;
    }
    condition.notify_one();
}

void JobSystem::wait() {
    std::unique_lock<std::mutex> lock(wait_mutex);
    wait_condition.wait(lock, [this] { 
        // We need to check if queue is empty AND active_jobs is 0.
        // Actually active_jobs incremented on push, decremented on finish.
        // So active_jobs == 0 means queue is empty AND no one is working.
        return active_jobs == 0; 
    });
}

void JobSystem::cleanup() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    workers.clear();
    stop = false;
}

} // namespace Pelican
