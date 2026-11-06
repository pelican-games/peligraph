#include "oscsendtable.hpp"

#include "oscaddress.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace Pelican::Osc {
namespace {

struct Conversion {
    SendValue value;
    enum { ok, non_finite, out_of_range } result = ok;
};

Conversion convert(ValueGraph::OscSendValue item) {
    Conversion converted;
    converted.value.kind = item.kind;
    if (!std::isfinite(item.value)) {
        converted.result = Conversion::non_finite;
        return converted;
    }
    switch (item.kind) {
    case ValueGraph::OscSendKind::floating:
        if (std::abs(item.value) > std::numeric_limits<float>::max()) {
            converted.result = Conversion::out_of_range;
            return converted;
        }
        converted.value.bits =
            std::bit_cast<std::uint32_t>(static_cast<float>(item.value));
        break;
    case ValueGraph::OscSendKind::integer: {
        const double rounded = std::round(item.value);
        if (rounded < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
            rounded > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
            converted.result = Conversion::out_of_range;
            return converted;
        }
        converted.value.bits = std::bit_cast<std::uint32_t>(
            static_cast<std::int32_t>(rounded));
        break;
    }
    case ValueGraph::OscSendKind::boolean:
        converted.value.bits = item.value != 0.0 ? 1u : 0u;
        break;
    }
    return converted;
}

} // namespace

SendTable::SendTable(Clock::time_point start, std::size_t capacity)
    : capacity_(capacity), next_resend_(start + send_resend_interval) {}

ApplyResult SendTable::applyFrame(
    std::span<const ValueGraph::OscSendValue> items) {
    ApplyResult result;
    ++stamp_;
    for (const auto &item : items) {
        if (const auto found = entries_.find(item.address); found != entries_.end())
            found->second.stamp = stamp_;
    }
    for (auto current = entries_.begin(); current != entries_.end();) {
        if (current->second.stamp != stamp_) current = entries_.erase(current);
        else ++current;
    }
    for (const auto &item : items) {
        if (!isExactOscAddress(item.address)) {
            ++result.dropped;
            continue;
        }
        auto converted = convert(item);
        const auto found = entries_.find(item.address);
        if (converted.result != Conversion::ok) {
            if (converted.result == Conversion::non_finite) ++result.non_finite;
            else ++result.out_of_range;
            if (found != entries_.end()) {
                found->second.dirty = false;
                found->second.suspended = true;
            }
            continue;
        }
        if (found == entries_.end()) {
            if (entries_.size() >= capacity_) {
                ++result.dropped;
                continue;
            }
            entries_.emplace(std::string{item.address},
                             Entry{converted.value, true, false, stamp_});
            ++result.changed;
            continue;
        }
        auto &entry = found->second;
        if (entry.suspended || entry.value != converted.value) {
            entry.value = converted.value;
            entry.dirty = true;
            ++result.changed;
        }
        entry.suspended = false;
    }
    return result;
}

void SendTable::clear() {
    entries_.clear();
}

Work SendTable::take(Clock::time_point now) {
    Work work;
    for (auto &[address, entry] : entries_) {
        if (entry.dirty && !entry.suspended) {
            work.changes.push_back({address, entry.value});
            entry.dirty = false;
        }
    }
    if (now >= next_resend_) {
        for (const auto &[address, entry] : entries_) {
            if (entry.suspended || entry.dirty) continue;
            const bool changed = std::any_of(
                work.changes.begin(), work.changes.end(),
                [&](const Outgoing &outgoing) { return outgoing.address == address; });
            if (!changed) work.resends.push_back({address, entry.value});
        }
        next_resend_ = now + send_resend_interval;
    }
    return work;
}

bool SendTable::hasDirty() const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [](const auto &entry) {
                           return entry.second.dirty && !entry.second.suspended;
                       });
}

Clock::time_point SendTable::nextResend() const {
    return next_resend_;
}

std::size_t SendTable::size() const {
    return entries_.size();
}

} // namespace Pelican::Osc
