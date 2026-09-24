#pragma once

#include "../container.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "resolvedscene.hpp"
#include "sceneload.hpp"
#include "../userpublic/object_ref.hpp"
#include <details/ecs/entity.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct SpawnParams;

// GP-2: objects game code makes and ends while a scene runs
// (GameContext::spawn / destroy).
//
// A request is checked and its prefab expanded when it is made -- through
// the ResolvedScene seam a scene load uses, so the prefab, its parameters
// and the build-feature gate answer the same way -- and queued. The queue is
// carried out at the frame boundary (applyBoundary), before any game code of
// the frame, in the order the requests were made, whether they came from a
// behavior callback or not.
//
// Everything is keyed by the scene run (SceneAuthority). A run counts its
// boundaries from 1; a spawn is named by (the boundary that materializes it,
// its index among that boundary's spawns), and its behaviors' attachment_seq
// is runtimeSpawnAttachmentSeq of that pair and the behavior's index. None of
// it is carried from one run to the next: a scene load retires the run's
// spawned objects with its other objects, and the first boundary of the new
// run starts the count again. Requests queued for a run that is no longer
// live -- or for a load candidate that failed -- are dropped at the boundary.
DECLARE_MODULE(RuntimeSpawner) {
  public:
    struct Stats {
        SceneAuthority run{};
        std::uint64_t boundary = 0;
        std::size_t queued = 0;
        std::size_t live = 0;
    };

  private:
    enum class RequestKind : std::uint8_t { spawn, destroy };

    struct Request {
        RequestKind kind = RequestKind::spawn;
        SceneAuthority authority{};
        // A spawn's own position; a destroy's target.
        std::uint64_t boundary = 0;
        std::uint32_t index = 0;
        std::optional<std::string> name;
        std::string identity_name;
        std::optional<ResolvedObject> object;
        std::vector<PreparedSceneBehaviorAttachment> behaviors;
    };

    struct LiveObject {
        SceneAuthority authority{};
        std::uint64_t boundary = 0;
        std::uint32_t index = 0;
        GameObjectId entity = invalidGameObjectId;
        std::optional<std::string> name;
    };

    SceneAuthority run_authority{};
    bool run_tracked = false;
    std::uint64_t boundary_count = 0;
    std::vector<Request> queue;
    std::vector<LiveObject> live;

    void materialize(Request &request);
    void retire(const Request &request);
    const LiveObject *findLive(SceneAuthority authority, std::uint64_t boundary,
                               std::uint32_t index) const noexcept;

  public:
    RuntimeSpawner() = default;

    TransientObjectRef spawn(std::string_view prefab, const SpawnParams &params);
    Resolved<TransientObjectRef> destroy(const TransientObjectRef &ref);
    Resolved<GameObjectId> materialize(const TransientObjectRef &ref) const;

    // The frame boundary. Called once per frame by updateFrameState, outside
    // every callback.
    void applyBoundary();

    Stats stats() const noexcept;
};

} // namespace Pelican
