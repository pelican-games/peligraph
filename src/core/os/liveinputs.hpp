#pragma once

#include "../container.hpp"
#include "../userpublic/valuegraph.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
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

// Where a frozen value came from. `none` is the declared default; a pending
// write from any source replaces it at the next freeze, and the source change
// alone does not move changed_frame.
enum class LiveInputSource : std::uint8_t {
    none,
    rpc,
    osc,
    replay,
};

// "default", "rpc", "osc".
std::string_view liveInputSourceName(LiveInputSource source) noexcept;

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
    LiveInputSource source = LiveInputSource::none;
    std::uint64_t changed_frame = 0;
    bool clamped = false;
};

// One value a non-RPC producer (OSC) wants to stage. `name` points at storage
// the caller keeps alive for the call. Values are already in the input's
// domain (an int update is integral, a bool update is 0/1); stageBatch
// validates that again and rejects a value of the wrong shape.
struct LiveInputUpdate {
    std::string_view name;
    double value = 0.0;
};

struct LiveStageResult {
    std::size_t applied = 0;
    std::size_t replay = 0;
    std::size_t invalid = 0;
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

    // OSC-2a: stage a batch of already-decoded values from a non-RPC producer.
    // Checks replay and commits all accepted updates under one lock, in order,
    // so a whole OSC packet lands together and no RPC write falls between its
    // messages. Never throws for replay or an unknown name: those are counted.
    // A non-finite or wrongly typed value is counted invalid and not staged.
    LiveStageResult stageBatch(std::span<const LiveInputUpdate> updates,
                               LiveInputSource source);

    // The frame boundary. A live replay supplies complete post-clamp values;
    // an input-sequence replay alone keeps the existing reset-to-default rule.
    void freeze(bool input_sequence_replaying, std::uint64_t frame,
                const std::map<std::string, double, std::less<>> *live_replay = nullptr);
    void beginLiveReplay();
    void endLiveReplay();
    void setLiveRecording(bool active);

    ValueGraph::Inputs frozenInputs(bool *replaying = nullptr) const;
    std::vector<LiveInputSnapshot> snapshot() const;

    bool isReplaying() const;
    bool isLiveReplaying() const;
    bool isLiveRecording() const;
    std::uint64_t droppedDuringReplay() const;

    // Successful graph validations accumulate reads; failed validation restores
    // the previous set so another active graph can continue unaffected.
    void setGraphLiveReads(std::vector<std::string> names);
    void noteGraphReadsLive(std::string_view name);
    bool graphReadsLive() const;
    std::string firstLiveRead() const;
    std::vector<std::string> graphLiveReads() const;

  private:
    struct Runtime {
        LiveInputSpec spec;
        double value = 0.0;
        LiveInputSource source = LiveInputSource::none;
        std::uint64_t changed_frame = 0;
        bool clamped = false;
    };
    struct Pending {
        double value = 0.0;
        bool clamped = false;
        LiveInputSource source = LiveInputSource::none;
    };

    struct StagedWrite {
        std::size_t index = 0;
        double value = 0.0;
        bool clamped = false;
        LiveInputSource source = LiveInputSource::none;
    };

    // Validate and clamp one already-decoded value for the input. Returns false
    // when the value is not in the input's domain (non-finite, fractional int,
    // non-0/1 bool); true otherwise with `out` filled in. Shared by every
    // staging path so the commit rules cannot drift.
    static bool normalizeValue(const LiveInputSpec &spec, double value,
                               StagedWrite &out);

    void installDeclaration(std::vector<LiveInputSpec> specs);
    void resetFrozenLocked();

    mutable std::mutex mutex_;
    std::vector<LiveInputSpec> specs_;
    std::map<std::string, std::size_t, std::less<>> indices_;
    std::vector<Runtime> runtime_;
    std::map<std::string, Pending, std::less<>> pending_;
    bool replaying_ = false;
    bool live_replaying_ = false;
    bool live_recording_ = false;
    std::uint64_t dropped_during_replay_ = 0;
    std::vector<std::string> graph_live_reads_;
};

} // namespace Pelican
