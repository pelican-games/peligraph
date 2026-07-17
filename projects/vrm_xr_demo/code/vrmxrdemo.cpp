#include <animation/abi_v1.hpp>
#include <animation/animgraph.hpp>
#include <animation/vrm_application_v1.hpp>
#include <gamecontext.hpp>
#include <gamesystem.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view movement_graph = R"json({
  "schema":"pelican.anim_graph","version":1,
  "parameters":{"speed":0.0,"jump":0.0},"initial_state":"Locomotion",
  "states":[
    {"name":"Locomotion","type":"blend1d","parameter":"speed","clips":[
      {"threshold":0.0,"clip":"Idle","speed":1.0,"start_offset":0.0,"loop":true},
      {"threshold":0.45,"clip":"Walk","speed":1.0,"start_offset":0.0,"loop":true},
      {"threshold":1.0,"clip":"Run","speed":1.6,"start_offset":0.0,"loop":true}]},
    {"name":"Jump","type":"clip","clip":"Jump","speed":1.0,"start_offset":0.0,"loop":false}],
  "transitions":[
    {"from":"Locomotion","to":"Jump","priority":100,"interrupt":"always","duration":0.08,
     "conditions":[{"parameter":"jump","op":">","value":0.0}]},
    {"from":"Jump","to":"Locomotion","priority":100,"interrupt":"always","duration":0.16,
     "conditions":[{"parameter":"jump","op":"<=","value":0.0}]}]
})json";

constexpr std::array expression_names{
    std::string_view{"happy"}, std::string_view{"angry"},
    std::string_view{"sad"}, std::string_view{"relaxed"},
};

float radiansToDegrees(float radians) {
    return radians * 57.29577951308232f;
}

class VrmXrDemoSystem {
    std::optional<Pelican::AnimationGraph::EvaluatorV1> evaluator_;
    Pelican::Vrm::ApplicationServiceV1 application_{};
    Pelican::Animation::InstanceHandle instance_{};
    bool services_ready_ = false;
    bool ready_reported_ = false;
    std::size_t expression_index_ = 0;
    std::uint64_t input_revision_ = 0;
    std::uint64_t last_status_frame_ = ~std::uint64_t{};

    bool resolveApplicationInstance(Pelican::GameContext &ctx) {
        using namespace Pelican::Animation;
        application_.struct_size = sizeof(application_);
        application_.version = Pelican::Vrm::applicationDescriptorVersionV1;
        if (Pelican::Vrm::getApplicationServiceV1(
                Pelican::Vrm::applicationServiceVersionV1,
                &application_) != Status::ok) {
            ctx.logError("WP135 VRM application service negotiation failed");
            return false;
        }

        ApiV1 api{};
        api.struct_size = sizeof(api);
        api.version = descriptorVersionV1;
        AnimationServiceV1 service{};
        service.struct_size = sizeof(service);
        service.version = descriptorVersionV1;
        if (getApiV1(abiVersionV1, &api) != Status::ok ||
            api.get_animation_service == nullptr ||
            api.get_animation_service(api.context, animationServiceVersionV1,
                                      &service) != Status::ok) {
            ctx.logError("WP135 animation service negotiation failed");
            return false;
        }

        constexpr char object_name[] = "VrmXrHero";
        ResolveAnimationSinkDescV1 sink{};
        sink.struct_size = sizeof(sink);
        sink.version = descriptorVersionV1;
        sink.object_name = object_name;
        sink.object_name_size = sizeof(object_name) - 1;
        sink.sink_kind = AnimationSinkKind::skeletal_pose;
        if (service.resolve_sink(service.context, &sink) != Status::ok) return false;

        ResolveAnimationInstanceDescV1 instance{};
        instance.struct_size = sizeof(instance);
        instance.version = descriptorVersionV1;
        instance.sink = sink.sink;
        if (service.resolve_instance(service.context, &instance) != Status::ok) {
            ctx.logError("WP135 VRM instance resolution failed");
            return false;
        }
        instance_ = instance.instance;
        return true;
    }

    std::pair<float, float> lookAtAngles(Pelican::GameContext &ctx) const {
        const auto head = ctx.actionPose("head");
        if (head.valid &&
            head.source == Pelican::ActionPoseSource::synthetic_head) {
            constexpr float gaze_height = 0.65f;
            const float dx = head.position[0];
            const float dy = head.position[1] - gaze_height;
            const float dz = head.position[2];
            const float horizontal = std::max(0.0001f, std::sqrt(dx * dx + dz * dz));
            return {radiansToDegrees(std::atan2(dx, -dz)),
                    radiansToDegrees(std::atan2(-dy, horizontal))};
        }

        // The flat camera is authored at (0, 0, -5). Arrow/right-stick input
        // adds an inspection offset while preserving camera-follow as fallback.
        const auto look = ctx.actionAxis2("look");
        return {look.x * 30.0f, -look.y * 20.0f};
    }

