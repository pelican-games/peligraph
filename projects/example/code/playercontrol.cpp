#include <animation/abi_v1.hpp>
#include <animation/vrm_application_v1.hpp>
#include <components/modelview.hpp>
#include <components/predefined.hpp>
#include <gameobjects.hpp>
#include <gamesystem.hpp>

#include <cmath>

#ifndef WP90_BEHAVIOR_VERSION
#define WP90_BEHAVIOR_VERSION 1
#endif

namespace {

class ExamplePlayerControl {
    Pelican::GameObjectId object = Pelican::invalidGameObjectId;
    bool object_created = false;
    bool preview_probe_completed = false;
    Pelican::Vrm::ApplicationServiceV1 preview_application{};
    Pelican::Animation::InstanceHandle preview_instance{};
    std::uint64_t preview_revision = 0;

    void updateVrmExpressionPreview(Pelican::GameContext &ctx) {
        using namespace Pelican;
        using namespace Pelican::Animation;
        if (!preview_probe_completed) {
            preview_application.struct_size = sizeof(preview_application);
            preview_application.version = Vrm::applicationDescriptorVersionV1;
            if (Vrm::getApplicationServiceV1(Vrm::applicationServiceVersionV1,
                                             &preview_application) != Status::ok) {
                preview_probe_completed = true;
                return;
            }
            ApiV1 api{};
            api.struct_size = sizeof(api);
            api.version = descriptorVersionV1;
            AnimationServiceV1 service{};
            service.struct_size = sizeof(service);
            service.version = descriptorVersionV1;
            if (getApiV1(abiVersionV1, &api) != Status::ok ||
                !api.get_animation_service ||
                api.get_animation_service(api.context, animationServiceVersionV1,
                                          &service) != Status::ok) {
                preview_probe_completed = true;
                return;
            }
            constexpr char object_name[] = "VrmExpressionPreview";
            ResolveAnimationSinkDescV1 sink{};
            sink.struct_size = sizeof(sink);
            sink.version = descriptorVersionV1;
            sink.object_name = object_name;
            sink.object_name_size = sizeof(object_name) - 1;
            sink.sink_kind = AnimationSinkKind::skeletal_pose;
            if (service.resolve_sink(service.context, &sink) != Status::ok) {
                preview_probe_completed = true;
                return;
            }
            ResolveAnimationInstanceDescV1 instance{};
            instance.struct_size = sizeof(instance);
            instance.version = descriptorVersionV1;
            instance.sink = sink.sink;
            if (service.resolve_instance(service.context, &instance) != Status::ok) {
                preview_probe_completed = true;
                return;
            }
            preview_instance = instance.instance;
            preview_probe_completed = true;
            ctx.logInfo("WP123 VRM expression preview connected");
        }
        if (!Pelican::Animation::isValid(preview_instance)) return;

        constexpr char expression_name[] = "happy";
        Vrm::ExpressionWeightV1 weight{};
        weight.name = expression_name;
        weight.name_size = sizeof(expression_name) - 1;
        weight.value = static_cast<float>(
            0.5 + 0.5 * std::sin(ctx.time() * 1.5));
        Vrm::SetExpressionInputDescV1 input{};
        input.struct_size = sizeof(input);
        input.version = Vrm::applicationDescriptorVersionV1;
        input.instance = preview_instance;
        input.weights = &weight;
        input.weight_count = 1;
        input.input_revision = ++preview_revision;
        input.look_at_yaw_degrees = static_cast<float>(
            30.0 * std::sin(ctx.time() * 0.75));
        input.flags = Vrm::expression_input_look_at;
        if (preview_application.set_expression_inputs(
                preview_application.context, &input) != Status::ok) {
            preview_instance = Animation::invalidHandle<InstanceHandle>();
            ctx.logWarning("WP123 VRM expression preview input stopped");
        }
    }

    Pelican::GameObjectId createControlledObject(Pelican::GameContext &ctx) {
        Pelican::LocalTransformComponent transform{
            .scale = Pelican::vec3{0.35f, 0.35f, 0.35f},
            .rotation = Pelican::quat{0.0f, 0.7071f, -0.7071f, 0.0f},
            .pos = Pelican::vec3{0.0f, -1.2f, 1.0f},
            .parent = Pelican::invalidGameObjectId,
        };
        Pelican::SimpleModelViewComponent model_view;
        model_view.model_name = "character";

        const auto id = Pelican::GameObjects::add()
                            .addComponent<Pelican::TransformComponent>()
                            .addComponent<Pelican::LocalTransformComponent>(transform)
                            .addComponent<Pelican::SimpleModelViewComponent>(model_view)
                            .finish();
        (void)ctx.setLocalTransform(id, transform);
        return id;
    }

  public:
    void update(Pelican::GameContext &ctx) {
#ifdef WP90_HOT_RELOAD_FIXTURE
#if WP90_BEHAVIOR_VERSION == 1
        ctx.logInfo("WP90 playercontrol behavior=v1 speed=2.5");
#else
        ctx.logInfo("WP90 playercontrol behavior=v2 speed=5.0");
#endif
#endif
        updateVrmExpressionPreview(ctx);
        if (!object_created) {
            object = createControlledObject(ctx);
            object_created = true;
        }

        if (!ctx.actionsConfigured()) {
            return;
        }

        const auto move = ctx.actionAxis2("move");
        const bool jump = ctx.actionHeld("jump");
        if (move.x == 0.0f && move.y == 0.0f && !jump) {
            return;
        }

        auto transform = ctx.localTransform(object);
        const auto dt = static_cast<float>(ctx.deltaTime() > 0.0 ? ctx.deltaTime() : 1.0 / 60.0);
#if WP90_BEHAVIOR_VERSION == 1
        constexpr float speed = 2.5f;
#else
        constexpr float speed = 5.0f;
#endif
        transform.pos.x += move.x * speed * dt;
        transform.pos.z += move.y * speed * dt;
        if (jump) {
            transform.pos.y += speed * dt;
        }
        (void)ctx.setLocalTransform(object, transform);
    }
};

} // namespace

PELICAN_REGISTER_SYSTEM(ExamplePlayerControl, 100);
