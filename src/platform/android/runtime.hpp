#pragma once
#include <filesystem>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
struct ANativeWindow;
namespace Pelican::Android {
// Initialized by the platform worker before entering the player.
const std::filesystem::path &pipelineCacheDirectory();
// One process / Activity. The Android Window backend owns this slot; the
// pump and every surface factory serialize native-window access through it.
struct NativeWindowSlot {
    std::mutex mutex;
    std::condition_variable changed;
    ANativeWindow *window = nullptr;
    std::uint64_t revision = 0, release_revision = 0, acked_revision = 0;
    bool valid = false;
    std::map<std::uint64_t, unsigned> live, quarantined;
    std::atomic<bool> release_requested{false};
};
NativeWindowSlot &nativeWindowSlot();
bool waitForNativeWindow();
bool waitForNativeWindow(std::chrono::steady_clock::time_point deadline);
void acknowledgeSurfaceReleaseLocked(NativeWindowSlot &slot);
bool nativeRevisionCurrent(std::uint64_t revision);
bool closeRequested();
bool paused();
void requestStop();
bool preserveModulesForProcessExit();
void quarantineModulesForProcessExit();
// Registered and invoked only on the engine owner thread, including PAUSE.
void setLifecycleHandler(std::function<void()> handler);
void processLifecycle();
std::uint64_t acceptedPresentCount();
bool testInput();
bool testNoMaintenance();
bool testZeroAcquireTimeout();
unsigned testBuildDelayMilliseconds();
// CPU wall time for each complete interactive iteration, including pacing.
void recordFrameMilliseconds(double milliseconds);
void flushFrameTimings();
// Called after vkQueuePresentKHR accepts the first frame (not scanout time).
void recordAcceptedPresent();
}
