#pragma once

#include "../container.hpp"
#include "../userpublic/valuegraph.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace Pelican {

// OSC-1a: the declared live inputs a graph may read. A project names them in
// basic_config.live_inputs_json; the declaration is fixed for the run. RPC
// writes a pending value, and the input boundary freezes it into the snapshot
// every graph evaluation of the frame reads. Nothing here talks to a socket:
// the transport is the existing JSON-RPC endpoint.
enum class LiveInputType : std::uint8_t {
    floating,
    integer,
    boolean,
};

std::string_view liveInputTypeName(LiveInputType type) noexcept;

struct LiveInputSpec {
    std::string name;
    LiveInputType type = LiveInputType::floating;
    double default_value = 0.0;
    std::optional<double> minimum;
    std::optional<double> maximum;
};

// One declared input as the frame froze it. `value` is in the declared type's
// domain: a finite double for float, an integer-valued double for int, 0/1 for
// bool.
struct LiveInputSnapshot {
    LiveInputSpec spec;
    double value = 0.0;
    bool from_rpc = false;
    std::uint64_t changed_frame = 0;
    bool clamped = false;
};

DECLARE_MODULE(LiveInputs) {
  public:
    LiveInputs();

    // Read basic_config.live_inputs_json again. An absent reference is an
    // empty declaration. An invalid document throws a named live_input.* error
    // and leaves the current declaration and frame state untouched.
    void reloadDeclaration();
    // The same, from memory. Used by tools/tests and by reloadDeclaration().
    void setDeclarationJson(std::string_view document);

    bool isDeclared(std::string_view name) const;
    std::optional<LiveInputSpec> declaration(std::string_view name) const;
    std::vector<LiveInputSpec> declarations() const;

    // Validate every value against the declaration before changing anything.
    // Returns how many values were accepted. On success they are pending
    // writes; freeze() applies them. Throws live_input.unknown@1, or
    // live_input.type@1 for a value of the wrong JSON type (including an int
    // outside int32). Out-of-range declared bounds are clamped and marked.
    std::size_t stageValues(const nlohmann::json &values);

    // The frame boundary. While replaying, pending writes are discarded and
    // the snapshot stays at the declared defaults; a replay start also drops
    // whatever was pending and resets the snapshot.
    void freeze(bool replaying, std::uint64_t frame);

    ValueGraph::Inputs frozenInputs() const;
    std::vector<LiveInputSnapshot> snapshot() const;

    bool isReplaying() const;
    std::uint64_t droppedDuringReplay() const;

    // The render graph validation records the first live input a graph reads,
    // so start_input_replay can refuse a project whose output depends on a
    // value a recording cannot carry.
    void noteGraphReadsLive(std::string_view name);
    bool graphReadsLive() const;
    std::string firstLiveRead() const;

  private:
    struct Runtime {
        LiveInputSpec spec;
        double value = 0.0;
        bool from_rpc = false;
        std::uint64_t changed_frame = 0;
        bool clamped = false;
    };
    struct Pending {
        double value = 0.0;
        bool clamped = false;
    };

    void installDeclaration(std::vector<LiveInputSpec> specs);
    void resetFrozenLocked();

    mutable std::mutex mutex_;
    std::vector<LiveInputSpec> specs_;
    std::map<std::string, std::size_t, std::less<>> indices_;
    std::vector<Runtime> runtime_;
    std::map<std::string, Pending, std::less<>> pending_;
    bool replaying_ = false;
    std::uint64_t dropped_during_replay_ = 0;
    bool graph_reads_live_ = false;
    std::string first_live_read_;
};

} // namespace Pelican