    void updateExpression(Pelican::GameContext &ctx, float yaw, float pitch) {
        if (!Pelican::Animation::isValid(instance_)) return;
        if (ctx.actionPressed("expression_next")) {
            expression_index_ = (expression_index_ + 1u) % expression_names.size();
        }

        const auto name = expression_names[expression_index_];
        Pelican::Vrm::ExpressionWeightV1 weight{};
        weight.name = name.data();
        weight.name_size = static_cast<std::uint32_t>(name.size());
        weight.value = 1.0f;

        Pelican::Vrm::SetExpressionInputDescV1 input{};
        input.struct_size = sizeof(input);
        input.version = Pelican::Vrm::applicationDescriptorVersionV1;
        input.instance = instance_;
        input.weights = &weight;
        input.weight_count = 1;
        input.input_revision = ++input_revision_;
        input.look_at_yaw_degrees = yaw;
        input.look_at_pitch_degrees = pitch;
        input.flags = Pelican::Vrm::expression_input_look_at;
        const auto status = application_.set_expression_inputs(
            application_.context, &input);
        if (status != Pelican::Animation::Status::ok &&
            status != Pelican::Animation::Status::duplicate_revision) {
            ctx.logError("WP135 expression input failed status=" +
                         std::to_string(static_cast<std::uint32_t>(status)));
        }
    }

    void reportStatus(Pelican::GameContext &ctx, float speed, float yaw,
                      float pitch) {
        if (last_status_frame_ == ctx.frameIndex()) return;
        last_status_frame_ = ctx.frameIndex();
        const auto trace = evaluator_->getStatus();
        const auto head = ctx.actionPose("head");
        std::ostringstream message;
        message << std::fixed << std::setprecision(3)
                << "WP135_STATUS frame=" << ctx.frameIndex()
                << " state=" << (trace.current_state.empty() ? "binding" : trace.current_state)
                << " expression=" << expression_names[expression_index_]
                << " speed=" << speed
                << " yaw=" << yaw
                << " pitch=" << pitch
                << " gaze=" << (head.valid ? "synthetic_head" : "flat_camera")
                << " hash=" << trace.semantic_pose_hash;
        ctx.logInfo(message.str());
    }

  public:
    void update(Pelican::GameContext &ctx) {
        if (!evaluator_) {
            evaluator_.emplace(
                Pelican::AnimationGraph::parseDocumentV1(movement_graph),
                "VrmXrHero");
        }
        if (!evaluator_->bound()) {
            const auto status = evaluator_->bind();
            if (status == Pelican::Animation::Status::not_found) return;
            if (status != Pelican::Animation::Status::ok) {
                ctx.logError("WP135 anim graph bind failed: " + evaluator_->lastError());
                return;
            }
        }
        if (!services_ready_) {
            services_ready_ = resolveApplicationInstance(ctx);
            if (!services_ready_) return;
        }
        if (!ready_reported_) {
            ready_reported_ = true;
            ctx.logInfo("WP135 VRM XR demo ready: WASD/pad/Touch locomotion; Space/A jumps; E/B cycles expressions; gaze follows camera/head");
        }

        const auto move = ctx.actionAxis2("move");
        const float speed = std::clamp(
            std::sqrt(move.x * move.x + move.y * move.y), 0.0f, 1.0f);
        const bool jumping = ctx.actionHeld("jump");
        (void)evaluator_->setParameter("speed", speed);
        (void)evaluator_->setParameter("jump", jumping ? 1.0 : 0.0);
        const auto tick = evaluator_->prepareTick(
            ctx.time(), ctx.deltaTime(), ctx.frameIndex());
        if (tick != Pelican::Animation::Status::ok) {
            ctx.logError("WP135 anim graph tick failed: " + evaluator_->lastError());
            return;
        }

        const auto [yaw, pitch] = lookAtAngles(ctx);
        updateExpression(ctx, yaw, pitch);
        reportStatus(ctx, speed, yaw, pitch);
    }
};

} // namespace

PELICAN_REGISTER_SYSTEM(VrmXrDemoSystem, 100);
