#pragma once

#include "osclistenerconfig.hpp"
#include "oscsendtable.hpp"

#include "../container.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include <nlohmann/json.hpp>

namespace Pelican {

class EngineLaunchConfig;

// Feedback is deliberately limited to on-change suppression of the encoded
// value. An unchanged value returned by a receiver causes no further `sent`;
// only the 1 Hz resend continues. A map, clamp, or smoother can change the
// value every frame and oscillate. There is no loop detection, source tagging,
// echo suppression, delivery acknowledgment, or output rate limit beyond one
// datagram per address per frame plus the resend. Do not bind an address to the
// input that feeds a sink on that same address.
struct OscSendOutcome {
    enum class Kind { sent, would_block, error } kind = Kind::sent;
    std::string error;
};
using OscSendTransport = std::function<OscSendOutcome(std::span<const std::uint8_t>)>;

class OscSender final : public ModuleBase<OscSender> {
  public:
    struct Impl;
    OscSender();
    ~OscSender();
    OscSender(const OscSender &) = delete;
    OscSender &operator=(const OscSender &) = delete;

    bool start(const OscSendConfig &config);
    void markOffline(const OscSendConfig &config);
    void stop();
    void publishFrame(std::span<const ValueGraph::OscSendValue> items,
                      bool replaying);

    // Replay acknowledgement waits for an in-flight transport call, never on
    // the render thread. Retirement invalidates taken batches without waiting.
    void pauseForReplay();
    void retire();

    [[nodiscard]] nlohmann::json statusJson() const;
    [[nodiscard]] bool socketOpen() const;
    [[nodiscard]] bool workerStarted() const;

  private:
    std::unique_ptr<Impl> impl_;

  public:
    bool startWithTransportForTesting(const OscSendConfig &config,
                                      OscSendTransport transport);
    std::unique_lock<std::mutex> lockTableForTesting();
};

bool oscSendOfflineLaunch(const EngineLaunchConfig &launch) noexcept;

} // namespace Pelican
