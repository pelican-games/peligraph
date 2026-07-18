#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/behavior.hpp"
#include "../src/core/userpublic/details/system/registerer.hpp"
#include "../src/core/userpublic/deterministicrng.hpp"
#include "../src/core/userpublic/gameobjects.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {
namespace {

std::vector<std::string> trace;

struct DeterminismEvent {
    std::int32_t value = 0;
};

} // namespace

PELICAN_REGISTER_EVENT(DeterminismEvent);

namespace {

struct ProbeParams {
    std::int32_t id = 0;
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&ProbeParams::id>("id"), std::int32_t{0}));
};

class DeterminismBehavior final : public Behavior {
  public:
    using Params = ProbeParams;

    void onInit(BehaviorContext &ctx) override {
        trace.push_back("init:" + std::to_string(ctx.params<Params>().id) + ":" +
                        std::to_string(ctx.randomInt(0, 999999)));
    }
    void onEvent(const DeterminismEvent &event, BehaviorContext &ctx) {
        trace.push_back("event:" + std::to_string(ctx.params<Params>().id) + ":" +
                        std::to_string(event.value) + ":" +
                        std::to_string(ctx.randomInt(0, 999999)));
    }
    void onUpdate(BehaviorContext &ctx) override {
        trace.push_back("update:" + std::to_string(ctx.params<Params>().id) + ":" +
                        std::to_string(ctx.randomInt(0, 999999)));
    }
    void onDestroy(BehaviorContext &ctx) noexcept override {
        trace.push_back("destroy:" + std::to_string(ctx.params<Params>().id) + ":" +
                        std::to_string(ctx.randomInt(0, 999999)));
    }
};

PELICAN_REGISTER_BEHAVIOR(DeterminismBehavior, "wp155_determinism", 1);

} // namespace
} // namespace Pelican

int main(int argc, char **argv) {
    using namespace Pelican;
    setupLogger(true);
    FastModuleContainer modules;
    GET_MODULE(EngineLaunchConfig).input_replay =
        argc == 2 && std::string_view{argv[1]} == "--replay";
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(ProjectSource).setProjectData(nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "wp155-determinism"},
        {"engine_min_version", "0.1.0"},
        {"basic_config", {{"seed", 0x155beefULL}}},
    }.dump());
    GET_MODULE(DeterministicRng).setSeed(0x155beefULL);

    const auto objects = nlohmann::json::array({
        {{"name", "First"},
         {"components", nlohmann::json::array({
              {{"name", "behavior"}, {"type", "wp155_determinism"}, {"params", {{"id", 1}}}},
              {{"name", "behavior"}, {"type", "wp155_determinism"}, {"params", {{"id", 2}}}},
          })}},
        {{"name", "Second"},
         {"components", nlohmann::json::array({
              {{"name", "behavior"}, {"type", "wp155_determinism"}, {"params", {{"id", 3}}}},
          })}},
    });
    auto prepared = prepareSceneBehaviorAttachments(objects, BehaviorRegistryAvailability::active);
    std::vector<GameObjectId> entities(objects.size(), invalidGameObjectId);
    std::vector<BoundSceneBehaviorAttachment> bound;
    for (auto &attachment : prepared) {
        auto &entity = entities[attachment.object_index];
        if (entity == invalidGameObjectId) {
            entity = GameObjects::createWithComponents(std::span<const ComponentId>{}, {});
        }
        bound.push_back({.prepared = std::move(attachment), .entity = entity});
    }

    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    arena.publishSceneAttachments(std::move(bound));
    arena.activatePublished();
    GameContext ctx;
    internal::dispatchEventToRegisteredGameSystems(
        internal::QueuedEvent{.type = std::type_index{typeid(DeterminismEvent)},
                              .name = "DeterminismEvent",
                              .payload = std::make_shared<DeterminismEvent>(DeterminismEvent{7})},
        ctx);
    internal::updateRegisteredGameSystems(ctx);
    internal::updateRegisteredGameSystems(ctx);
    arena.deactivateAll();

    for (const auto &entry : trace) {
        std::cout << entry << '\n';
    }
    return 0;
}
