#include "fixture_protocol.hpp"
#include "animation/abi_v1.hpp"

#include <array>
#include <cstdint>

using namespace Pelican::Animation;

namespace {

template <class T> T descriptor() {
    T value{};
    value.struct_size = sizeof(T);
    value.version = descriptorVersionV1;
    return value;
}

Status phaseCallback(void *user_context, const AnimationPhaseContextV1 *context) {
    if (!user_context || !context || context->phase != Phase::base_pose_and_root_modifier)
        return Status::invalid_argument;
    ++static_cast<AnimationEvaluatorFixtureResult *>(user_context)->phase_callback_count;
    return Status::ok;
}

Status acquire(const AnimationServiceV1 &service, PoseArenaHandle arena, PoseLayoutHandle layout,
               std::uint32_t joints, PoseViewV1 &view) {
    view = descriptor<PoseViewV1>();
    auto request = descriptor<AcquirePoseDescV1>();
    request.arena = arena;
    request.layout = layout;
    request.joint_count = joints;
    request.out_view = &view;
    return service.acquire_pose(service.context, &request);
}

Status resolveClip(const AnimationServiceV1 &service, RigHandle rig, const char *name,
                   std::uint32_t name_size, ClipHandle &clip) {
    auto request = descriptor<ResolveAnimationClipDescV1>();
    request.rig = rig;
    request.clip_name = name;
    request.clip_name_size = name_size;
    const auto status = service.resolve_clip(service.context, &request);
    clip = request.clip;
    return status;
}

} // namespace

