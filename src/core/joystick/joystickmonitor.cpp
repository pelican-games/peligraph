#include "joystickmonitor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {
using json = nlohmann::json;

void requireSlot(std::size_t slot) {
    if (slot >= joystick_slot_count) throw std::invalid_argument("slot outside 0..15");
}
} // namespace

JoystickMonitor::JoystickMonitor(JoystickSource *source)
    : source_(source), owner_thread_(std::this_thread::get_id()),
      published_(std::make_shared<const JoystickSnapshot>()) {}

bool JoystickMonitor::enabled() const noexcept { return source_ != nullptr; }
const char *JoystickMonitor::backend() const noexcept {
    return source_ == nullptr ? "none" : source_->backend();
}

bool JoystickMonitor::leaseActiveLocked(JoystickClock::time_point now) const {
    return now < last_renewal_ + joystick_lease_duration;
}
bool JoystickMonitor::leaseActive(JoystickClock::time_point now) const {
    std::scoped_lock lock{mutex_};
    return leaseActiveLocked(now);
}

void JoystickMonitor::renewLease(JoystickClock::time_point now) {
    std::scoped_lock lock{mutex_};
    if (!leaseActiveLocked(now)) {
        ++lease_epoch_;
        auto fresh = std::make_shared<JoystickSnapshot>(*published_);
        for (auto &device : fresh->devices) {
            device.has_values = false;
            device.values = {};
        }
        published_ = std::move(fresh);
    }
    last_renewal_ = std::max(last_renewal_, now);
}

void JoystickMonitor::publish(bool monitoring, std::uint64_t lease_epoch) {
    auto next = std::make_shared<JoystickSnapshot>();
    next->generation = generation_;
    next->sample_serial = sample_serial_;
    for (const auto &slot : slots_) {
        if (!slot) continue;
        JoystickDevice device = slot->device;
        device.has_values = monitoring && slot->sample_valid && slot->sample_epoch == lease_epoch;
        if (!device.has_values) device.values = {};
        next->devices.push_back(std::move(device));
    }
    std::ranges::sort(next->devices, {}, &JoystickDevice::instance_id);
    std::scoped_lock lock{mutex_};
    published_ = std::move(next);
    ++publish_count_;
}

void JoystickMonitor::pumpFrame(JoystickClock::time_point now) {
    if (source_ == nullptr) return;
    if (std::this_thread::get_id() != owner_thread_) {
        throw std::logic_error("joystick.wrong_thread@1: pumpFrame must run on the monitor owner thread");
    }
    bool monitoring;
    std::uint64_t lease_epoch;
    {
        std::scoped_lock lock{mutex_};
        monitoring = leaseActiveLocked(now);
        lease_epoch = lease_epoch_;
    }
    if (!started_ && !monitoring) return;

    bool changed = false;
    const auto limits = source_->limits();
    auto connect = [&](std::size_t slot) {
        if (slot >= joystick_slot_count) return;
        if (slots_[slot]) {
            slots_[slot].reset();
            ++generation_;
            changed = true;
        }
        JoystickIdentity identity;
        if (!source_->identify(slot, identity)) return;
        identity.name = joystickSafeUtf8(identity.name);
        ++generation_;
        Slot entry;
        entry.device.instance_id = next_instance_id_++;
        entry.device.generation = generation_;
        entry.device.identity = std::move(identity);
        slots_[slot] = std::move(entry);
        changed = true;
    };
    auto disconnect = [&](std::size_t slot) {
        if (slot >= joystick_slot_count || !slots_[slot]) return;
        slots_[slot].reset();
        ++generation_;
        changed = true;
    };

    if (!started_) {
        source_->startWatching();
        for (std::size_t slot = 0; slot < joystick_slot_count; ++slot) {
            if (source_->present(slot)) connect(slot);
        }
        started_ = true;
    }
    for (const auto &change : source_->drainChanges()) {
        if (change.connected) connect(change.slot);
        else disconnect(change.slot);
    }

    if (monitoring) {
        for (auto &slot : slots_) {
            if (!slot) continue;
            JoystickSample sample;
            if (!source_->sample(static_cast<std::size_t>(&slot - slots_.data()), sample)) {
                slot->sample_valid = false;
                slot->device.has_values = false;
                slot->device.values = {};
                changed = true;
                continue;
            }
            for (auto &axis : sample.axes) {
                if (!std::isfinite(axis)) axis = 0.0f;
            }
            slot->device.values = std::move(sample);
            slot->device.axes = slot->device.values.axes.size();
            slot->device.buttons = slot->device.values.buttons.size();
            slot->device.hats = slot->device.values.hats.size();
            slot->device.sampled = true;
            slot->device.has_values = true;
            slot->device.maybe_truncated = limits.has_value() &&
                (slot->device.axes >= limits->axes || slot->device.buttons >= limits->buttons ||
                 slot->device.hats >= limits->hats);
            slot->sample_epoch = lease_epoch;
            slot->sample_valid = true;
            changed = true;
        }
        ++sample_serial_;
    } else if (was_monitoring_) {
        for (auto &slot : slots_) {
            if (!slot) continue;
            slot->device.has_values = false;
            slot->device.values = {};
        }
        changed = true;
    }
    was_monitoring_ = monitoring;
    if (changed || monitoring) publish(monitoring, lease_epoch);
}

