#include "oscinput.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

constexpr std::size_t recent_capacity = 64;
constexpr std::uint64_t ntp_unix_offset_seconds = 2208988800ULL;

double applyMap(const Osc::OscBindingMap &map, double value) {
    const double t = (value - map.from[0]) / (map.from[1] - map.from[0]);
    return map.to[0] + t * (map.to[1] - map.to[0]);
}

} // namespace

std::uint64_t ntpNowUtc() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now);
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds)
            .count();
    const auto ntp_seconds =
        static_cast<std::uint64_t>(seconds.count()) + ntp_unix_offset_seconds;
    const auto fraction = (static_cast<std::uint64_t>(nanos) << 32) /
                          std::uint64_t{1000000000};
    return (ntp_seconds << 32) | fraction;
}

OscInput::OscInput() = default;

void OscInput::setBindings(const std::vector<Osc::OscBinding> &bindings) {
    std::vector<Binding> parsed;
    parsed.reserve(bindings.size());
    std::unordered_map<std::string, std::size_t> lookup;
    lookup.reserve(bindings.size());
    for (const auto &binding : bindings) {
        Binding entry;
        entry.input = binding.input;
        entry.input_type = binding.input_type;
        entry.has_map = binding.has_map;
        entry.map = binding.map;
        lookup[binding.address] = parsed.size();
        parsed.push_back(std::move(entry));
    }
    std::scoped_lock lock{mutex_};
    bindings_ = std::move(parsed);
    lookup_ = std::move(lookup);
}

OscInput::Convert OscInput::convert(const Binding &binding,
                                    const Osc::Message &message,
                                    double &value) {
    // OSC-2c: a live binding consumes exactly one argument.  OSC messages
    // with no argument or with several arguments are ambiguous, so the whole
    // message is dropped rather than silently reading the first one.
    if (message.arguments.size() != 1) return Convert::argument_count;
    const auto &argument = message.arguments.front();

    // Only the interoperable scalar tags apply.  The parser still validates
    // the payload structure of d/h/t/c/r/m (and s/b/N/I), but this engine has
    // no typed destination for them, so they are dropped as unsupported.
    switch (argument.tag) {
    case 'f':
        value = argument.number;
        break;
    case 'i':
        value = static_cast<double>(argument.integer);
        break;
    case 'T':
    case 'F':
        if (binding.input_type != LiveInputType::boolean)
            return Convert::unsupported_type;
        value = argument.boolean ? 1.0 : 0.0;
        return Convert::ok;
    default:
        return Convert::unsupported_type;
    }

    if (binding.input_type == LiveInputType::boolean) {
        // A bool accepts a numeric argument as non-zero true, but the profile
        // parser refuses a linear map on a bool input.
        if (!std::isfinite(value)) return Convert::non_finite;
        value = value != 0.0 ? 1.0 : 0.0;
        return Convert::ok;
    }

    if (binding.has_map) value = applyMap(binding.map, value);
    if (!std::isfinite(value)) return Convert::non_finite;

    if (binding.input_type == LiveInputType::integer) {
        const auto rounded = std::round(value);
        if (rounded < static_cast<double>(
                          std::numeric_limits<std::int32_t>::min()) ||
            rounded > static_cast<double>(
                          std::numeric_limits<std::int32_t>::max()))
            return Convert::out_of_range;
        value = rounded;
    }
    return Convert::ok;
}

