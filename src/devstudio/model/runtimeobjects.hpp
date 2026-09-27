#pragma once

#include "playstatemodel.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace PelicanStudio {

// PM-3c (docs/design_play_mode.md 12.6). The objects the running game spawned,
// as the engine's get_runtime_objects lists them (RO-1), for the Outliner's
// "(runtime)" rows and the Inspector's read-only view of one. No Qt here.

// A spawned object's entity: the same {index, generation} the engine's
// runtime gizmo selection names.
struct RuntimeObjectEntity {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    bool operator==(const RuntimeObjectEntity &) const = default;
};

// One of Transform's two readings (local_trs or world_trs), as far as each
// of its fields is the shape the Transform component writes.
struct RuntimeObjectTrs {
    std::optional<std::array<double, 3>> pos;
    std::optional<std::array<double, 4>> rotation;
    std::optional<std::array<double, 3>> scale;

    bool operator==(const RuntimeObjectTrs &) const = default;
};

struct RuntimeObject {
    // Its name, or spawn:<boundary>.<index> when the spawn gave none.
    std::string name;
    RuntimeObjectEntity entity;
    // The prefab the spawn named; empty when the engine did not say.
    std::string prefab;
    // Its runtime Transform; empty when it has none.
    std::optional<RuntimeObjectTrs> local;
    std::optional<RuntimeObjectTrs> world;

    bool operator==(const RuntimeObject &) const = default;
};

// A get_runtime_objects result. Empty when it is not the shape RO-1 writes; an
// entry without a name or an entity is left out, the rest of it is kept.
[[nodiscard]] std::optional<std::vector<RuntimeObject>>
parseRuntimeObjects(const nlohmann::json &result);

struct RuntimeObjectsRequest {
    std::uint64_t request_id = 0;
    std::string method;
    nlohmann::json params;
};

// What the Outliner shows of the spawned objects, and when it asks for them.
//
// The play state is read as the play controls read it (parsePlayStatus,
// PM-3b); only the running scene's id, which is get_status's and not the
// play state's, is read here. It asks when Studio reads the play state (any
// get_status reply) and that
// reading says the game started, stopped, paused, resumed or stepped; and,
// while the game plays, once a second (tick). It never asks in the edit state,
// where the list is empty, nor of an engine that does not know the method.
class RuntimeObjectsModel {
  public:
    // How often the list is read while the game plays. Paused, it moves only
    // with a step, and a step is a play reading of its own.
    static constexpr int PlayingReadIntervalMs = 1000;

    // The engine answers editor requests.
    void connect();
    // The engine went away or is being restarted: nothing it listed is live.
    void disconnect();

    // A get_status result, whoever asked for it. True when it carried a play
    // state this model reads.
    bool observeStatus(const nlohmann::json &status);
    // The playing clock. False, and nothing queued, when no read is wanted.
    bool tick();
    // Whether the playing clock should run.
    [[nodiscard]] bool tickWanted() const noexcept;

    [[nodiscard]] std::vector<RuntimeObjectsRequest> takeRequests();
    [[nodiscard]] bool owns(std::uint64_t request_id) const noexcept;
    void receiveResult(std::uint64_t request_id, const nlohmann::json &result);
    // The request was refused or never answered. `unknown_method` when the
    // engine does not have get_runtime_objects (JSON-RPC -32601): it is not
    // asked again until the next engine.
    void receiveFailure(std::uint64_t request_id, bool unknown_method);

    // Empty until a get_status carrying the play state was read.
    [[nodiscard]] const std::optional<PlayState> &state() const noexcept { return state_; }
    // The scene the engine runs, as get_status last named it; empty before.
    [[nodiscard]] const std::string &sceneId() const noexcept { return scene_id_; }
    [[nodiscard]] bool supported() const noexcept { return supported_; }
    [[nodiscard]] const std::vector<RuntimeObject> &objects() const noexcept {
        return objects_;
    }
    [[nodiscard]] const RuntimeObject *find(const RuntimeObjectEntity &entity) const noexcept;

    // The Outliner's choice of one of these rows. False when no such object
    // is listed. Cleared when the object leaves the list.
    bool select(const RuntimeObjectEntity &entity);
    void clearSelection();
    [[nodiscard]] const std::optional<RuntimeObjectEntity> &selected() const noexcept {
        return selected_;
    }
    [[nodiscard]] const RuntimeObject *selectedObject() const noexcept;

    // Moves whenever the list or the selection can read differently.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

  private:
    bool connected_ = false;
    bool supported_ = true;
    std::optional<PlayState> state_;
    std::optional<std::uint64_t> game_frame_;
    std::string scene_id_;
    std::vector<RuntimeObject> objects_;
    std::optional<RuntimeObjectEntity> selected_;
    std::uint64_t next_request_ = 1;
    std::vector<RuntimeObjectsRequest> outgoing_;
    std::uint64_t in_flight_ = 0;
    // Something asked for a read while one was in flight: its answer may be
    // older than what was asked about, so read once more after it.
    bool read_again_ = false;
    std::uint64_t revision_ = 1;

    [[nodiscard]] bool running() const noexcept {
        return state_ == PlayState::playing || state_ == PlayState::paused;
    }
    void read();
    void replace(std::vector<RuntimeObject> objects);
    void changed() noexcept { ++revision_; }
};

} // namespace PelicanStudio
