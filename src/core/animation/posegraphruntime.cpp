#include "posegraphruntime.hpp"

#include "../userpublic/animation/abi_v1.hpp"
#include "../userpublic/animation/animgraph.hpp"
#include "../userpublic/animation/pose_staging_v1.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>
#include <vector>

namespace Pelican::Animation {
namespace {
using Json = nlohmann::json;

template <class T> T descriptor() {
    T value{};
    value.struct_size = sizeof(T);
    value.version = descriptorVersionV1;
    return value;
}

std::uint64_t fnv1a(std::uint64_t hash, const void *bytes, std::size_t size) {
    const auto *data = static_cast<const std::uint8_t *>(bytes);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

double canonicalTime(double time, double start, double end, bool loop) {
    const auto duration = end - start;
    if (!(duration > 0.0)) return start;
    if (!loop) return std::clamp(time, start, end);
    auto offset = std::fmod(time - start, duration);
    if (offset < 0.0) offset += duration;
    return start + offset;
}

// The pose graph's own source slot. Higher than the animation component's 100,
// so a handoff between them is ordered rather than accidental.
constexpr std::uint32_t poseGraphSourceOrdinal = 120;

[[noreturn]] void statusFailure(const std::string &what, Status status) {
    throw std::runtime_error("pose graph " + what + " failed with animation status " +
                             std::to_string(static_cast<std::uint32_t>(status)));
}
} // namespace

// One live sink: the object it writes, everything the chain feeding it needs,
// and the phase registration that evaluates it. A wrapped state machine keeps
// the writer itself, so the attachment binds no source of its own.
struct PoseGraphRuntime::Impl {
    struct NodeBinding {
        AnimationSinkHandle sink{};
        InstanceHandle instance{};
        RigHandle rig{};
        PoseLayoutHandle layout{};
        std::uint32_t joint_count = 0;
        ClipHandle clip{};
        ClipMetadataV1 metadata{};
        CursorHandle cursor{};
        std::unique_ptr<AnimationGraph::EvaluatorV1> evaluator;
    };
    struct Attachment {
        Impl *runtime = nullptr;
        std::shared_ptr<const PoseGraph> graph;
        std::size_t sink = 0;
        std::string object_name;
        GraphInstanceOwner owner;
        // Reachable producers of this sink, in dependency order.
        std::vector<std::size_t> chain;
        std::map<std::size_t, NodeBinding> bindings;
        AnimationServiceV1 service{};
        AnimationOwnerHandle service_owner{};
        AnimationSinkHandle sink_handle{};
        AnimationSourceHandle source{};
        InstanceHandle instance{};
        RigHandle rig{};
        PoseLayoutHandle layout{};
        SkinBindingHandle skin_binding{};
        std::uint32_t joint_count = 0;
        std::uint32_t palette_count = 0;
        PhaseRegistrationHandle phase_registration{};
        bool evaluator_writes = false;
        bool bound = false;
        bool failed = false;
        // One evaluation per frame revision: the phase refuses a revision it
        // was not prepared for instead of running twice.
        bool tick_pending = false;
        std::uint64_t pending_revision = 0;
        std::uint64_t time_set_revision = 0;
        double time = 0.0;
        double delta_seconds = 0.0;
        std::map<std::string, double, std::less<>> scalars;
        SinkTrace trace;
    };

    std::map<const PoseGraph *, std::vector<std::unique_ptr<Attachment>>> attachments;
    std::map<const PoseGraph *, std::weak_ptr<const PoseGraph>> published;

    static Status phase(void *user, const AnimationPhaseContextV1 *context) noexcept;

    static ApiV1 api() {
        auto value = descriptor<ApiV1>();
        if (getApiV1(abiVersionV1, &value) != Status::ok || !value.get_animation_service)
            throw std::runtime_error("pose graph needs AnimationServiceV1");
        return value;
    }