std::shared_ptr<const JoystickSnapshot> JoystickMonitor::snapshot() const {
    std::scoped_lock lock{mutex_};
    return published_;
}
std::uint64_t JoystickMonitor::publishCount() const {
    std::scoped_lock lock{mutex_};
    return publish_count_;
}

std::size_t joystickRealButtonCount(int reported_buttons, int hats) noexcept {
    if (reported_buttons <= 0) return 0;
    const auto synthetic = std::max(0, hats) * 4;
    return static_cast<std::size_t>(std::max(0, reported_buttons - synthetic));
}

std::string joystickSafeUtf8(const std::string &text) {
    return json::parse(json(text).dump(-1, ' ', false, json::error_handler_t::replace))
        .get<std::string>();
}

nlohmann::json joystickDevicesJson(const JoystickMonitor &monitor, JoystickClock::time_point now,
                                   bool with_values, const std::string &engine_instance_id) {
    const auto current = monitor.snapshot();
    const bool monitoring = monitor.enabled() && monitor.leaseActive(now);
    auto devices = json::array();
    if (monitor.enabled()) {
        for (const auto &device : current->devices) {
            json entry{
                {"instance_id", device.instance_id},
                {"generation", device.generation},
                {"name", device.identity.name},
                {"guid", device.identity.guid},
                {"is_gamepad", device.identity.is_gamepad},
                {"sampled", device.sampled},
                {"axes", device.axes},
                {"buttons", device.buttons},
                {"hats", device.hats},
                {"maybe_truncated", device.maybe_truncated},
            };
            if (with_values && monitoring && device.has_values) {
                entry["values"] = {{"axes", device.values.axes},
                                   {"buttons", device.values.buttons},
                                   {"hats", device.values.hats}};
            }
            devices.push_back(std::move(entry));
        }
    }
    return {
        {"enabled", monitor.enabled()},
        {"backend", monitor.backend()},
        {"source_kind", "joy"},
        {"engine_instance_id", engine_instance_id},
        {"monitoring", monitoring},
        {"lease_ms", joystick_lease_duration.count()},
        {"generation", monitor.enabled() ? current->generation : 0},
        {"sample_serial", monitor.enabled() ? current->sample_serial : 0},
        {"devices", std::move(devices)},
    };
}

void InjectedJoystickSource::connect(std::size_t slot, JoystickIdentity identity, JoystickSample state) {
    requireSlot(slot);
    std::scoped_lock lock{mutex_};
    if (devices_[slot]) throw std::invalid_argument("connect on a busy slot");
    devices_[slot] = Device{std::move(identity), std::move(state)};
    pending_.push_back({slot, true});
}
void InjectedJoystickSource::set(std::size_t slot, JoystickSample state) {
    requireSlot(slot);
    std::scoped_lock lock{mutex_};
    if (!devices_[slot]) throw std::invalid_argument("set on an empty slot");
    devices_[slot]->state = std::move(state);
}
void InjectedJoystickSource::disconnect(std::size_t slot) {
    requireSlot(slot);
    std::scoped_lock lock{mutex_};
    if (!devices_[slot]) throw std::invalid_argument("disconnect on an empty slot");
    devices_[slot].reset();
    pending_.push_back({slot, false});
}
const char *InjectedJoystickSource::backend() const { return "injected"; }
std::optional<JoystickLimits> InjectedJoystickSource::limits() const { return std::nullopt; }
void InjectedJoystickSource::startWatching() {
    std::scoped_lock lock{mutex_};
    pending_.clear();
}
std::vector<JoystickConnectionChange> InjectedJoystickSource::drainChanges() {
    std::scoped_lock lock{mutex_};
    return std::exchange(pending_, {});
}
bool InjectedJoystickSource::present(std::size_t slot) {
    requireSlot(slot);
    std::scoped_lock lock{mutex_};
    return devices_[slot].has_value();
}
bool InjectedJoystickSource::identify(std::size_t slot, JoystickIdentity &out) {
    requireSlot(slot);
    std::scoped_lock lock{mutex_};
    if (!devices_[slot]) return false;
    out = devices_[slot]->identity;
    return true;
}
bool InjectedJoystickSource::sample(std::size_t slot, JoystickSample &out) {
    requireSlot(slot);
    std::scoped_lock lock{mutex_};
    if (!devices_[slot]) return false;
    out = devices_[slot]->state;
    return true;
}
} // namespace Pelican
