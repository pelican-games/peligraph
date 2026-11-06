#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#if PELICAN_OUTPUT_TEST_SEAMS
#include <atomic>
#include <nlohmann/json.hpp>
#endif

namespace Pelican {

// VO-4: what the output.spout sink sends.
//   Windows only. DXGI_FORMAT_B8G8R8A8_UNORM, tightly packed, row 0 on top.
//   Bytes are the source's own: display-encoded SDR with straight alpha is
//   assumed, as in output.video. Linear data is not converted to display
//   encoding and alpha is not premultiplied; an RGBA8 source only has red and
//   blue swapped.
//   frames_written counts frames SendImage accepted. It isn't proof a frame
//   reached the shared texture or a receiver: SendImage returns true when a
//   receiver held the texture mutex past its 67 ms wait, and no pixels were
//   copied. A receiver that holds it causes delay or skipped updates and no
//   error. There is no receiver count.
//   The sender and the receiver must be on compatible graphics adapters; a
//   sender name being available doesn't guarantee reception on a hybrid-GPU
//   system. Adapter selection isn't part of this path.
//   Name ownership isn't atomic against other processes: Spout's API lets
//   another program take the name between the check and the first send. This
//   process's own senders are serialized.

inline constexpr auto output_spout_failure_limit = std::chrono::seconds(5);

// VO-5b: the GPU path. Vulkan copies the source into a D3D11 texture imported
// into Vulkan memory, and the sender's thread hands it to spoutDX::SendTexture,
// which copies it again into Spout's own shared texture.
//
// Synchronization: Vulkan's capture submit only signals an imported timeline
// semaphore and has no wait semaphore, so nothing D3D11 or a receiver does can
// stall the graphics queue. D3D11 waits for that value on its context, sends,
// then signals a local fence, and the bridge is free only once that fence has
// passed. A timed-out wait cancels nothing: the bridge stays quarantined until
// it finishes or the sink gives up its device. GPU allocations made on the
// sender's thread can still cause driver-level contention with rendering.
inline constexpr std::size_t output_spout_bridge_count = 2;
inline constexpr auto output_spout_sync_limit = std::chrono::seconds(1);
// How many more limits a timed-out D3D11 operation gets to finish.
inline constexpr int output_spout_quarantine_tries = 5;
inline constexpr auto output_spout_gpu_clear_after = std::chrono::seconds(60);
inline constexpr std::array<std::chrono::seconds, 3> output_spout_retry_after{
    std::chrono::seconds(30), std::chrono::seconds(120), std::chrono::seconds(480)};

enum class OutputSpoutSourceFormat : std::uint8_t {
    bgra8_unorm,
    bgra8_srgb,
    rgba8_unorm,
    rgba8_srgb,
};

enum class OutputSpoutGpuFailure : std::uint8_t {
    none,
    // Unsupported, or an invalid argument: trying again can't help.
    permanent,
    // Exhaustion or a passing fault: tried again later.
    transient,
    // This extent can't be sent this way. Another one may be.
    this_extent,
    device_removed,
};

// By the value that was returned, never by the stage that returned it.
OutputSpoutGpuFailure outputSpoutClassifyVkResult(std::int32_t result);
OutputSpoutGpuFailure outputSpoutClassifyHresult(std::int32_t result);

struct OutputSpoutGpuStep {
    OutputSpoutGpuFailure failure = OutputSpoutGpuFailure::none;
    std::string reason;
    std::string detail;
};

// What the sender may do, decided before it starts. Its Vulkan device outlives it.
struct OutputSpoutGpuRequest {
    VkDevice device = VK_NULL_HANDLE;
    std::array<std::uint8_t, 8> vulkan_luid{};
    bool allowed = false;
    // Why not, when it isn't allowed.
    std::string gate_reason = "preflight_unavailable";
    std::string gate_detail;
    std::uint32_t max_width = 0;
    std::uint32_t max_height = 0;
    // A blit from R8G8B8A8_UNORM to the bridge kept every byte on this device,
    // checked at startup: vkCmdBlitImage is a numeric conversion, not a byte swap.
    bool rgba8_blit_exact = false;
};


enum class OutputSpoutResultKind : std::uint8_t {
    ok,
    // An observable API failure that may pass: counts toward the 5-second rule.
    failed,
    // No device, context, shared texture or registration: terminal.
    open_failed,
    // ID3D11Device::GetDeviceRemovedReason isn't S_OK: terminal.
    device_removed,
};

struct OutputSpoutResult {
    OutputSpoutResultKind kind = OutputSpoutResultKind::ok;
    std::string detail;
};

struct OutputSpoutSentBridge {
    OutputSpoutResult result;
    // A D3D11 wait is queued, so the bridge isn't free until waitSent says so.
    bool completion_enqueued = false;
};

enum class OutputSpoutSentState : std::uint8_t { done, timeout, device_removed, failed };

// The GPU side of a backend, on the sender's own thread only.
class OutputSpoutGpuBackend {
  public:
    virtual ~OutputSpoutGpuBackend() = default;
    // The actual device's adapter, the fences, the imported semaphore.
    virtual OutputSpoutGpuStep prepare(const OutputSpoutGpuRequest &request,
                                       VkSemaphore &semaphore) = 0;
    // Nothing of a failed attempt stays alive.
    virtual OutputSpoutGpuStep createBridges(
        std::uint32_t width, std::uint32_t height,
        std::array<VkImage, output_spout_bridge_count> &images) = 0;
    // Wait for wait_value, SendTexture, signal completion, flush.
    virtual OutputSpoutSentBridge sendBridge(std::size_t bridge, std::uint64_t wait_value) = 0;
    // Whether the last sendBridge has finished on the GPU.
    virtual OutputSpoutSentState waitSent(std::chrono::milliseconds limit) = 0;
    // Vulkan image, Vulkan memory, D3D11 texture. Only with no bridge in use.
    virtual void destroyBridges() = 0;
    virtual void releaseGpu() = 0;
    // A D3D11 operation will never be known to finish. Vulkan's objects go, since
    // all Vulkan work on them has finished. The D3D11 device and what depends on
    // it are kept for the process's life, and close() then does nothing.
    virtual void abandonGpu() = 0;
};

// The boundary for tests. Every call, the constructor and the destructor are on
// the sender's own thread. `send` gets tightly packed top-first BGRA8.
class OutputSpoutBackend {
  public:
    virtual ~OutputSpoutBackend() = default;
    // Null when this backend can only send pixels.
    virtual OutputSpoutGpuBackend *gpu() { return nullptr; }
    virtual OutputSpoutResult open(const std::string &name) = 0;
    virtual OutputSpoutResult send(const std::uint8_t *bgra, std::uint32_t width,
                                   std::uint32_t height) = 0;
    // The name the sender would publish under. Read right after open.
    virtual std::string actualName() const = 0;
    virtual void close() = 0;
};

using OutputSpoutBackendFactory =
    std::function<std::unique_ptr<OutputSpoutBackend>()>;

// Authored-disabled or no --output-spout: inactive, not_enabled. An offline
// run: inactive, offline. Otherwise, no Spout in this build or platform:
// refused, output.spout_unavailable@1. Only then may a backend open.
struct OutputSpoutActivation {
    bool start = false;
    std::string state;
    std::string stop_reason = "none";
    std::string error;
};
OutputSpoutActivation outputSpoutActivation(bool enabled, bool armed,
                                            bool offline, bool available);

bool outputSpoutAvailable();
// Disables Spout's process-global logging once, so call it before any sender
// or receiver thread starts. Empty when outputSpoutAvailable() is false.
OutputSpoutBackendFactory outputSpoutBackendFactory(OutputSpoutGpuRequest request = {});

// Swaps red and blue in place. Nothing else about the bytes changes.
void outputSpoutSwapRedBlue(std::uint8_t *pixels, std::size_t bytes);

// Set once a sender's thread has released its name and closed its device, or
// has found it never will. A new sender with the same name waits on this.
class OutputSpoutRelease {
  public:
    OutputSpoutRelease();
    void acknowledge();
    void wait() const;
    bool done() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

struct OutputSpoutTiming {
    std::uint64_t count = 0;
    double total_ms = 0.0;
    double max_ms = 0.0;
};

struct OutputSpoutStatus {
    std::string state = "running";
    std::string stop_reason = "none";
    std::string last_error;
    std::uint64_t frames_written = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t frames_discarded = 0;
    // True only after a send verified the sender is published; false again
    // after the release.
    bool registered = false;
    std::string sender_name;
    // The last extent Spout accepted.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t mailbox_depth = 0;
    std::size_t peak_mailbox_depth = 0;
    // A frame is out of the mailbox and its send hasn't returned.
    bool sending = false;
    std::size_t live_buffers = 0;
    std::size_t peak_live_buffers = 0;
    // VO-5b. frames_written is frames_gpu + frames_cpu, API submissions in both.
    std::string transport = "cpu";
    std::string transport_reason = "starting";
    std::string transport_detail;
    std::uint64_t frames_gpu = 0;
    std::uint64_t frames_cpu = 0;
    std::uint64_t bridge_busy = 0;
    std::uint64_t gpu_fallbacks = 0;
    std::uint64_t sync_timeouts = 0;
    // A timed-out bridge whose D3D11 work isn't known to have finished.
    bool gpu_quarantined = false;
    std::optional<std::uint64_t> gpu_next_retry_ms;
    // Sender-side API latency and D3D11 completion, not receiver latency.
    OutputSpoutTiming capture_to_send_gpu;
    OutputSpoutTiming capture_to_send_cpu;
    OutputSpoutTiming d3d11_completion;
    OutputSpoutTiming bridge_cycle;
};

// Storage from the sender's pool. The worker fills `pixels` and publishes it.
struct OutputSpoutFrame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool bgra = false;
    std::chrono::steady_clock::time_point captured_at{};
};

// A reserved bridge, from reserveGpu until completeGpu or cancelGpu.
struct OutputSpoutGpuFrame {
    std::size_t bridge = 0;
    VkImage image = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    std::uint64_t signal_value = 0;
    std::uint64_t bridge_generation = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool blit = false;
    std::chrono::steady_clock::time_point captured_at{};
};

enum class OutputSpoutRoute : std::uint8_t { cpu, gpu, busy };

struct OutputSpoutReservation {
    OutputSpoutRoute route = OutputSpoutRoute::cpu;
    std::optional<OutputSpoutGpuFrame> frame;
};

// One sink's Spout sender, with its own thread, which is the only one that
// touches the backend.
//
// At most two pixel buffers exist per sender, allocated on first need and kept:
//   - in the free list, owned by the sender until acquire();
//   - with the worker from acquire() to publish();
//   - in the mailbox, from publish() until the thread takes it or a newer
//     frame's acquire() reclaims it, which counts one drop;
//   - on the sender thread during the send, which returns it to the free list.
// So a busy send never holds a staging slot and never makes the worker wait.
// The buffer is reused for the RGBA to BGRA swap, in place on the sender thread.
class OutputSpoutSender {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;
    struct Options {
        std::string name;
        // Called on the sender's thread, after any predecessor has released.
        OutputSpoutBackendFactory factory;
        // The release of the last sender that had this name, if any.
        std::optional<OutputSpoutRelease> predecessor;
        // Defaults to steady_clock; a test injects its own.
        std::function<TimePoint()> clock;
        // Called on the sender's thread with no lock held, once, after the
        // sender refused and let go of Spout.
        std::function<void(const OutputSpoutSender &)> on_refused;
        OutputSpoutGpuRequest gpu;
    };

