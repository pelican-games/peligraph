#include "liveinputslearn.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace PelicanStudio {

void LiveInputsLearn::start(std::string input, std::string type,
                            std::int64_t now) {
    input_ = std::move(input);
    type_ = std::move(type);
    state_ = State::arming;
    started_ = now;
    last_processed_ = 0;
    gap_warning_ = false;
    observations_.clear();
    candidates_.clear();
}

void LiveInputsLearn::start(std::string input, std::string type,
                            std::uint64_t last_sequence, std::int64_t now) {
    start(std::move(input), std::move(type), now);
    arm(last_sequence, now);
}

void LiveInputsLearn::begin(std::uint64_t last_sequence) {
    last_processed_ = last_sequence;
    state_ = State::listening;
}

void LiveInputsLearn::arm(std::uint64_t last_sequence, std::int64_t now) {
    if (state_ != State::arming) return;
    if (now - started_ >= timeout_ms) {
        state_ = State::timed_out;
        return;
    }
    begin(last_sequence);
}

void LiveInputsLearn::cancel() {
    state_ = State::idle;
    observations_.clear();
    candidates_.clear();
    gap_warning_ = false;
}

bool LiveInputsLearn::usable(const nlohmann::json &entry,
                             double &value) const {
    const auto type = entry.find("arg_type");
    const auto json_value = entry.find("value");
    if (type == entry.end() || !type->is_string() || json_value == entry.end() ||
        !json_value->is_number())
        return false;
    value = json_value->get<double>();
    if (!std::isfinite(value)) return false;
    const std::string tag = type->get<std::string>();
    if (type_ == "float") return tag == "f" || tag == "i";
    if (type_ == "int") {
        if (tag != "f" && tag != "i") return false;
        const double rounded = std::round(value);
        return rounded >= static_cast<double>(std::numeric_limits<std::int32_t>::min()) &&
               rounded <= static_cast<double>(std::numeric_limits<std::int32_t>::max());
    }
    if (type_ == "bool") {
        if (tag == "T") return value == 1.0;
        if (tag == "F") return value == 0.0;
        return tag == "i" && (value == 0.0 || value == 1.0);
    }
    return false;
}

void LiveInputsLearn::updateState(std::int64_t now) {
    if (state_ != State::listening && state_ != State::heard) return;
    if (now - started_ >= timeout_ms) {
        state_ = State::timed_out;
        return;
    }
    candidates_.clear();
    for (auto &[key, observation] : observations_) {
        auto candidate = observation.latest;
        candidate.distinct_values = observation.values.size();
        candidate.confident = candidate.distinct_values >= 2 ||
                              (type_ == "bool" && candidate.count >= 1);
        if (!candidate.confident && observation.constant_repeat) continue;
        candidates_.push_back(std::move(candidate));
    }
    std::stable_sort(candidates_.begin(), candidates_.end(),
                     [](const Candidate &left, const Candidate &right) {
                         if (left.distinct_values != right.distinct_values)
                             return left.distinct_values > right.distinct_values;
                         return left.latest_sequence > right.latest_sequence;
                     });
    state_ = candidates_.empty() ? State::listening : State::heard;
}

void LiveInputsLearn::receive(const nlohmann::json &recent, std::int64_t now) {
    if (state_ == State::arming) {
        if (now - started_ >= timeout_ms) state_ = State::timed_out;
        return;
    }
    if (state_ != State::listening && state_ != State::heard) return;
    if (!recent.is_array()) {
        updateState(now);
        return;
    }
    std::vector<const nlohmann::json *> ordered;
    for (const auto &entry : recent) {
        if (entry.is_object()) ordered.push_back(&entry);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto *left, const auto *right) {
        const auto a = left->value("sequence", std::uint64_t{0});
        const auto b = right->value("sequence", std::uint64_t{0});
        return a < b;
    });
    if (!ordered.empty()) {
        const auto oldest = ordered.front()->value("sequence", std::uint64_t{0});
        const auto newest = ordered.back()->value("sequence", std::uint64_t{0});
        if (newest < last_processed_) {
            cancel();
            return;
        }
        if (oldest > last_processed_ + 1) gap_warning_ = true;
    }
    for (const auto *entry : ordered) {
        const auto sequence = entry->value("sequence", std::uint64_t{0});
        if (sequence <= last_processed_) continue;
        last_processed_ = sequence;
        std::string address;
        std::string sender;
        if (const auto found = entry->find("address"); found != entry->end() && found->is_string())
            address = found->get<std::string>();
        if (const auto found = entry->find("sender"); found != entry->end() && found->is_string())
            sender = found->get<std::string>();
        double value = 0.0;
        if (address.empty() || !usable(*entry, value)) continue;
        auto &observation = observations_[{sender, address}];
        const bool existed = !observation.values.empty();
        observation.constant_repeat = existed && observation.values.size() == 1 &&
                                      observation.values.contains(value);
        observation.values.insert(value);
        Candidate candidate;
        candidate.sender = sender;
        candidate.address = address;
        candidate.arg_type = entry->value("arg_type", std::string{});
        candidate.value = value;
        candidate.count = observation.latest.count + 1;
        candidate.latest_sequence = sequence;
        observation.latest = std::move(candidate);
    }
    updateState(now);
}

} // namespace PelicanStudio
