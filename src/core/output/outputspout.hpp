#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if PELICAN_OUTPUT_TEST_SEAMS
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

// The boundary for tests. Every call, the constructor and the destructor are on
// the sender's own thread. `send` gets tightly packed top-first BGRA8.
class OutputSpoutBackend {
  public:
    virtual ~OutputSpoutBackend() = default;
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
OutputSpoutBackendFactory outputSpoutBackendFactory();

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
};

// Storage from the sender's pool. The worker fills `pixels` and publishes it.
struct OutputSpoutFrame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool bgra = false;
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
OutputSpoutBackendFactory outputSpoutRealBackendFactory();
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
#endif

} // namespace Pelican