    explicit OutputSpoutSender(Options options);
    // Stops and joins, so it waits for a send in progress to return. Never
    // destroy a sender while holding a lock its callback takes.
    ~OutputSpoutSender();
    OutputSpoutSender(const OutputSpoutSender &) = delete;
    OutputSpoutSender &operator=(const OutputSpoutSender &) = delete;

    // Worker side. Neither waits on the backend. acquire returns nothing when
    // the sender stopped or refused, for an invalid size, or when both buffers
    // are out; the frame is then counted and goes no further.
    std::optional<OutputSpoutFrame> acquire(std::uint32_t width,
                                            std::uint32_t height, bool bgra);
    void publish(OutputSpoutFrame frame);

    // Render side. Takes only this sender's mutex and never calls the backend.
    // cpu: use acquire and publish. busy: the frame is dropped and counted. gpu:
    // every frame then reaches exactly one of the three calls below.
    OutputSpoutReservation reserveGpu(std::uint32_t width, std::uint32_t height,
                                      OutputSpoutSourceFormat format);
    // Its Vulkan fence has passed.
    void completeGpu(const OutputSpoutGpuFrame &frame);
    // Never submitted, or its fence has passed, or the device is lost.
    void cancelGpu(const OutputSpoutGpuFrame &frame);
    // vkQueueSubmit threw: nothing was submitted.
    void gpuSubmitFailed(const OutputSpoutGpuFrame &frame, std::int32_t vk_result,
                         std::string detail);