void OscInput::receive(std::span<const std::uint8_t> bytes,
                       std::uint64_t now_ntp, std::string_view sender) {
    if (bytes.size() > Osc::max_packet_bytes) {
        std::scoped_lock lock{mutex_};
        ++counters_.received;
        ++counters_.limit;
        return;
    }
    try {
        // The whole parse is outside the lock; only resolving against the
        // current table and committing touches shared state.
        const auto packet = Osc::parsePacket(bytes, now_ntp);

        std::scoped_lock lock{mutex_};
        ++counters_.received;
        switch (packet.drop) {
        case Osc::PacketDrop::malformed:
            ++counters_.malformed;
            return;
        case Osc::PacketDrop::limit:
            ++counters_.limit;
            return;
        case Osc::PacketDrop::future_timetag:
            ++counters_.future_timetag;
            return;
        case Osc::PacketDrop::none:
            break;
        }

        struct Event {
            Recent recent;
            bool has_update = false;
            std::size_t update_index = 0;
        };
        std::vector<Event> events;
        events.reserve(packet.messages.size());
        std::vector<LiveInputUpdate> updates;
        updates.reserve(packet.messages.size());

        for (const auto &message : packet.messages) {
            Event event;
            event.recent.address = message.address;
            event.recent.sender = std::string{sender};
            if (message.arguments.size() == 1) {
                const auto &argument = message.arguments.front();
                switch (argument.tag) {
                case 'f':
                    if (std::isfinite(argument.number)) {
                        event.recent.arg_type = "f";
                        event.recent.value = argument.number;
                        event.recent.has_value = true;
                    }
                    break;
                case 'i':
                    event.recent.arg_type = "i";
                    event.recent.value = static_cast<double>(argument.integer);
                    event.recent.has_value = true;
                    break;
                case 'T':
                case 'F':
                    event.recent.arg_type = std::string(1, argument.tag);
                    event.recent.value = argument.boolean ? 1.0 : 0.0;
                    event.recent.has_value = true;
                    break;
                default:
                    break;
                }
            }
            if (message.reject == Osc::MessageReject::nested_bundle) {
                ++counters_.nested_bundle;
                event.recent.reason = "nested_bundle";
                events.push_back(std::move(event));
                continue;
            }
            if (message.reject == Osc::MessageReject::pattern_unsupported) {
                ++counters_.pattern_unsupported;
                event.recent.reason = "pattern_unsupported";
                events.push_back(std::move(event));
                continue;
            }
            const auto found = lookup_.find(message.address);
            if (found == lookup_.end()) {
                ++counters_.unbound;
                event.recent.reason = "unbound";
                events.push_back(std::move(event));
                continue;
            }
            const auto &binding = bindings_[found->second];
            event.recent.bound = true;
            event.recent.input = binding.input;

            if (message.reject == Osc::MessageReject::unsupported_type) {
                ++counters_.unsupported_type;
                event.recent.reason = "unsupported_type";
                events.push_back(std::move(event));
                continue;
            }

            double value = 0.0;
            switch (convert(binding, message, value)) {
            case Convert::ok:
                event.recent.reason.clear();
                event.has_update = true;
                event.update_index = updates.size();
                updates.push_back(LiveInputUpdate{binding.input, value});
                break;
            case Convert::argument_count:
                ++counters_.argument_count;
                event.recent.reason = "argument_count";
                break;
            case Convert::unsupported_type:
                ++counters_.unsupported_type;
                event.recent.reason = "unsupported_type";
                break;
            case Convert::non_finite:
                ++counters_.non_finite;
                event.recent.reason = "non_finite";
                break;
            case Convert::out_of_range:
                ++counters_.out_of_range;
                event.recent.reason = "out_of_range";
                break;
            }
            events.push_back(std::move(event));
        }

        if (!updates.empty()) {
            const auto staged =
                GET_MODULE(LiveInputs).stageBatch(updates, LiveInputSource::osc);
            counters_.applied += staged.applied;
            counters_.replay += staged.replay;
            if (staged.replay != 0) {
                for (auto &event : events) {
                    if (event.has_update) event.recent.reason = "replay";
                }
            }
            if (staged.invalid != 0) {
                // A declaration changed between profile application and this
                // packet. The batch refused the whole rejected part; record a
                // concrete per-message reason rather than hide it.
                counters_.out_of_range += staged.invalid;
                std::size_t remaining = staged.invalid;
                for (auto it = events.rbegin();
                     it != events.rend() && remaining != 0; ++it) {
                    if (!it->has_update) continue;
                    it->recent.reason = "out_of_range";
                    --remaining;
                }
            }
        }

        for (auto &event : events) {
            event.recent.sequence = next_sequence_++;
            recent_.push_back(std::move(event.recent));
            if (recent_.size() > recent_capacity) recent_.pop_front();
        }
    } catch (...) {
        // A non-engine thread must never see an OSC datagram raise into it.
    }
}

std::vector<Osc::OscBinding> OscInput::snapshotBindings() const {
    std::scoped_lock lock{mutex_};
    std::vector<Osc::OscBinding> result;
    result.reserve(bindings_.size());
    for (const auto &binding : bindings_) {
        result.push_back(Osc::OscBinding{
            .address = {},
            .input = binding.input,
            .input_type = binding.input_type,
            .has_map = binding.has_map,
            .map = binding.map,
        });
        result.back().address = std::string{};
        for (const auto &[address, index] : lookup_) {
            if (index == &binding - bindings_.data()) result.back().address = address;
        }
    }
    return result;
}

nlohmann::json OscInput::snapshotJson() const {
    std::scoped_lock lock{mutex_};
    nlohmann::json recent = nlohmann::json::array();
    for (const auto &event : recent_) {
        recent.push_back({
            {"sequence", event.sequence},
            {"address", event.address},
            {"input", event.input},
            {"bound", event.bound},
            {"reason", event.reason},
            {"sender", event.sender},
            {"arg_type", event.arg_type},
            {"value", event.has_value ? nlohmann::json(event.value)
                                      : nlohmann::json(nullptr)},
        });
    }
    return {
        {"bindings", bindings_.size()},
        {"last_sequence", next_sequence_ - 1},
        {"received", counters_.received},
        {"applied", counters_.applied},
        {"unbound", counters_.unbound},
        {"dropped",
         {
             {"malformed", counters_.malformed},
             {"limit", counters_.limit},
             {"future_timetag", counters_.future_timetag},
             {"nested_bundle", counters_.nested_bundle},
             {"unsupported_type", counters_.unsupported_type},
             {"argument_count", counters_.argument_count},
             {"non_finite", counters_.non_finite},
             {"pattern_unsupported", counters_.pattern_unsupported},
             {"out_of_range", counters_.out_of_range},
             {"replay", counters_.replay},
         }},
        {"recent", std::move(recent)},
    };
}

} // namespace Pelican