    static AnimationServiceV1 service() {
        const auto entry = api();
        auto value = descriptor<AnimationServiceV1>();
        if (entry.get_animation_service(entry.context, animationServiceVersionV1, &value) != Status::ok)
            throw std::runtime_error("AnimationServiceV1 negotiation failed for the pose graph");
        return value;
    }

    static void resolveObject(const AnimationServiceV1 &service, const std::string &object,
                              AnimationSinkKind kind, AnimationSinkHandle &sink,
                              InstanceHandle &instance, RigHandle &rig, PoseLayoutHandle &layout,
                              SkinBindingHandle &skin_binding, std::uint32_t &joints,
                              std::uint32_t &palette) {
        auto sink_request = descriptor<ResolveAnimationSinkDescV1>();
        sink_request.object_name = object.data();
        sink_request.object_name_size = static_cast<std::uint32_t>(object.size());
        sink_request.sink_kind = kind;
        if (const auto status = service.resolve_sink(service.context, &sink_request); status != Status::ok)
            statusFailure("sink resolution for " + object, status);
        sink = sink_request.sink;
        auto instance_request = descriptor<ResolveAnimationInstanceDescV1>();
        instance_request.sink = sink;
        if (const auto status = service.resolve_instance(service.context, &instance_request); status != Status::ok)
            statusFailure("instance resolution for " + object, status);
        instance = instance_request.instance;
        auto rig_request = descriptor<ResolveAnimationRigDescV1>();
        rig_request.instance = instance;
        if (const auto status = service.resolve_rig(service.context, &rig_request); status != Status::ok)
            statusFailure("rig resolution for " + object, status);
        rig = rig_request.rig;
        auto layout_request = descriptor<ResolvePoseLayoutDescV1>();
        layout_request.rig = rig;
        if (const auto status = service.resolve_layout(service.context, &layout_request); status != Status::ok)
            statusFailure("layout resolution for " + object, status);
        layout = layout_request.layout;
        skin_binding = layout_request.skin_binding;
        joints = layout_request.joint_count;
        palette = layout_request.palette_count;
    }

    void unbind(Attachment &attachment) noexcept {
        auto &service = attachment.service;
        if (isValid(attachment.phase_registration) && service.unregister_phase) {
            auto request = descriptor<UnregisterAnimationPhaseDescV1>();
            request.owner = attachment.service_owner;
            request.registration = attachment.phase_registration;
            (void)service.unregister_phase(service.context, &request);
        }
        for (auto &[node, binding] : attachment.bindings) {
            (void)node;
            if (binding.evaluator) binding.evaluator->unbind();
            if (isValid(binding.cursor) && service.destroy_cursor) {
                auto request = descriptor<DestroyClipCursorDescV1>();
                request.owner = attachment.service_owner;
                request.cursor = binding.cursor;
                (void)service.destroy_cursor(service.context, &request);
            }
        }
        if (isValid(attachment.source) && service.release_source) {
            auto request = descriptor<ReleaseAnimationSourceDescV1>();
            request.owner = attachment.service_owner;
            request.source = attachment.source;
            (void)service.release_source(service.context, &request);
        }
        attachment.bindings.clear();
        attachment.phase_registration = {};
        attachment.source = {};
        attachment.bound = false;
        attachment.tick_pending = false;
    }

    // Producers reachable from one sink, in the graph's dependency order.
    static std::vector<std::size_t> chainOf(const PoseGraph &graph, std::size_t sink) {
        std::set<std::size_t> reachable;
        const auto walk = [&](auto &&self, std::size_t node) -> void {
            if (!reachable.insert(node).second) return;
            for (const auto &[port, source] : graph.nodes()[node].pose_inputs) {
                (void)port;
                self(self, source);
            }
        };
        walk(walk, sink);
        std::vector<std::size_t> chain;
        for (const auto node : graph.order())
            if (reachable.contains(node)) chain.push_back(node);
        return chain;
    }

