#include "../watch/userdataguard.hpp"
#include "../userpublic/userdataerror.hpp"
#include "posegraphruntime.hpp"

#include "../userpublic/animation/abi_v1.hpp"
#include "../userpublic/animation/animgraph.hpp"
#include "../userpublic/animation/pose_staging_v1.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <set>
#include <stdexcept>
#include <utility>
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

// Keep the registration contract's code/detail while retaining the object
// context used by frame and RPC diagnostics. This is never a UserDataError.
class PoseGraphContractFailure final : public GraphRegistryError {
    std::string message_;
  public:
    PoseGraphContractFailure(const GraphRegistryError &error, const std::string &context)
        : GraphRegistryError(error), message_(context + ": " + error.what()) {}
    const char *what() const noexcept override { return message_.c_str(); }
};

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
        // AG-2 root motion, on the clip_sample nodes that asked for it.
        bool root_motion = false;
        // The layout index of the node the document called the root, resolved
        // once at attach: it is a property of the rig, not of a frame.
        std::uint32_t root_layout_node = 0;
        // The clip's own root translation at its first and last key. The span
        // between them is what one loop of the clip advances by; the first key
        // is what the pose is pinned to, so the mesh stops sliding inside the
        // instance. Both are clip constants, sampled on the first evaluation
        // and kept.
        bool root_samples_ready = false;
        std::array<double, 3> root_start{};
        std::array<double, 3> root_span{};
        // R(u) as of the previous frame. Absent on the first frame and after a
        // seek: a set_time is a cut, not a movement, so the frame that observes
        // it contributes no displacement.
        bool has_root_previous = false;
        std::array<double, 3> root_previous{};
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
        // What the phase callback caught, kept whole. An exception cannot cross
        // the C callback boundary, so the exception object itself waits here
        // for the owner thread instead of collapsing into a bool that loses
        // which port was mis-wired.
        std::exception_ptr failure;
        UserDataScope data_scope;
        // One evaluation per frame revision: the phase refuses a revision it
        // was not prepared for instead of running twice.
        bool tick_pending = false;
        std::uint64_t pending_revision = 0;
        std::uint64_t time_set_revision = 0;
        double time = 0.0;
        double delta_seconds = 0.0;
        std::map<std::string, double, std::less<>> scalars;
        SinkTrace trace;
        // AG-2. True when some node in this chain extracts root motion; then
        // the object token is resolved at attach and the applier is the one the
        // owner thread handed over. The displacement is summed across the chain
        // and written once, just before the sink publishes.
        bool root_motion = false;
        std::uint64_t root_object = 0;
        bool (*apply_root_translation)(std::uint64_t, const double *) = nullptr;
        std::array<double, 3> root_delta{};
        // Dropped for every node in this chain on the frame a set_time lands.
        bool drop_root_previous = false;
    };

    // A wrapped state machine whose attachment retired, waiting for the
    // attachment that replaces it. It is still bound -- that is the point: it
    // keeps its state and its claim on the object until either the next
    // generation adopts it or the next retirement pass releases it.
    struct Donor {
        GraphInstanceOwner owner;
        std::string node;
        Json document;
        std::unique_ptr<AnimationGraph::EvaluatorV1> evaluator;
    };

    std::map<const PoseGraph *, std::vector<std::unique_ptr<Attachment>>> attachments;
    std::map<const PoseGraph *, std::weak_ptr<const PoseGraph>> published;
    std::vector<Donor> donors;

    // The evaluator kept aside for this owner, if any. Callers either adopt it
    // or erase it; a donor that stays behind keeps claiming its object.
    std::vector<Donor>::iterator donorFor(const GraphInstanceOwner &owner) {
        return std::find_if(donors.begin(), donors.end(), [&](const Donor &donor) {
            return donor.owner.scene == owner.scene && donor.owner.object == owner.object;
        });
    }

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
            throw UserDataError("pose graph sink resolution failed for " + object +
                " with animation status " + std::to_string(static_cast<std::uint32_t>(status)));
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
        // The root-motion state lives on the bindings that just went away. A
        // rebind resolves the layout index, the clip's root samples and the
        // object token again, and starts with no previous displacement -- the
        // same restart a parameter edit causes, for the same reason.
        attachment.root_motion = false;
        attachment.root_object = 0;
        attachment.apply_root_translation = nullptr;
        attachment.root_delta = {};
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

    // The owner thread's end of the phase callback. The exception the callback
    // caught is raised again here, with the object the chain was writing put in
    // front of the original message rather than replacing it: `what()` still
    // carries "pose port is unconnected: blend1.b", which is the only text the
    // graph's author can act on. Prefixing rather than nesting because every
    // surface that reports this -- framephase, the RPC status -- reads one
    // `what()` and never walks a nested chain.
    [[noreturn]] void raiseFailure(Attachment &attachment) {
        const auto failure = std::exchange(attachment.failure, {});
        unbind(attachment);
        const auto named = "pose graph evaluation failed for " + attachment.object_name;
        try {
            std::rethrow_exception(failure);
        } catch (const UserDataError &error) {
            throw UserDataError(named + ": " + error.what());
        } catch (const GraphRegistryError &error) {
            throw PoseGraphContractFailure(error, named);
        }
    }

    void bind(Attachment &attachment, const PoseGraphFrame &frame);
    Status evaluate(Attachment &attachment);
    // One evaluation's working set: the pose arena this chain opens on demand
    // and what each node produced. It exists so the five operations below can
    // be five functions instead of five case bodies sharing six stack lambdas.
    struct Produced {
        PoseViewV1 view{};
        PoseLayoutHandle layout{};
    };
    struct Evaluation {
        Attachment &attachment;
        AnimationServiceV1 &service;
        BeginPoseArenaFrameDescV1 begin{};
        bool arena_open = false;
        std::map<std::size_t, Produced> poses;

        // Beginning an arena frame retires what this owner staged earlier in
        // the same phase run, so a chain that only reads a staged pose must
        // never open one. The arena opens on the first operation that actually
        // needs storage.
        Status openArena();
        Status acquire(PoseLayoutHandle layout, std::uint32_t joints, PoseViewV1 &view);
        const Produced &poseInput(const PoseGraphNode &node, const char *port) const;
        double scalar(const PoseGraphNode &node, const char *port, double fallback) const;
        static void requireLayout(const PoseGraphNode &node, const char *port,
                                  PoseLayoutHandle expected, PoseLayoutHandle observed);
    };
    static Status evaluateClipSample(Evaluation &state, const PoseGraphNode &node,
                                     std::size_t index);
    static Status evaluateStateMachine(Evaluation &state, const PoseGraphNode &node,
                                       std::size_t index);
    static Status evaluateBlend(Evaluation &state, const PoseGraphNode &node, std::size_t index);
    static Status evaluateRetarget(Evaluation &state, const PoseGraphNode &node, std::size_t index);
    static Status evaluateSink(Evaluation &state, const PoseGraph &graph,
                               const PoseGraphNode &node);
    // Turns the clip's root translation into a displacement for the object and
    // pins the pose's own root, in that order. `pose` is the pose this node
    // just produced, already sampled at the canonical time; `unwrapped` is the
    // same time before the clip's bounds folded it.
    static Status extractRootMotion(Attachment &attachment, NodeBinding &binding,
                                    const PoseGraphNode &node, PoseViewV1 &pose, double unwrapped);
};

