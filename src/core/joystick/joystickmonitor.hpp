#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Pelican {
inline constexpr std::size_t joystick_slot_count = 16;
inline constexpr std::chrono::milliseconds joystick_lease_duration{2000};
using JoystickClock = std::chrono::steady_clock;

struct JoystickIdentity {
    std::string name;
    std::string guid;
    bool is_gamepad = false;
};
struct JoystickSample {
    std::vector<float> axes;
    std::vector<std::uint8_t> buttons;
    std::vector<std::uint8_t> hats;
};
struct JoystickLimits {
    std::size_t axes = 0;
    std::size_t buttons = 0;
    std::size_t hats = 0;
};
struct JoystickConnectionChange {
    std::size_t slot = 0;
    bool connected = false;
};

class JoystickSource {
  public:
    virtual ~JoystickSource() = default;
    virtual const char *backend() const = 0;
    virtual std::optional<JoystickLimits> limits() const = 0;
    virtual void startWatching() = 0;
    virtual std::vector<JoystickConnectionChange> drainChanges() = 0;
    virtual bool present(std::size_t slot) = 0;
    virtual bool identify(std::size_t slot, JoystickIdentity &out) = 0;
    virtual bool sample(std::size_t slot, JoystickSample &out) = 0;
};

struct JoystickDevice {
    std::uint64_t instance_id = 0;
    std::uint64_t generation = 0;
    JoystickIdentity identity;
    bool sampled = false;
    bool has_values = false;
    bool maybe_truncated = false;
    std::size_t axes = 0, buttons = 0, hats = 0;
    JoystickSample values;
};
struct JoystickSnapshot {
    std::uint64_t generation = 0;
    std::uint64_t sample_serial = 0;
    std::vector<JoystickDevice> devices;
};

class JoystickMonitor {
  public:
    explicit JoystickMonitor(JoystickSource *source);
    bool enabled() const noexcept;
    const char *backend() const noexcept;
    void renewLease(JoystickClock::time_point now);
    bool leaseActive(JoystickClock::time_point now) const;
    void pumpFrame(JoystickClock::time_point now);
    std::shared_ptr<const JoystickSnapshot> snapshot() const;
    std::uint64_t publishCount() const;

  private:
    struct Slot {
        JoystickDevice device;
        std::uint64_t sample_epoch = 0;
        bool sample_valid = false;
    };
    bool leaseActiveLocked(JoystickClock::time_point now) const;
    void publish(bool monitoring, std::uint64_t lease_epoch);

    JoystickSource *source_;
    const std::thread::id owner_thread_;
    mutable std::mutex mutex_;
    JoystickClock::time_point last_renewal_{};
    std::uint64_t lease_epoch_ = 0;
    std::shared_ptr<const JoystickSnapshot> published_;
    std::uint64_t publish_count_ = 0;
    std::array<std::optional<Slot>, joystick_slot_count> slots_;
    std::uint64_t generation_ = 0;
    std::uint64_t sample_serial_ = 0;
    std::uint64_t next_instance_id_ = 1;
    bool started_ = false;
    bool was_monitoring_ = false;
};

std::size_t joystickRealButtonCount(int reported_buttons, int hats) noexcept;
std::string joystickSafeUtf8(const std::string &text);
nlohmann::json joystickDevicesJson(const JoystickMonitor &monitor, JoystickClock::time_point now,
                                   bool with_values, const std::string &engine_instance_id);

class InjectedJoystickSource final : public JoystickSource {
  public:
    void connect(std::size_t slot, JoystickIdentity identity, JoystickSample state);
    void set(std::size_t slot, JoystickSample state);
    void disconnect(std::size_t slot);
    const char *backend() const override;
    std::optional<JoystickLimits> limits() const override;
    void startWatching() override;
    std::vector<JoystickConnectionChange> drainChanges() override;
    bool present(std::size_t slot) override;
    bool identify(std::size_t slot, JoystickIdentity &out) override;
    bool sample(std::size_t slot, JoystickSample &out) override;

  private:
    struct Device {
        JoystickIdentity identity;
        JoystickSample state;
    };
    mutable std::mutex mutex_;
    std::array<std::optional<Device>, joystick_slot_count> devices_;
    std::vector<JoystickConnectionChange> pending_;
};
} // namespace Pelican
