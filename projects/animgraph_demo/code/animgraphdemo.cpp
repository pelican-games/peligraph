#include <animation/animgraph.hpp>
#include <gamecontext.hpp>
#include <gamesystem.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace {

constexpr std::string_view movement_graph = R"json({
  "schema":"pelican.anim_graph","version":1,
  "parameters":{"speed":0.0,"jump":0.0},"initial_state":"Move",
  "states":[
    {"name":"Move","type":"blend1d","parameter":"speed","clips":[
      {"threshold":0.0,"clip":"Walk","speed":1.0,"start_offset":0.0,"loop":true},
      {"threshold":1.0,"clip":"Run","speed":1.6,"start_offset":0.0,"loop":true}]},
    {"name":"Jump","type":"clip","clip":"Jump","speed":1.0,"start_offset":0.0,"loop":false}],
  "transitions":[
    {"from":"Move","to":"Jump","priority":100,"interrupt":"always","duration":0.12,
     "conditions":[{"parameter":"jump","op":">","value":0.0}]},
    {"from":"Jump","to":"Move","priority":100,"interrupt":"always","duration":0.18,
     "conditions":[{"parameter":"jump","op":"<=","value":0.0}]}]
})json";

class AnimGraphDemoSystem {
    std::optional<Pelican::AnimationGraph::EvaluatorV1> evaluator;
    std::uint64_t last_reported_second = ~std::uint64_t{};

  public:
    void update(Pelican::GameContext &ctx) {
        if (!evaluator) {
            evaluator.emplace(Pelican::AnimationGraph::parseDocumentV1(movement_graph), "AnimGraphHero");
        }
        if (!evaluator->bound()) {
            const auto status = evaluator->bind();
            if (status == Pelican::Animation::Status::not_found) return;
            if (status != Pelican::Animation::Status::ok) {
                ctx.logError("WP101 animgraph demo bind failed: " + evaluator->lastError());
                return;
            }
            ctx.logInfo("WP101 animgraph demo ready: WASD blends Walk/Run; Space interrupts with Jump");
        }

        const auto move = ctx.actionAxis2("move");
        const auto speed = std::clamp(std::sqrt(move.x * move.x + move.y * move.y), 0.0f, 1.0f);
        const auto jumping = ctx.actionHeld("jump");
        (void)evaluator->setParameter("speed", speed);
        (void)evaluator->setParameter("jump", jumping ? 1.0 : 0.0);
        const auto status = evaluator->prepareTick(ctx.time(), ctx.deltaTime(), ctx.frameIndex());
        if (status != Pelican::Animation::Status::ok) {
            ctx.logError("WP101 animgraph demo tick failed: " + evaluator->lastError());
            return;
        }

        const auto second = static_cast<std::uint64_t>(std::max(0.0, ctx.time()));
        if (second != last_reported_second) {
            last_reported_second = second;
            const auto trace = evaluator->getStatus();
            ctx.logInfo("WP101 trace state=" + trace.current_state +
                        " hash=" + std::to_string(trace.semantic_pose_hash));
        }
    }
};

} // namespace

PELICAN_REGISTER_SYSTEM(AnimGraphDemoSystem, 100);
