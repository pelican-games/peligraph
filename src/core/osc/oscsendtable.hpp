#pragma once

#include "oscencoder.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace Pelican::Osc {

using Clock = std::chrono::steady_clock;
inline constexpr std::size_t send_table_capacity = 256;
inline constexpr std::chrono::seconds send_resend_interval{1};

struct ApplyResult {
    std::size_t changed = 0, dropped = 0, non_finite = 0, out_of_range = 0;
};
struct Outgoing {
    std::string address;
    SendValue value;
};
struct Work {
    std::vector<Outgoing> changes, resends;
};

class SendTable {
  public:
    explicit SendTable(Clock::time_point start,
                       std::size_t capacity = send_table_capacity);
    ApplyResult applyFrame(std::span<const ValueGraph::OscSendValue> items);
    void clear();
    Work take(Clock::time_point now);
    bool hasDirty() const;
    Clock::time_point nextResend() const;
    std::size_t size() const;

  private:
    struct Entry {
        SendValue value;
        bool dirty = false;
        bool suspended = false;
        std::uint64_t stamp = 0;
    };
    std::map<std::string, Entry, std::less<>> entries_;
    std::size_t capacity_;
    std::uint64_t stamp_ = 0;
    Clock::time_point next_resend_;
};

} // namespace Pelican::Osc
