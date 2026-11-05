#pragma once

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace PelicanStudio {

class LiveInputsLearn {
  public:
    enum class State { idle, arming, listening, heard, timed_out };

    struct Candidate {
        std::string sender;
        std::string address;
        std::string arg_type;
        double value = 0.0;
        std::size_t count = 0;
        std::size_t distinct_values = 0;
        std::uint64_t latest_sequence = 0;
        bool confident = false;
    };

    void start(std::string input, std::string type, std::int64_t now);
    void start(std::string input, std::string type,
               std::uint64_t last_sequence, std::int64_t now);
    void arm(std::uint64_t last_sequence, std::int64_t now);
    void cancel();
    void receive(const nlohmann::json &recent, std::int64_t now);

    State state() const noexcept { return state_; }
    const std::string &input() const noexcept { return input_; }
    const std::vector<Candidate> &candidates() const noexcept { return candidates_; }
    bool gapWarning() const noexcept { return gap_warning_; }
    static constexpr std::int64_t timeout_ms = 15000;

  private:
    struct Observation {
        std::set<double> values;
        Candidate latest;
        bool constant_repeat = false;
    };

    void begin(std::uint64_t last_sequence);
    void updateState(std::int64_t now);
    bool usable(const nlohmann::json &entry, double &value) const;

    State state_ = State::idle;
    std::string input_;
    std::string type_;
    std::int64_t started_ = 0;
    std::uint64_t last_processed_ = 0;
    bool gap_warning_ = false;
    std::map<std::pair<std::string, std::string>, Observation> observations_;
    std::vector<Candidate> candidates_;
};

} // namespace PelicanStudio