    void bind(Attachment &attachment, const PoseGraphFrame &frame);
    Status evaluate(Attachment &attachment);
};

Status PoseGraphRuntime::Impl::phase(void *user, const AnimationPhaseContextV1 *context) noexcept {
    if (!user || !context || context->phase != Phase::base_pose_and_root_modifier)
        return Status::invalid_argument;
    auto &attachment = *static_cast<Attachment *>(user);
    if (context->sink.identity != attachment.sink_handle.identity ||
        context->sink.generation != attachment.sink_handle.generation)
        return Status::ok;
    if (!attachment.tick_pending || context->frame_revision != attachment.pending_revision)
        return Status::invalid_argument;
    attachment.tick_pending = false;
    if (!attachment.runtime || !attachment.graph) return Status::invalid_handle;
    try {
        return attachment.runtime->evaluate(attachment);
    } catch (...) {
        // A named pose failure cannot cross the C callback boundary; the owner
        // thread re-raises it on the next synchronize, which runs outside a phase.
        attachment.failed = true;
        return Status::callback_failed;
    }
}

void PoseGraphRuntime::Impl::bind(Attachment &attachment, const PoseGraphFrame &frame) {
    struct Pending {
        Impl &impl;
        Attachment &attachment;
        ~Pending() { if (!attachment.bound) impl.unbind(attachment); }
    } pending{*this, attachment};
    const auto &graph = *attachment.graph;
    attachment.service = service();
    auto owner_request = descriptor<CurrentAnimationOwnerDescV1>();
    if (const auto status = attachment.service.get_current_owner(attachment.service.context, &owner_request);
        status != Status::ok)
        statusFailure("owner resolution", status);
    attachment.service_owner = owner_request.owner;
    // pose.transform names the object transform sink and takes part in the
    // single-writer rule, but writing a root transform from a pose has no
    // publication yet. Refuse it here rather than publishing a skeletal frame
    // through the transform sink; AG-2 brings root motion.
    if (graph.nodes()[attachment.sink].op == PoseGraphOp::transform_sink)
        throw std::runtime_error("pose.transform has no root writer yet: " +
                                 graph.nodes()[attachment.sink].id);
    const auto kind = AnimationSinkKind::skeletal_pose;
    resolveObject(attachment.service, attachment.object_name, kind, attachment.sink_handle,
                  attachment.instance, attachment.rig, attachment.layout, attachment.skin_binding,
                  attachment.joint_count, attachment.palette_count);

    for (const auto node : attachment.chain) {
        const auto &compiled = graph.nodes()[node];
        if (compiled.op != PoseGraphOp::clip_sample && compiled.op != PoseGraphOp::state_machine)
            continue;
        auto &binding = attachment.bindings[node];
        const auto object = frame.object_name(compiled.owner);
        if (!object)
            throw std::runtime_error("pose operation owner is not live: " + compiled.id);
        if (frame.verify_rig) frame.verify_rig(compiled.owner, compiled.rig, compiled.id);
        SkinBindingHandle unused_binding{};
        std::uint32_t unused_palette = 0;
        resolveObject(attachment.service, *object, AnimationSinkKind::skeletal_pose, binding.sink,
                      binding.instance, binding.rig, binding.layout, unused_binding,
                      binding.joint_count, unused_palette);
        if (compiled.op == PoseGraphOp::state_machine) {
            // The wrapped evaluator is the writer for its own object; the graph
            // declares that intent with the sink and claims nothing itself.
            binding.evaluator = std::make_unique<AnimationGraph::EvaluatorV1>(
                AnimationGraph::parseDocumentV1(compiled.state_machine.dump()), *object);
            if (const auto status = binding.evaluator->bind(); status != Status::ok)
                throw std::runtime_error("pose.state_machine bind failed for " + compiled.id + ": " +
                                         binding.evaluator->lastError());
            attachment.evaluator_writes = true;
            continue;
        }
        auto resolve = descriptor<ResolveAnimationClipDescV1>();
        resolve.rig = binding.rig;
        resolve.clip_name = compiled.clip.data();
        resolve.clip_name_size = static_cast<std::uint32_t>(compiled.clip.size());
        if (const auto status = attachment.service.resolve_clip(attachment.service.context, &resolve);
            status != Status::ok)
            statusFailure("clip resolution for " + compiled.clip, status);
        binding.clip = resolve.clip;
        binding.metadata = descriptor<ClipMetadataV1>();
        binding.metadata.clip = binding.clip;
        if (const auto status = attachment.service.get_clip_metadata(attachment.service.context, &binding.metadata);
            status != Status::ok)
            statusFailure("clip metadata for " + compiled.clip, status);
        if (!(binding.metadata.end_seconds > binding.metadata.start_seconds))
            throw std::runtime_error("pose.clip_sample needs a nonzero duration: " + compiled.id);
    }

    const auto &sink_node = graph.nodes()[attachment.sink];
    if (!attachment.evaluator_writes) {
        auto claim = descriptor<ClaimAnimationSourceDescV1>();
        claim.owner = attachment.service_owner;
        claim.sink = attachment.sink_handle;
        claim.source_ordinal = poseGraphSourceOrdinal;
        if (const auto status = attachment.service.claim_source(attachment.service.context, &claim);
            status != Status::ok) {
            // Somebody already writes this object. Name the conflict rather
            // than letting two writers race for the same sink.
            if (status == Status::authority_conflict)
                PoseGraph::raiseSinkConflict(attachment.owner, attachment.object_name, sink_node.id);
            statusFailure("source claim for " + attachment.object_name, status);
        }
        attachment.source = claim.source;
    }
    auto registration = descriptor<RegisterAnimationPhaseDescV1>();
    registration.owner = attachment.service_owner;
    registration.registration = descriptor<PhaseRegistrationV1>();
    registration.registration.phase = Phase::base_pose_and_root_modifier;
    // A wrapped evaluator publishes its object's frame in the same phase run.
    // Running after it is what lets the sink observe the staged pose instead of
    // guessing; a graph that writes the sink itself runs at the base priority.
    registration.registration.priority = attachment.evaluator_writes ? 1 : 0;
    registration.registration.registration_identity =
        fnv1a(0x504f534547524150ull, sink_node.id.data(), sink_node.id.size());
    registration.registration.registration_generation = attachment.service_owner.generation;
    registration.registration.source_ordinal = poseGraphSourceOrdinal;
    registration.callback = &Impl::phase;
    registration.user_context = &attachment;
    if (const auto status = attachment.service.register_phase(attachment.service.context, &registration);
        status != Status::ok)
        statusFailure("phase registration for " + attachment.object_name, status);
    attachment.phase_registration = registration.registration_handle;
    attachment.bound = true;
}

Status PoseGraphRuntime::Impl::evaluate(Attachment &attachment) {
    const auto &graph = *attachment.graph;
    auto &service = attachment.service;
    // Beginning an arena frame retires what this owner staged earlier in the
    // same phase run, so a chain that only reads a staged pose must never open
    // one. The arena opens on the first operation that actually needs storage.
    auto begin = descriptor<BeginPoseArenaFrameDescV1>();
    bool arena_open = false;
    const auto openArena = [&] {
        if (arena_open) return Status::ok;
        begin.owner = attachment.service_owner;
        begin.frame_revision = attachment.pending_revision;
        const auto status = service.begin_pose_frame(service.context, &begin);
        arena_open = status == Status::ok;
        return status;
    };

    struct Produced {
        PoseViewV1 view{};
        PoseLayoutHandle layout{};
    };
    std::map<std::size_t, Produced> poses;
    const auto acquire = [&](PoseLayoutHandle layout, std::uint32_t joints, PoseViewV1 &view) {
        if (const auto status = openArena(); status != Status::ok) return status;
        view = descriptor<PoseViewV1>();
        auto request = descriptor<AcquirePoseDescV1>();
        request.arena = begin.arena;
        request.layout = layout;
        request.joint_count = joints;
        request.out_view = &view;
        return service.acquire_pose(service.context, &request);
    };
    const auto poseInput = [&](const PoseGraphNode &node, const char *port) -> const Produced & {
        const auto found = node.pose_inputs.find(port);
        if (found == node.pose_inputs.end())
            throw std::runtime_error(std::string{"pose port is unconnected: "} + node.id + "." + port);
        return poses.at(found->second);
    };
    const auto scalar = [&](const PoseGraphNode &node, const char *port, double fallback) {
        const auto found = node.scalar_inputs.find(port);
        if (found == node.scalar_inputs.end()) return fallback;
        const auto value = attachment.scalars.find(found->second);
        if (value == attachment.scalars.end())
            throw std::runtime_error("pose scalar input was not evaluated this frame: " + found->second);
        return value->second;
    };
    const auto sameLayout = [](PoseLayoutHandle left, PoseLayoutHandle right) {
        return left.identity == right.identity && left.generation == right.generation;
    };
    const auto requireLayout = [&](const PoseGraphNode &node, const char *port,
                                   PoseLayoutHandle expected, PoseLayoutHandle observed) {
        if (sameLayout(expected, observed)) return;
        PoseGraph::raiseLayoutMismatch(node.id, port,
            Json{{"identity", expected.identity}, {"generation", expected.generation}},
            Json{{"identity", observed.identity}, {"generation", observed.generation}});
    };

    for (const auto index : attachment.chain) {
        const auto &node = graph.nodes()[index];
        switch (node.op) {
        case PoseGraphOp::clip_sample: {
            auto &binding = attachment.bindings.at(index);
            Produced produced;
            produced.layout = binding.layout;
            if (const auto status = acquire(binding.layout, binding.joint_count, produced.view);
                status != Status::ok)
                return status;
            auto sample = descriptor<SamplePoseAtDescV1>();
            sample.clip = binding.clip;
            sample.time_seconds = canonicalTime(scalar(node, "time", attachment.time),
                binding.metadata.start_seconds, binding.metadata.end_seconds, true);
            sample.output_pose = produced.view.pose;
            if (const auto status = service.sample_pose_at(service.context, &sample); status != Status::ok)
                return status;
            poses.emplace(index, produced);
            break;
        }
        case PoseGraphOp::state_machine: {
            // The evaluator published this object's pose earlier in this same
            // phase run; the port carries that staged pose, not a copy.
            auto &binding = attachment.bindings.at(index);
            auto staged = descriptor<Animation::AcquireStagedPoseDescV1>();
            staged.instance = binding.instance;
            staged.frame_revision = attachment.pending_revision;
            auto staging = descriptor<PoseStagingServiceV1>();
            if (const auto status = getPoseStagingServiceV1(poseStagingServiceVersionV1, &staging);
                status != Status::ok)
                return status;
            if (const auto status = staging.acquire_staged_pose(staging.context, &staged);
                status != Status::ok)
                return status;
            Produced produced;
            produced.view = staged.local_pose;
            produced.layout = staged.local_pose.layout;
            poses.emplace(index, produced);
            break;
        }
        case PoseGraphOp::blend: {
            const auto &a = poseInput(node, "a");
            const auto &b = poseInput(node, "b");
            requireLayout(node, "b", a.layout, b.layout);
            Produced produced;
            produced.layout = a.layout;
            if (const auto status = acquire(a.layout, a.view.joint_count, produced.view);
                status != Status::ok)
                return status;
            const auto weight = static_cast<float>(std::clamp(scalar(node, "weight", 0.0), 0.0, 1.0));
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
            layers[0].pose = a.view.pose;
            layers[0].weight = 1.0f - weight;
            layers[1].pose = b.view.pose;
            layers[1].weight = weight;
            auto blend = descriptor<BlendNormalDescV1>();
            blend.rig = attachment.rig;
            blend.layers = layers.data();
            blend.layer_count = 2;
            blend.output_pose = produced.view.pose;
            if (const auto status = service.blend_normal(service.context, &blend); status != Status::ok)
                return status;
            poses.emplace(index, produced);
            break;
        }
        case PoseGraphOp::retarget: {
            // R0 applies a VRMA profile when the clip is baked, so at pose level
            // the node asserts that the pose it received belongs to the layout
            // the profile targets and hands the same staged pose on.
            const auto &input = poseInput(node, "pose");
            requireLayout(node, "pose", attachment.layout, input.layout);
            poses.emplace(index, input);
            break;
        }
        case PoseGraphOp::skeletal_sink:
        case PoseGraphOp::transform_sink: {
            const auto &input = poseInput(node, "pose");
            requireLayout(node, "pose", attachment.layout, input.layout);
            attachment.trace.sink = node.id;
            attachment.trace.node = graph.nodes()[node.pose_inputs.at("pose")].id;
            attachment.trace.layout_identity = input.layout.identity;
            attachment.trace.layout_generation = input.layout.generation;
            attachment.trace.frame_revision = attachment.pending_revision;
            auto hash = 1469598103934665603ull;
            hash = fnv1a(hash, input.view.translations, input.view.joint_count * sizeof(Vec4fV1));
            hash = fnv1a(hash, input.view.rotations, input.view.joint_count * sizeof(QuatfV1));
            hash = fnv1a(hash, input.view.scales, input.view.joint_count * sizeof(Vec4fV1));
            attachment.trace.pose_hash = hash;
            // A state machine already published its object's frame; the sink
            // only records that this graph declared the writer.
            if (attachment.evaluator_writes) break;
            PoseViewV1 model_pose{};
            if (const auto status = acquire(attachment.layout, attachment.joint_count, model_pose);
                status != Status::ok)
                return status;
            std::vector<Matrix4fV1> matrices(attachment.joint_count);
            auto local_to_model = descriptor<LocalToModelDescV1>();
            local_to_model.rig = attachment.rig;
            local_to_model.local_pose = input.view.pose;
            local_to_model.model_pose = model_pose.pose;
            local_to_model.model_matrices = matrices.data();
            local_to_model.model_matrix_capacity = static_cast<std::uint32_t>(matrices.size());
            if (const auto status = service.local_to_model(service.context, &local_to_model);
                status != Status::ok)
                return status;
            std::vector<Matrix4fV1> palette(attachment.palette_count);
            auto build = descriptor<BuildSkinPaletteDescV1>();
            build.skin_binding = attachment.skin_binding;
            build.model_matrices = matrices.data();
            build.model_matrix_count = local_to_model.model_matrix_count;
            build.palette = palette.data();
            build.palette_capacity = static_cast<std::uint32_t>(palette.size());
            if (const auto status = service.build_skin_palette(service.context, &build); status != Status::ok)
                return status;
            auto publish = descriptor<PublishAnimationFrameFromSourceDescV1>();
            publish.source = attachment.source;
            publish.frame = descriptor<PublishAnimationFrameDescV1>();
            publish.frame.instance = attachment.instance;
            publish.frame.local_pose = input.view.pose;
            publish.frame.model_pose = model_pose.pose;
            publish.frame.palette = palette.data();
            publish.frame.palette_count = static_cast<std::uint32_t>(palette.size());
            publish.frame.frame_revision = attachment.pending_revision;
            publish.frame.root_delta.rotation.w = 1.0f;
            if (const auto status =
                    service.publish_animation_frame_from_source(service.context, &publish);
                status != Status::ok)
                return status;
            break;
        }
        }
    }
    return Status::ok;
}

PoseGraphRuntime::PoseGraphRuntime() : impl_(std::make_unique<Impl>()) {}
PoseGraphRuntime::~PoseGraphRuntime() { clear(); }

void PoseGraphRuntime::clear() noexcept {
    for (auto &[graph, list] : impl_->attachments) {
        (void)graph;
        for (auto &attachment : list) impl_->unbind(*attachment);
    }
    impl_->attachments.clear();
    impl_->published.clear();
}

void PoseGraphRuntime::retireUnreferenced() noexcept {
    for (auto entry = impl_->published.begin(); entry != impl_->published.end();) {
        if (!entry->second.expired()) {
            ++entry;
            continue;
        }
        const auto found = impl_->attachments.find(entry->first);
        if (found != impl_->attachments.end()) {
            for (auto &attachment : found->second) impl_->unbind(*attachment);
            impl_->attachments.erase(found);
        }
        entry = impl_->published.erase(entry);
    }
}

void PoseGraphRuntime::synchronize(const std::shared_ptr<const PoseGraph> &graph,
                                   const PoseGraphFrame &frame) {
    retireUnreferenced();
    if (!graph || graph->empty()) return;
    impl_->published.insert_or_assign(graph.get(), graph);
    auto &list = impl_->attachments[graph.get()];
    if (list.empty())
        for (const auto sink : graph->sinks()) {
            auto attachment = std::make_unique<Impl::Attachment>();
            attachment->runtime = impl_.get();
            attachment->graph = graph;
            attachment->sink = sink;
            attachment->owner = graph->nodes()[sink].owner;
            attachment->chain = Impl::chainOf(*graph, sink);
            list.push_back(std::move(attachment));
        }
    for (auto &attachment : list) {
        const auto object = frame.object_name(attachment->owner);
        if (!object) {
            if (attachment->bound) impl_->unbind(*attachment);
            attachment->object_name.clear();
            continue;
        }
        // An animation component already writing this object is the same
        // conflict as a second sink; AG-1b removes the component path.
        if (frame.component_writer && frame.component_writer(attachment->owner))
            PoseGraph::raiseSinkConflict(attachment->owner, "animation component",
                                         graph->nodes()[attachment->sink].id);
        if (attachment->bound && attachment->object_name != *object) impl_->unbind(*attachment);
        attachment->object_name = *object;
        if (!attachment->bound) impl_->bind(*attachment, frame);
        if (attachment->failed) {
            attachment->failed = false;
            impl_->unbind(*attachment);
            throw std::runtime_error("pose graph evaluation failed for " + attachment->object_name);
        }
        attachment->trace = {};
        attachment->time = frame.time;
        attachment->delta_seconds = frame.delta_seconds;
        attachment->scalars = frame.scalars;
        attachment->pending_revision = frame.frame_revision;
        attachment->tick_pending = true;
        for (auto &[node, binding] : attachment->bindings) {
            if (!binding.evaluator) continue;
            const auto &compiled = graph->nodes()[node];
            for (const auto &parameter : compiled.parameter_ports) {
                const auto port = compiled.scalar_inputs.find(parameter);
                if (port == compiled.scalar_inputs.end()) continue;
                const auto value = frame.scalars.find(port->second);
                if (value == frame.scalars.end()) continue;
                if (const auto status = binding.evaluator->setParameter(parameter, value->second);
                    status != Status::ok)
                    throw std::runtime_error("pose.state_machine parameter rejected: " + parameter);
            }
            if (attachment->time_set_revision != frame.time_set_revision) {
                attachment->time_set_revision = frame.time_set_revision;
                (void)binding.evaluator->notify(AnimationNotificationKind::set_time, frame.time);
            }
            if (const auto status = binding.evaluator->prepareTick(frame.time, frame.delta_seconds,
                                                                   frame.frame_revision);
                status != Status::ok)
                throw std::runtime_error("pose.state_machine tick rejected: " + compiled.id);
        }
    }
}

std::vector<PoseGraphRuntime::SinkTrace> PoseGraphRuntime::trace() const {
    std::vector<SinkTrace> result;
    for (const auto &[graph, list] : impl_->attachments) {
        (void)graph;
        for (const auto &attachment : list)
            if (attachment->bound) result.push_back(attachment->trace);
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.sink < right.sink;
    });
    return result;
}

PoseGraphRuntime &poseGraphRuntime() {
    static PoseGraphRuntime runtime;
    return runtime;
}

} // namespace Pelican::Animation
