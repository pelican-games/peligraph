#pragma once

#include "../container.hpp"
#include "oscbindings.hpp"
#include "oscparser.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace Pelican {

// OSC-2a: maps OSC 1.0 datagrams onto declared live inputs. There is no
// socket here; a transport parses bytes elsewhere and hands them to receive().
// One mutex guards bindings, counters and the recent-message ring. The lock
// order is always OscInput then LiveInputs; receive() holds this lock across
// one stageBatch so a whole packet lands as one batch.
DECLARE_MODULE(OscInput) {
  public:
    OscInput();

    // Replace the address table atomically. A receive() in flight finishes
    // first. Bindings must already be valid; a duplicate address keeps the
    // last entry (the profile parser refuses duplicates before this point).
    void setBindings(const std::vector<Osc::OscBinding> &bindings);

    // Parse and apply one datagram. `now_ntp` is the caller's single 32.32 NTP
    // timestamp for the whole datagram. Never throws: malformed, unknown,
    // unsupported and replaying traffic is counted and recorded instead.
    void receive(std::span<const std::uint8_t> bytes, std::uint64_t now_ntp,
                 std::string_view sender = {});

    // The `osc` object of get_live_inputs, read as one consistent snapshot.
    nlohmann::json snapshotJson() const;
    std::vector<Osc::OscBinding> snapshotBindings() const;

  private:
    struct Binding {
        std::string input;
        LiveInputType input_type = LiveInputType::floating;
        bool has_map = false;
        Osc::OscBindingMap map;
    };

    struct Recent {
        std::uint64_t sequence = 0;
        std::string address;
        std::string input;
        std::string reason;
        std::string sender;
        std::string arg_type;
        double value = 0.0;
        bool has_value = false;
        bool bound = false;
    };

    struct Counters {
        std::uint64_t received = 0;
        std::uint64_t applied = 0;
        std::uint64_t unbound = 0;
        std::uint64_t malformed = 0;
        std::uint64_t limit = 0;
        std::uint64_t future_timetag = 0;
        std::uint64_t nested_bundle = 0;
        std::uint64_t unsupported_type = 0;
        std::uint64_t argument_count = 0;
        std::uint64_t non_finite = 0;
        std::uint64_t pattern_unsupported = 0;
        std::uint64_t out_of_range = 0;
        std::uint64_t replay = 0;
    };

    enum class Convert : std::uint8_t {
        ok,
        unsupported_type,
        argument_count,
        non_finite,
        out_of_range,
    };

    static Convert convert(const Binding &binding,
                           const Osc::Message &message, double &value);

    mutable std::mutex mutex_;
    std::vector<Binding> bindings_;
    std::unordered_map<std::string, std::size_t> lookup_;
    Counters counters_;
    std::deque<Recent> recent_;
    std::uint64_t next_sequence_ = 1;
};

// Current UTC time as 32.32 NTP fixed point (seconds since 1900 + fraction).
std::uint64_t ntpNowUtc() noexcept;

} // namespace Pelican