Status PoseGraphRuntime::Impl::extractRootMotion(Attachment &attachment, NodeBinding &binding,
                                                 const PoseGraphNode &node, PoseViewV1 &pose,
                                                 double unwrapped) {
    auto &service = attachment.service;
    const auto begin = binding.metadata.start_seconds;
    const auto end = binding.metadata.end_seconds;
    // bind() refused anything else, so there is a whole loop to divide by.
    const auto duration = end - begin;
    const auto read = [&](std::array<double, 3> &out) {
        const auto &value = pose.translations[binding.root_layout_node];
        out = {static_cast<double>(value.x), static_cast<double>(value.y),
               static_cast<double>(value.z)};
    };
    if (!binding.root_samples_ready) {
        // Two clip constants: where the root starts and how far one whole loop
        // carries it. Sampling them into the pose this node just produced costs
        // no extra arena storage, happens once per binding, and the frame's own
        // sample is put back before anything reads it.
        auto sample = descriptor<SamplePoseAtDescV1>();
        sample.clip = binding.clip;
        sample.output_pose = pose.pose;
        sample.time_seconds = begin;
        if (const auto status = service.sample_pose_at(service.context, &sample); status != Status::ok)
            return status;
        read(binding.root_start);
        std::array<double, 3> last{};
        sample.time_seconds = end;
        if (const auto status = service.sample_pose_at(service.context, &sample); status != Status::ok)
            return status;
        read(last);
        for (std::size_t axis = 0; axis < 3; ++axis)
            binding.root_span[axis] = last[axis] - binding.root_start[axis];
        sample.time_seconds = canonicalTime(unwrapped, begin, end, node.loop);
        if (const auto status = service.sample_pose_at(service.context, &sample); status != Status::ok)
            return status;
        binding.root_samples_ready = true;
    }
    // R(u) = loops(u) * (root(end) - root(start)) + root(canonical(u)). The
    // canonical term is the sample already in `pose`; the loops term is what
    // makes crossing the end of the clip a continuation rather than a jump back
    // to its start, and it is also what makes a negative speed and more than one
    // loop in a frame the same arithmetic. A clip that does not loop is clamped,
    // so it crosses nothing.
    const auto loops = node.loop ? std::floor((unwrapped - begin) / duration) : 0.0;
    std::array<double, 3> current{};
    read(current);
    for (std::size_t axis = 0; axis < 3; ++axis) current[axis] += loops * binding.root_span[axis];
    // A set_time is a cut, not a movement. The frame that observes one starts
    // over from where the clip now is instead of charging the object for the
    // distance between the two times.
    if (attachment.drop_root_previous) binding.has_root_previous = false;
    if (binding.has_root_previous)
        for (std::size_t axis = 0; axis < 3; ++axis)
            attachment.root_delta[axis] += current[axis] - binding.root_previous[axis];
    binding.root_previous = current;
    binding.has_root_previous = true;
    // The pin. The clip's root translation stops sliding the mesh inside the
    // instance -- what the individual shows is the clip's first key -- and the
    // movement leaves through the object's transform instead. Rotation is
    // untouched: extracting a turn is not in v0.
    pose.translations[binding.root_layout_node].x = static_cast<float>(binding.root_start[0]);
    pose.translations[binding.root_layout_node].y = static_cast<float>(binding.root_start[1]);
    pose.translations[binding.root_layout_node].z = static_cast<float>(binding.root_start[2]);
    return Status::ok;
}

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
        // A named pose failure cannot cross the C callback boundary, so the
        // exception is kept and raised again by the owner thread -- from
        // rethrowPendingFailure() on this same frame, or from the next
        // synchronize. Both run outside a phase.
        attachment.failure = std::current_exception();
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
            throw UserDataError("pose operation owner is not live: " + compiled.id);
        if (frame.verify_rig) frame.verify_rig(compiled.owner, compiled.rig, compiled.id);
        SkinBindingHandle unused_binding{};
        std::uint32_t unused_palette = 0;
        resolveObject(attachment.service, *object, AnimationSinkKind::skeletal_pose, binding.sink,
                      binding.instance, binding.rig, binding.layout, unused_binding,
                      binding.joint_count, unused_palette);
        if (compiled.op == PoseGraphOp::state_machine) {
            // The wrapped evaluator is the writer for its own object; the graph
            // declares that intent with the sink and claims nothing itself.
            // An evaluator kept aside from the previous generation is still
            // bound and still claims this object, so it is re-resolved rather
            // than bound again: rebind() keeps the state, the clip clocks and
            // the transition it was in the middle of. A donor whose node id or
            // wrapped document no longer matches is a different machine -- it
            // is released here, before the fresh one claims the same object.
            const auto donor = donorFor(compiled.owner);
            const bool adopt = donor != donors.end() && donor->node == compiled.id &&
                               donor->document == compiled.state_machine;
            if (adopt) {
                binding.evaluator = std::move(donor->evaluator);
                donors.erase(donor);
                if (const auto status = binding.evaluator->rebind(); status != Status::ok)
                    throw UserDataError("pose.state_machine rebind failed for " + compiled.id +
                                             ": " + binding.evaluator->lastError());
            } else {
                if (donor != donors.end()) donors.erase(donor);
                binding.evaluator = std::make_unique<AnimationGraph::EvaluatorV1>(
                    AnimationGraph::parseDocumentV1(compiled.state_machine.dump()), *object);
                if (const auto status = binding.evaluator->bind(); status != Status::ok)
                    throw UserDataError("pose.state_machine bind failed for " + compiled.id + ": " +
                                             binding.evaluator->lastError());
            }
            attachment.evaluator_writes = true;
            continue;
        }
        auto resolve = descriptor<ResolveAnimationClipDescV1>();
        resolve.rig = binding.rig;
        resolve.clip_name = compiled.clip.data();
        resolve.clip_name_size = static_cast<std::uint32_t>(compiled.clip.size());
        if (const auto status = attachment.service.resolve_clip(attachment.service.context, &resolve);
            status != Status::ok)
            throw UserDataError("pose graph clip resolution failed: " + compiled.clip);
        binding.clip = resolve.clip;
        binding.metadata = descriptor<ClipMetadataV1>();
        binding.metadata.clip = binding.clip;
        if (const auto status = attachment.service.get_clip_metadata(attachment.service.context, &binding.metadata);
            status != Status::ok)
            statusFailure("clip metadata for " + compiled.clip, status);
        if (compiled.root_motion) {
            // A clip with no duration has no "one loop", so loops(u) has no
            // denominator: R(u) would be an infinity times a zero span. It is a
            // real shape -- a single-keyframe clip -- so it is named rather
            // than divided by.
            if (!(binding.metadata.end_seconds > binding.metadata.start_seconds))
                PoseGraph::raiseRootMotionUnsupported(compiled.id,
                    "root motion needs a clip with a duration",
                    Json{{"clip", compiled.clip}});
            auto staging = descriptor<PoseStagingServiceV1>();
            if (const auto status = getPoseStagingServiceV1(poseStagingServiceVersionV1, &staging);
                status != Status::ok)
                statusFailure("pose staging negotiation for " + compiled.id, status);
            if ((staging.capability_bits & pose_staging_rig_source_node_mapping) == 0 ||
                !staging.resolve_rig_source_node)
                statusFailure("pose staging has no rig source node mapping for " + compiled.id,
                              Status::unsupported_version);
            auto source_node = descriptor<ResolveRigSourceNodeDescV1>();
            source_node.rig = binding.rig;
            source_node.clip = binding.clip;
            source_node.source_node_index = compiled.root_node;
            // Out of range and absent from the layout arrive here as the same
            // not_found, and they are the same thing to the author.
            if (const auto status =
                    staging.resolve_rig_source_node(staging.context, &source_node);
                status != Status::ok)
                PoseGraph::raiseRootNodeUnknown(compiled.id, compiled.root_node);
            // The root is what the clip translates. A clip that never
            // translates the node the document called the root would extract a
            // delta that is permanently zero, which reads as "root motion did
            // nothing" rather than "this clip has no root motion in it".
            if ((source_node.channel_kind_mask &
                 (1u << static_cast<std::uint32_t>(ChannelKind::translation))) == 0)
                PoseGraph::raiseRootMotionUnsupported(compiled.id,
                    "the clip has no translation channel on the root node",
                    Json{{"clip", compiled.clip}, {"root_node", compiled.root_node}});
            binding.root_motion = true;
            binding.root_layout_node = source_node.layout_node_index;
            attachment.root_motion = true;
        }
        if (!(binding.metadata.end_seconds > binding.metadata.start_seconds))
            throw UserDataError("pose.clip_sample needs a nonzero duration: " + compiled.id);
    }
    if (attachment.root_motion) {
        // The displacement lands on the object this sink writes -- compile
        // already made sure exactly one sink can reach the extracting node --
        // and the token is resolved here, once, because the callback that
        // resolves it belongs to the owner thread's frame and the write happens
        // in the phase.
        const auto object = frame.root_transform_object
            ? frame.root_transform_object(attachment.owner) : std::optional<std::uint64_t>{};
        if (!object || !frame.apply_root_translation)
            PoseGraph::raiseRootMotionNoTransform(attachment.owner,
                graph.nodes()[attachment.sink].id, attachment.object_name);
        attachment.root_object = *object;
        attachment.apply_root_translation = frame.apply_root_translation;
    }

    const auto &sink_node = graph.nodes()[attachment.sink];
    if (!attachment.evaluator_writes) {
        // Nothing in this chain adopted the previous generation's writer for
        // this object, and this attachment is about to become that writer:
        // release it rather than claim against it.
        if (const auto stale = donorFor(attachment.owner); stale != donors.end()) donors.erase(stale);
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

Status PoseGraphRuntime::Impl::Evaluation::openArena() {
    if (arena_open) return Status::ok;
    begin.owner = attachment.service_owner;
    begin.frame_revision = attachment.pending_revision;
    const auto status = service.begin_pose_frame(service.context, &begin);
    arena_open = status == Status::ok;
    return status;
}

Status PoseGraphRuntime::Impl::Evaluation::acquire(PoseLayoutHandle layout, std::uint32_t joints,
                                                   PoseViewV1 &view) {
    if (const auto status = openArena(); status != Status::ok) return status;
    view = descriptor<PoseViewV1>();
    auto request = descriptor<AcquirePoseDescV1>();
    request.arena = begin.arena;
    request.layout = layout;
    request.joint_count = joints;
    request.out_view = &view;
    return service.acquire_pose(service.context, &request);
}

const PoseGraphRuntime::Impl::Produced &
PoseGraphRuntime::Impl::Evaluation::poseInput(const PoseGraphNode &node, const char *port) const {
    const auto found = node.pose_inputs.find(port);
    if (found == node.pose_inputs.end())
        throw UserDataError(std::string{"pose port is unconnected: "} + node.id + "." + port);
    return poses.at(found->second);
}

double PoseGraphRuntime::Impl::Evaluation::scalar(const PoseGraphNode &node, const char *port,
                                                  double fallback) const {
    const auto found = node.scalar_inputs.find(port);
    if (found == node.scalar_inputs.end()) return fallback;
    const auto value = attachment.scalars.find(found->second);
    if (value == attachment.scalars.end())
        throw std::runtime_error("pose scalar input was not evaluated this frame: " + found->second);
    return value->second;
}

void PoseGraphRuntime::Impl::Evaluation::requireLayout(const PoseGraphNode &node, const char *port,
                                                       PoseLayoutHandle expected,
                                                       PoseLayoutHandle observed) {
    if (expected.identity == observed.identity && expected.generation == observed.generation) return;
    PoseGraph::raiseLayoutMismatch(node.id, port,
        Json{{"identity", expected.identity}, {"generation", expected.generation}},
        Json{{"identity", observed.identity}, {"generation", observed.generation}});
}

Status PoseGraphRuntime::Impl::evaluateClipSample(Evaluation &state, const PoseGraphNode &node,
                                                  std::size_t index) {
    auto &attachment = state.attachment;
    auto &service = state.service;
    auto &binding = attachment.bindings.at(index);
    Produced produced;
    produced.layout = binding.layout;
    if (const auto status = state.acquire(binding.layout, binding.joint_count, produced.view);
        status != Status::ok)
        return status;
    auto sample = descriptor<SamplePoseAtDescV1>();
    sample.clip = binding.clip;
    // The same arithmetic the animation component runs (sampleTime in
    // animationjobs.cpp): shift by start_time, scale by speed, then wrap
    // or clamp inside the clip's own [start, end]. The result is the
    // absolute clip time, which is what sample_pose_at now takes -- the
    // service no longer wraps a second time, so a clip whose keyframes
    // do not begin at 0 keeps its phase.
    // The node's own time map, before the clip's bounds fold it: seek,
    // wrap and a negative speed are three readings of the same number.
    const auto unwrapped = (state.scalar(node, "time", attachment.time) - node.start_time) *
                               node.speed + binding.metadata.start_seconds;
    sample.time_seconds = canonicalTime(unwrapped, binding.metadata.start_seconds,
                                        binding.metadata.end_seconds, node.loop);
    sample.output_pose = produced.view.pose;
    if (const auto status = service.sample_pose_at(service.context, &sample); status != Status::ok)
        return status;
    if (binding.root_motion) {
        if (const auto status = extractRootMotion(attachment, binding, node, produced.view,
                                                  unwrapped);
            status != Status::ok)
            return status;
    }
    // AG-3a. A clip_sample node keeps no cursor -- the service samples
    // an absolute time and forgets it -- so where the clip stood is
    // only observable if the frame that computed it writes it down.
    // The phase is measured inside the clip's own bounds, which is the
    // same span canonicalTime folded the time into just above.
    {
        const auto span = binding.metadata.end_seconds - binding.metadata.start_seconds;
        attachment.trace.cursors.push_back(CursorTrace{node.clip,
            sample.time_seconds,
            span > 0.0 ? (sample.time_seconds - binding.metadata.start_seconds) / span : 0.0});
    }
    state.poses.emplace(index, produced);
    return Status::ok;
}

Status PoseGraphRuntime::Impl::evaluateStateMachine(Evaluation &state, const PoseGraphNode &node,
                                                    std::size_t index) {
    (void)node;
    auto &attachment = state.attachment;
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
    state.poses.emplace(index, produced);
    // The deterministic record of what the evaluator did this frame.
    // AG-3a takes the rest of the same snapshot: a wrapped machine
    // already keeps its cursors and its transition, so nothing here is
    // recomputed -- it is copied out of the one status read.
    {
        const auto status = binding.evaluator->getStatus();
        attachment.trace.state = status.current_state;
        attachment.trace.source = SinkSource::state_machine;
        attachment.trace.transition_active = status.transition_active;
        attachment.trace.transition_target = status.transition_target;
        attachment.trace.transition_progress = status.transition_progress;
        for (const auto &cursor : status.cursors)
            attachment.trace.cursors.push_back(CursorTrace{
                cursor.clip, cursor.time_seconds, cursor.normalized_phase});
    }
    return Status::ok;
}

Status PoseGraphRuntime::Impl::evaluateBlend(Evaluation &state, const PoseGraphNode &node,
                                             std::size_t index) {
    auto &attachment = state.attachment;
    auto &service = state.service;
    const auto &a = state.poseInput(node, "a");
    const auto &b = state.poseInput(node, "b");
    Evaluation::requireLayout(node, "b", a.layout, b.layout);
    Produced produced;
    produced.layout = a.layout;
    if (const auto status = state.acquire(a.layout, a.view.joint_count, produced.view);
        status != Status::ok)
        return status;
    const auto weight = static_cast<float>(std::clamp(state.scalar(node, "weight", 0.0), 0.0, 1.0));
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
    state.poses.emplace(index, produced);
    return Status::ok;
}

Status PoseGraphRuntime::Impl::evaluateRetarget(Evaluation &state, const PoseGraphNode &node,
                                                std::size_t index) {
    // R0 applies a VRMA profile when the clip is baked, so at pose level
    // the node asserts that the pose it received belongs to the layout
    // the profile targets and hands the same staged pose on.
    const auto &input = state.poseInput(node, "pose");
    Evaluation::requireLayout(node, "pose", state.attachment.layout, input.layout);
    state.poses.emplace(index, input);
    return Status::ok;
}

Status PoseGraphRuntime::Impl::evaluateSink(Evaluation &state, const PoseGraph &graph,
                                            const PoseGraphNode &node) {
    auto &attachment = state.attachment;
    auto &service = state.service;
    const auto &input = state.poseInput(node, "pose");
    Evaluation::requireLayout(node, "pose", attachment.layout, input.layout);
    attachment.trace.sink = node.id;
    attachment.trace.node = graph.nodes()[node.pose_inputs.at("pose")].id;
    // AG-3a. Who this sink writes, and what AG-2 is about to hand the
    // object. Recorded before the transfer below, so a frame whose
    // apply fails still says what it was going to move.
    attachment.trace.owner = attachment.owner;
    attachment.trace.root_motion = attachment.root_motion;
    attachment.trace.root_delta = attachment.root_delta;
    attachment.trace.layout_identity = input.layout.identity;
    attachment.trace.layout_generation = input.layout.generation;
    attachment.trace.frame_revision = attachment.pending_revision;
    auto hash = 1469598103934665603ull;
    hash = fnv1a(hash, input.view.translations, input.view.joint_count * sizeof(Vec4fV1));
    hash = fnv1a(hash, input.view.rotations, input.view.joint_count * sizeof(QuatfV1));
    hash = fnv1a(hash, input.view.scales, input.view.joint_count * sizeof(Vec4fV1));
    attachment.trace.pose_hash = hash;
    // AG-2: animation, then root motion, then the publish -- all three
    // in this one function, so the order cannot drift. The individual
    // moves by what the clip's root advanced this frame; the pose that
    // is about to be published has already had that root pinned, so the
    // two do not both move it. There is no movement or physics step in
    // the engine to place between them yet.
    if (attachment.root_motion) {
        if (!attachment.apply_root_translation ||
            !attachment.apply_root_translation(attachment.root_object,
                                               attachment.root_delta.data()))
            PoseGraph::raiseRootMotionNoTransform(attachment.owner, node.id,
                                                  attachment.object_name);
    }
    // A state machine already published its object's frame; the sink
    // only records that this graph declared the writer.
    if (attachment.evaluator_writes) return Status::ok;
    PoseViewV1 model_pose{};
    if (const auto status = state.acquire(attachment.layout, attachment.joint_count, model_pose);
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
    return Status::ok;
}

Status PoseGraphRuntime::Impl::evaluate(Attachment &attachment) {
    const auto &graph = *attachment.graph;
    attachment.root_delta = {};
    Evaluation state{attachment, attachment.service};
    state.begin = descriptor<BeginPoseArenaFrameDescV1>();
    for (const auto index : attachment.chain) {
        const auto &node = graph.nodes()[index];
        auto status = Status::ok;
        switch (node.op) {
        case PoseGraphOp::clip_sample:
            status = evaluateClipSample(state, node, index);
            break;
        case PoseGraphOp::state_machine:
            status = evaluateStateMachine(state, node, index);
            break;
        case PoseGraphOp::blend:
            status = evaluateBlend(state, node, index);
            break;
        case PoseGraphOp::retarget:
            status = evaluateRetarget(state, node, index);
            break;
        case PoseGraphOp::skeletal_sink:
        case PoseGraphOp::transform_sink:
            status = evaluateSink(state, graph, node);
            break;
        }
        if (status != Status::ok) return status;
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
    impl_->donors.clear();
}

void PoseGraphRuntime::rethrowPendingFailure() {
    for (auto &[graph, list] : impl_->attachments) {
        (void)graph;
        for (auto &attachment : list)
            if (attachment->failure) impl_->raiseFailure(*attachment);
    }
}

bool PoseGraphRuntime::recoverPendingUserDataFailures() {
    bool recovered = false;
    for (auto &[graph, list] : impl_->attachments)
        for (auto &attachment : list)
            if (attachment->failure) {
                // Inspect the saved exception before any failed-scope skip.
                // Contract refusals and unmarked engine failures must escape.
                try {
                    impl_->raiseFailure(*attachment);
                } catch (const UserDataError &error) {
                    failUserData(attachment->data_scope, error.what());
                    recovered = true;
                }
            }
    return recovered;
}

void PoseGraphRuntime::retainOnly(const std::set<const PoseGraph *> &live) noexcept {
    // Anything the previous pass set aside and nobody adopted belongs to a
    // generation that is gone for good. Releasing it here, before this pass
    // harvests, bounds the wait at exactly one pass.
    impl_->donors.clear();
    for (auto entry = impl_->attachments.begin(); entry != impl_->attachments.end();) {
        if (live.contains(entry->first)) {
            ++entry;
            continue;
        }
        for (auto &attachment : entry->second) {
            // Take the wrapped state machines out before unbinding: they stay
            // bound, keeping their state and their claim, so the attachment
            // that replaces them can adopt rather than start over. unbind()
            // would release exactly what is worth keeping.
            for (auto &[node, binding] : attachment->bindings) {
                if (!binding.evaluator) continue;
                const auto &compiled = attachment->graph->nodes()[node];
                impl_->donors.push_back(Impl::Donor{compiled.owner, compiled.id,
                                                    compiled.state_machine,
                                                    std::move(binding.evaluator)});
            }
            impl_->unbind(*attachment);
        }
        impl_->published.erase(entry->first);
        entry = impl_->attachments.erase(entry);
    }
    if (FastModuleContainer::tryGet<ProblemList>()) {
        std::set<std::string> live_scopes;
        for (const auto &[graph, list] : impl_->attachments)
            for (const auto &attachment : list)
                if (!attachment->data_scope.key.empty()) live_scopes.insert(attachment->data_scope.key);
        retainUserDataScopes("pose_graph/", live_scopes);
    }

}

void PoseGraphRuntime::synchronize(const std::shared_ptr<const PoseGraph> &graph,
                                   const PoseGraphFrame &frame) {
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
        attachment->data_scope = userDataScope("pose_graph", graph->nodes()[attachment->sink].id,
            invalidGameObjectId, std::to_string(reinterpret_cast<std::uintptr_t>(graph.get())));
        attachment->data_scope.fields["object"] = attachment->owner.object;
        attachment->data_scope.fields["scene_id"] = attachment->owner.scene;
        if (!guardUserData(attachment->data_scope, [&] {
        const auto object = frame.object_name(attachment->owner);
        if (!object) {
            if (attachment->bound) impl_->unbind(*attachment);
            attachment->object_name.clear();
            return;
        }
        // The animation component's clip path already published this object's
        // frame this revision. That is the same conflict as a second sink: two
        // ways of attaching a pose never share one owner.
        if (frame.component_writer && frame.component_writer(attachment->owner))
            PoseGraph::raiseSinkConflict(attachment->owner, "animation component",
                                         graph->nodes()[attachment->sink].id);
        if (attachment->bound && attachment->object_name != *object) impl_->unbind(*attachment);
        attachment->object_name = *object;
        if (!attachment->bound) impl_->bind(*attachment, frame);
        if (attachment->failure) impl_->raiseFailure(*attachment);
        attachment->trace = {};
        attachment->time = frame.time;
        attachment->delta_seconds = frame.delta_seconds;
        attachment->scalars = frame.scalars;
        attachment->pending_revision = frame.frame_revision;
        attachment->tick_pending = true;
        // Whether the clock was set since this attachment last looked is a
        // property of the attachment, not of the nodes inside it. Reading and
        // writing it in the loop below would leave an attachment with no
        // wrapped state machine -- every clip_sample chain -- at revision 0
        // forever, so the first set_time would make every later frame look like
        // a fresh seek and root motion would stop for good.
        const bool time_was_set = attachment->time_set_revision != frame.time_set_revision;
        attachment->time_set_revision = frame.time_set_revision;
        attachment->drop_root_previous = time_was_set;
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
                    throw UserDataError("pose.state_machine parameter rejected: " + parameter);
            }
            if (time_was_set)
                (void)binding.evaluator->notify(AnimationNotificationKind::set_time, frame.time);
            if (const auto status = binding.evaluator->prepareTick(frame.time, frame.delta_seconds,
                                                                   frame.frame_revision);
                status != Status::ok)
                throw UserDataError("pose.state_machine tick rejected: " + compiled.id);
        }
        })) impl_->unbind(*attachment);
    }
}

std::string_view PoseGraphRuntime::sinkSourceName(SinkSource source) noexcept {
    switch (source) {
    case SinkSource::clip: return "pose_graph_clip";
    case SinkSource::state_machine: return "pose_graph_state_machine";
    }
    return "pose_graph_clip";
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