ANIM_FIXTURE_EXPORT std::uint32_t pelican_animation_public_evaluator(
    const ApiV1 *api, AnimationEvaluatorFixtureResult *result) {
    if (!api || !result || !api->get_animation_service) return static_cast<std::uint32_t>(Status::invalid_argument);
    *result = {};
    auto fail = [&](Status status) {
        result->status = static_cast<std::uint32_t>(status);
        return result->status;
    };

    auto service = descriptor<AnimationServiceV1>();
    if (const auto status = api->get_animation_service(api->context, animationServiceVersionV1, &service);
        status != Status::ok)
        return fail(status);
    if ((service.capability_bits & animationServiceCapabilitiesV1) != animationServiceCapabilitiesV1)
        return fail(Status::unsupported_version);

    auto owner = descriptor<CurrentAnimationOwnerDescV1>();
    if (const auto status = service.get_current_owner(service.context, &owner); status != Status::ok)
        return fail(status);
    constexpr char object_name[] = "Hero";
    auto sink = descriptor<ResolveAnimationSinkDescV1>();
    sink.object_name = object_name;
    sink.object_name_size = sizeof(object_name) - 1;
    sink.sink_kind = AnimationSinkKind::skeletal_pose;
    if (const auto status = service.resolve_sink(service.context, &sink); status != Status::ok)
        return fail(status);
    result->sink_identity = sink.sink.identity;
    result->sink_generation = sink.sink.generation;

    auto instance = descriptor<ResolveAnimationInstanceDescV1>();
    instance.sink = sink.sink;
    if (const auto status = service.resolve_instance(service.context, &instance); status != Status::ok)
        return fail(status);
    auto rig = descriptor<ResolveAnimationRigDescV1>();
    rig.instance = instance.instance;
    if (const auto status = service.resolve_rig(service.context, &rig); status != Status::ok)
        return fail(status);
    auto layout = descriptor<ResolvePoseLayoutDescV1>();
    layout.rig = rig.rig;
    if (const auto status = service.resolve_layout(service.context, &layout); status != Status::ok)
        return fail(status);

    ClipHandle clip_a{}, clip_b{};
    if (const auto status = resolveClip(service, rig.rig, "MoveA", 5, clip_a); status != Status::ok)
        return fail(status);
    if (const auto status = resolveClip(service, rig.rig, "MoveB", 5, clip_b); status != Status::ok)
        return fail(status);
    auto metadata = descriptor<ClipMetadataV1>();
    metadata.clip = clip_a;
    if (const auto status = service.get_clip_metadata(service.context, &metadata); status != Status::ok)
        return fail(status);

    auto cursor = descriptor<CreateClipCursorDescV1>();
    cursor.owner = owner.owner;
    cursor.clip = clip_a;
    if (const auto status = service.create_cursor(service.context, &cursor); status != Status::ok)
        return fail(status);
    auto advance = descriptor<AdvanceDescV1>();
    advance.cursor = cursor.cursor;
    advance.delta_seconds = 0.25;
    auto interval = descriptor<IntervalResultV1>();
    result->cursor_advance_status = static_cast<std::uint32_t>(
        api->advance_cursor(api->context, &advance, &interval));
    auto destroy_cursor = descriptor<DestroyClipCursorDescV1>();
    destroy_cursor.owner = owner.owner;
    destroy_cursor.cursor = cursor.cursor;
    if (const auto status = service.destroy_cursor(service.context, &destroy_cursor); status != Status::ok)
        return fail(status);

    auto frame = descriptor<BeginPoseArenaFrameDescV1>();
    frame.owner = owner.owner;
    frame.frame_revision = 42;
    if (const auto status = service.begin_pose_frame(service.context, &frame); status != Status::ok)
        return fail(status);
    std::array<PoseViewV1, 5> poses{};
    for (auto &pose : poses)
        if (const auto status = acquire(service, frame.arena, layout.layout, layout.joint_count, pose);
            status != Status::ok)
            return fail(status);

    auto sample = descriptor<SamplePoseAtDescV1>();
    sample.clip = clip_a;
    sample.time_seconds = 0.5;
    sample.output_pose = poses[0].pose;
    if (const auto status = service.sample_pose_at(service.context, &sample); status != Status::ok)
        return fail(status);
    result->single_clip_translation_x = poses[0].translations[1].x;
    sample.time_seconds = 0.25;
    sample.output_pose = poses[1].pose;
    if (const auto status = service.sample_pose_at(service.context, &sample); status != Status::ok)
        return fail(status);
    sample.clip = clip_b;
    sample.output_pose = poses[2].pose;
    if (const auto status = service.sample_pose_at(service.context, &sample); status != Status::ok)
        return fail(status);

    std::array<BlendLayerV1, 2> layers{};
    for (auto &layer : layers) {
        layer = descriptor<BlendLayerV1>();
        layer.mode = BlendMode::normal;
        layer.additive_space = AdditiveSpace::local;
        layer.rest_fallback = RestFallback::use_rest_pose;
        layer.root_policy = SidebandPolicy::suppress;
        layer.curve_policy = SidebandPolicy::suppress;
        layer.event_marker_policy = SidebandPolicy::suppress;
    }
    layers[0].pose = poses[1].pose;
    layers[0].weight = 0.25f;
    layers[1].pose = poses[2].pose;
    layers[1].weight = 0.75f;
    auto blend = descriptor<BlendNormalDescV1>();
    blend.rig = rig.rig;
    blend.layers = layers.data();
    blend.layer_count = static_cast<std::uint32_t>(layers.size());
    blend.output_pose = poses[3].pose;
    if (const auto status = service.blend_normal(service.context, &blend); status != Status::ok)
        return fail(status);
    result->blended_translation_x = poses[3].translations[1].x;

    std::array<Matrix4fV1, 2> model_matrices{};
    auto local_to_model = descriptor<LocalToModelDescV1>();
    local_to_model.rig = rig.rig;
    local_to_model.local_pose = poses[3].pose;
    local_to_model.model_pose = poses[4].pose;
    local_to_model.model_matrices = model_matrices.data();
    local_to_model.model_matrix_capacity = static_cast<std::uint32_t>(model_matrices.size());
    if (const auto status = service.local_to_model(service.context, &local_to_model); status != Status::ok)
        return fail(status);
    std::array<Matrix4fV1, 1> palette{};
    auto build_palette = descriptor<BuildSkinPaletteDescV1>();
    build_palette.skin_binding = layout.skin_binding;
    build_palette.model_matrices = model_matrices.data();
    build_palette.model_matrix_count = local_to_model.model_matrix_count;
    build_palette.palette = palette.data();
    build_palette.palette_capacity = static_cast<std::uint32_t>(palette.size());
    if (const auto status = service.build_skin_palette(service.context, &build_palette); status != Status::ok)
        return fail(status);
    result->palette_translation_x = palette[0].column_major[12];

    auto claim = descriptor<ClaimAnimationSourceDescV1>();
    claim.owner = owner.owner;
    claim.sink = sink.sink;
    claim.source_ordinal = 7;
    if (const auto status = service.claim_source(service.context, &claim); status != Status::ok)
        return fail(status);
    result->source_identity = claim.source.identity;
    result->source_generation = claim.source.generation;
    auto notification = descriptor<AnimationNotificationDescV1>();
    notification.kind = AnimationNotificationKind::set_time;
    notification.sink = sink.sink;
    notification.source = claim.source;
    notification.time_seconds = 0.25;
    notification.notification_revision = 1;
    if (const auto status = service.notify(service.context, &notification); status != Status::ok)
        return fail(status);

    auto registration = descriptor<RegisterAnimationPhaseDescV1>();
    registration.owner = owner.owner;
    registration.registration = descriptor<PhaseRegistrationV1>();
    registration.registration.phase = Phase::base_pose_and_root_modifier;
    registration.registration.priority = 5;
    registration.registration.registration_identity = 1000;
    registration.registration.registration_generation = owner.owner.generation;
    registration.registration.source_ordinal = 7;
    registration.callback = phaseCallback;
    registration.user_context = result;
    if (const auto status = service.register_phase(service.context, &registration); status != Status::ok)
        return fail(status);
    auto unregister = descriptor<UnregisterAnimationPhaseDescV1>();
    unregister.owner = owner.owner;
    unregister.registration = registration.registration_handle;
    if (const auto status = service.unregister_phase(service.context, &unregister); status != Status::ok)
        return fail(status);
    registration.registration.registration_identity = 1001;
    registration.registration_handle = invalidHandle<PhaseRegistrationHandle>();
    if (const auto status = service.register_phase(service.context, &registration); status != Status::ok)
        return fail(status);

    auto publish = descriptor<PublishAnimationFrameFromSourceDescV1>();
    publish.source = claim.source;
    publish.frame = descriptor<PublishAnimationFrameDescV1>();
    publish.frame.instance = instance.instance;
    publish.frame.local_pose = poses[3].pose;
    publish.frame.model_pose = poses[4].pose;
    publish.frame.palette = palette.data();
    publish.frame.palette_count = static_cast<std::uint32_t>(palette.size());
    publish.frame.frame_revision = 42;
    publish.frame.root_delta.rotation.w = 1.0f;
    if (const auto status = service.publish_animation_frame_from_source(service.context, &publish);
        status != Status::ok)
        return fail(status);
    return fail(Status::ok);
}