    // Never waits. The thread drops the unsent frame, releases the sender and
    // closes DirectX, in that order, once any send in progress returns.
    void requestStop();
    // A refusal met outside the sender (the source's format, a readback). It
    // stays until a reload makes a new sender.
    void refuse(std::string error);

    const std::string &name() const;
    bool finished() const;
    OutputSpoutRelease release() const;
    OutputSpoutStatus status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#if PELICAN_WITH_SPOUT
// Real spoutDX, implemented in outputspoutbackend_win.cpp, the only source that
// sees Windows or D3D11 headers.
OutputSpoutBackendFactory outputSpoutRealBackendFactory(OutputSpoutGpuRequest request = {});
#if PELICAN_OUTPUT_TEST_SEAMS
// As above, except that it doesn't create the D3D11 device: a real spoutDX
// whose device is missing, which no fake can stand in for.
std::unique_ptr<OutputSpoutBackend> makeOutputSpoutBackendWithoutDeviceForTesting();
#endif
#endif

#if PELICAN_OUTPUT_TEST_SEAMS
// With PELICAN_OUTPUT_SPOUT_TEST_BACKEND set in the environment, the player's
// sinks use a recording backend instead of Spout ("1", or "fail_open"). The
// call sets `block_send` and `block_close` when given, and returns what it saw.
nlohmann::json outputSpoutTestBackendControl(const nlohmann::json &params);
// The real backend's sendBridge waits here under the fault block_send.
std::atomic<bool> &outputSpoutGpuBlockSendForTesting();
std::atomic<bool> &outputSpoutGpuSendBlockedForTesting();
#endif

} // namespace Pelican
