#include "animgraph.hpp"
#include "vrm_application_v1.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Pelican::AnimationGraph {
namespace Internal {

// pelican_core is a static archive. The runtime calls this anchor so the
// evaluator object file (and its exported user-space API) is retained in the
// player import library for game DLLs.
void linkAnchor() noexcept {}

} // namespace Internal
namespace {

using namespace Pelican::Animation;
using Json = nlohmann::json;

std::string sha256Hex(const std::uint8_t (&bytes)[32]) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t index = 0; index < 32; ++index) {
        result[index * 2] = digits[bytes[index] >> 4u];
        result[index * 2 + 1] = digits[bytes[index] & 0x0fu];
    }
    return result;
}

template <class T> T descriptor() {
    T value{};
    value.struct_size = sizeof(T);
    value.version = descriptorVersionV1;
    return value;
}

[[noreturn]] void invalid(std::string message) {
    throw std::runtime_error("pelican.anim_graph v1: " + std::move(message));
}

void knownKeys(const Json &value, std::initializer_list<std::string_view> keys, std::string_view context) {
    const std::set<std::string, std::less<>> known(keys.begin(), keys.end());
    for (const auto &[key, _] : value.items()) {
        if (!known.contains(key)) invalid(std::string(context) + " has unknown key '" + key + "'");
    }
}

double finiteNumber(const Json &value, std::string_view context) {
    if (!value.is_number()) invalid(std::string(context) + " must be a number");
    const auto result = value.get<double>();
    if (!std::isfinite(result)) invalid(std::string(context) + " must be finite");
    return result;
}

std::string requiredString(const Json &value, std::string_view key, std::string_view context) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_string() || found->get_ref<const std::string &>().empty())
        invalid(std::string(context) + " requires non-empty string '" + std::string(key) + "'");
    return found->get<std::string>();
}

ClipNodeV1 parseClip(const Json &value, bool threshold, std::string_view context) {
    if (!value.is_object()) invalid(std::string(context) + " must be an object");
    knownKeys(value, threshold
                         ? std::initializer_list<std::string_view>{"clip", "speed", "start_offset", "loop", "threshold"}
                         : std::initializer_list<std::string_view>{"name", "type", "clip", "speed", "start_offset", "loop"},
              context);
    ClipNodeV1 result;
    result.clip = requiredString(value, "clip", context);
    if (const auto it = value.find("speed"); it != value.end()) result.speed = finiteNumber(*it, "clip speed");
    if (const auto it = value.find("start_offset"); it != value.end())
        result.start_offset = finiteNumber(*it, "clip start_offset");
    if (const auto it = value.find("loop"); it != value.end()) {
        if (!it->is_boolean()) invalid("clip loop must be boolean");
        result.loop = it->get<bool>();
    }
    if (threshold) {
        const auto it = value.find("threshold");
        if (it == value.end()) invalid(std::string(context) + " requires threshold");
        result.threshold = finiteNumber(*it, "blend1d threshold");
    }
    return result;
}

std::uint64_t fnv1a(std::uint64_t hash, const void *data, std::size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

double canonicalTime(double unwrapped, double start, double end, bool loop) {
    const auto duration = end - start;
    if (!(duration > 0.0) || !std::isfinite(duration)) return start;
    if (!loop) return std::clamp(unwrapped, start, end);
    auto offset = std::fmod(unwrapped - start, duration);
    if (offset < 0.0) offset += duration;
    auto result = start + offset;
    // Repeat clips use [start,end); guard rounding at the endpoint.
    if (!(result < end)) result = start;
    return result;
}

bool conditionTrue(double actual, const ConditionV1 &condition) {
    if (condition.operation == ">") return actual > condition.value;
    if (condition.operation == ">=") return actual >= condition.value;
    if (condition.operation == "<") return actual < condition.value;
    if (condition.operation == "<=") return actual <= condition.value;
    if (condition.operation == "==") return actual == condition.value;
    return actual != condition.value;
}

} // namespace

DocumentV1 parseDocumentV1(std::string_view json_text) {
    Json root;
    try {
        root = Json::parse(json_text);
    } catch (const Json::exception &error) {
        invalid(std::string("invalid JSON: ") + error.what());
    }
    if (!root.is_object()) invalid("document must be an object");
    for (const auto reserved : {"layers", "events", "graphs", "sync", "trigger"})
        if (root.contains(reserved)) invalid(std::string("reserved v2 key '") + reserved + "' is not allowed");
    knownKeys(root, {"schema", "version", "parameters", "states", "initial_state", "transitions"}, "document");
    if (root.value("schema", std::string{}) != "pelican.anim_graph") invalid("schema must be 'pelican.anim_graph'");
    if (!root.contains("version") || !root.at("version").is_number_integer() || root.at("version").get<int>() != 1)
        invalid("version must be 1");

    DocumentV1 result;
    const auto parameters = root.value("parameters", Json::object());
    if (!parameters.is_object()) invalid("parameters must be a flat object");
    for (const auto &[name, value] : parameters.items()) {
        if (name.empty() || name.find('.') != std::string::npos || value.is_object() || value.is_array())
            invalid("parameter namespace must be single and flat");
        result.parameters.push_back({name, finiteNumber(value, "parameter '" + name + "'")});
    }

    const auto states_it = root.find("states");
    if (states_it == root.end() || !states_it->is_array() || states_it->empty())
        invalid("states must be a non-empty array");
    std::set<std::string> state_names;
    for (std::size_t i = 0; i < states_it->size(); ++i) {
        const auto &value = states_it->at(i);
        if (!value.is_object()) invalid("state must be an object");
        const auto type = requiredString(value, "type", "state");
        StateV1 state;
        state.name = requiredString(value, "name", "state");
        if (!state_names.insert(state.name).second) invalid("duplicate state '" + state.name + "'");
        if (type == "clip") {
            knownKeys(value, {"name", "type", "clip", "speed", "start_offset", "loop"}, "clip state");
            state.kind = StateKind::clip;
            state.clips.push_back(parseClip(value, false, "clip state"));
        } else if (type == "blend1d") {
            knownKeys(value, {"name", "type", "parameter", "clips"}, "blend1d state");
            state.kind = StateKind::blend1d;
            state.parameter = requiredString(value, "parameter", "blend1d state");
            if (!parameters.contains(state.parameter)) invalid("blend1d references unknown parameter '" + state.parameter + "'");
            const auto clips = value.find("clips");
            if (clips == value.end() || !clips->is_array() || clips->empty()) invalid("blend1d clips must be non-empty");
            for (std::size_t c = 0; c < clips->size(); ++c)
                state.clips.push_back(parseClip(clips->at(c), true, "blend1d clip"));
            std::stable_sort(state.clips.begin(), state.clips.end(), [](const auto &a, const auto &b) {
                return a.threshold < b.threshold;
            });
            for (std::size_t c = 1; c < state.clips.size(); ++c)
                if (state.clips[c - 1].threshold == state.clips[c].threshold)
                    invalid("blend1d thresholds must be unique");
        } else {
            invalid("state type must be 'clip' or 'blend1d'");
        }
        result.states.push_back(std::move(state));
    }

    result.initial_state = requiredString(root, "initial_state", "document");
    if (!state_names.contains(result.initial_state)) invalid("initial_state is unknown");
    const auto transitions = root.value("transitions", Json::array());
    if (!transitions.is_array()) invalid("transitions must be an array");
    for (std::size_t i = 0; i < transitions.size(); ++i) {
        const auto &value = transitions.at(i);
        if (!value.is_object()) invalid("transition must be an object");
        knownKeys(value, {"from", "to", "priority", "interrupt", "duration", "conditions"}, "transition");
        TransitionV1 transition;
        transition.from = requiredString(value, "from", "transition");
        transition.to = requiredString(value, "to", "transition");
        if (transition.from != "*" && !state_names.contains(transition.from)) invalid("transition from state is unknown");
        if (!state_names.contains(transition.to)) invalid("transition to state is unknown");
        if (const auto it = value.find("priority"); it != value.end()) {
            if (!it->is_number_integer()) invalid("transition priority must be integer");
            const auto priority = it->get<std::int64_t>();
            if (priority < std::numeric_limits<std::int32_t>::min() || priority > std::numeric_limits<std::int32_t>::max())
                invalid("transition priority is out of range");
            transition.priority = static_cast<std::int32_t>(priority);
        }
        const auto interrupt = value.value("interrupt", std::string{"never"});
        if (interrupt == "never") transition.interrupt = InterruptMode::never;
        else if (interrupt == "higher_priority") transition.interrupt = InterruptMode::higher_priority;
        else if (interrupt == "always") transition.interrupt = InterruptMode::always;
        else invalid("transition interrupt is invalid");
        if (const auto it = value.find("duration"); it != value.end())
            transition.duration = finiteNumber(*it, "transition duration");
        if (transition.duration < 0.0) invalid("transition duration must be non-negative");
        const auto conditions = value.value("conditions", Json::array());
        if (!conditions.is_array()) invalid("transition conditions must be an array");
        for (const auto &condition_value : conditions) {
            if (!condition_value.is_object()) invalid("transition condition must be an object");
            knownKeys(condition_value, {"parameter", "op", "value"}, "condition");
            ConditionV1 condition;
            condition.parameter = requiredString(condition_value, "parameter", "condition");
            condition.operation = requiredString(condition_value, "op", "condition");
            if (!parameters.contains(condition.parameter)) invalid("condition references unknown parameter");
            if (condition.operation != ">" && condition.operation != ">=" && condition.operation != "<" &&
                condition.operation != "<=" && condition.operation != "==" && condition.operation != "!=")
                invalid("condition op is invalid");
            const auto number = condition_value.find("value");
            if (number == condition_value.end()) invalid("condition requires value");
            condition.value = finiteNumber(*number, "condition value");
            transition.conditions.push_back(std::move(condition));
        }
        transition.declaration_index = static_cast<std::uint32_t>(i);
        result.transitions.push_back(std::move(transition));
    }
    return result;
}

struct EvaluatorV1::Impl {
    struct RuntimeClip {
        ClipNodeV1 node;
        ClipHandle handle{};
        ClipMetadataV1 metadata{};
        CursorHandle cursor{};
        double time{};
    };
    struct RuntimeState {
        const StateV1 *definition{};
        std::vector<RuntimeClip> clips;
        double phase{};
    };
    struct PoseBytes {
        std::vector<Vec4fV1> translations;
        std::vector<QuatfV1> rotations;
        std::vector<Vec4fV1> scales;
        PoseLayoutHandle layout{};
        std::uint64_t revision{};
        std::uint64_t hash{};
        bool valid{};
    };
    struct ActiveTransition {
        const TransitionV1 *definition{};
        std::size_t target{};
        double elapsed{};
        PoseBytes snapshot;
        bool source_is_snapshot{};
        std::size_t source{};
    };
    struct Forced {
        std::size_t target{};
        std::uint64_t sequence{};
    };
    struct TypedIdentity {
        std::uint64_t asset_identity{};
        std::uint32_t asset_generation{};
        std::uint32_t profile_version{};
    };
    struct TypedSideband {
        std::map<std::string, float, std::less<>> expressions;
        QuatfV1 gaze{0.0f, 0.0f, 0.0f, 1.0f};
        bool gaze_present{};
        bool typed_source{};
        TypedIdentity identity;
    };

    DocumentV1 document;
    std::string object_name;
    std::uint32_t source_ordinal{};
    ApiV1 api{};
    AnimationServiceV1 service{};
    Vrm::ApplicationServiceV1 application{};
    AnimationOwnerHandle owner{};
    AnimationSinkHandle sink{};
    AnimationSourceHandle source{};
    InstanceHandle instance{};
    RigHandle rig{};
    PoseLayoutHandle layout{};
    SkinBindingHandle binding{};
    PhaseRegistrationHandle phase_registration{};
    std::uint32_t joint_count{};
    std::uint32_t palette_count{};
    std::vector<RuntimeState> states;
    std::unordered_map<std::string, std::size_t> state_by_name;
    std::unordered_map<std::string, double> parameters;
    std::size_t current{};
    std::optional<ActiveTransition> transition;
    std::vector<Forced> forced;
    std::uint64_t next_force_sequence{1};
    std::uint64_t notification_revision{};
    std::uint64_t source_reset_count{};
    double absolute_time{};
    double dt{};
    std::uint64_t pending_revision{};
    bool tick_pending{};
    bool is_bound{};
    PoseBytes last_pose;
    bool typed_sink_active{};
    TypedIdentity last_typed_identity;
    StatusTraceV1 trace;
    std::string error;

    Impl(DocumentV1 value, std::string object, std::uint32_t ordinal)
        : document(std::move(value)), object_name(std::move(object)), source_ordinal(ordinal) {
        for (const auto &parameter : document.parameters) parameters.emplace(parameter.name, parameter.initial_value);
        for (std::size_t i = 0; i < document.states.size(); ++i) state_by_name.emplace(document.states[i].name, i);
        current = state_by_name.at(document.initial_state);
    }

    ~Impl() { unbind(); }

    void unbind() noexcept {
        if (!is_bound) return;
        if (isValid(phase_registration) && service.unregister_phase) {
            auto request = descriptor<UnregisterAnimationPhaseDescV1>();
            request.owner = owner;
            request.registration = phase_registration;
            (void)service.unregister_phase(service.context, &request);
        }
        for (auto &state : states) for (auto &clip : state.clips) {
            if (!isValid(clip.cursor) || !service.destroy_cursor) continue;
            auto request = descriptor<DestroyClipCursorDescV1>();
            request.owner = owner;
            request.cursor = clip.cursor;
            (void)service.destroy_cursor(service.context, &request);
        }
        if (isValid(source) && service.release_source) {
            auto request = descriptor<ReleaseAnimationSourceDescV1>();
            request.owner = owner;
            request.source = source;
            (void)service.release_source(service.context, &request);
        }
        is_bound = false;
    }

    Status fail(Status status, std::string message) {
        error = std::move(message);
        return status;
    }

    void resetClockState() {
        current = state_by_name.at(document.initial_state);
        for (auto &state : states) state.phase = 0.0;
        transition.reset();
        forced.clear();
        last_pose = {};
        trace = {};
        trace.source_reset_count = source_reset_count;
        trace.authority = AnimationSourceAuthorityV1::graph_apply;
        tick_pending = false;
    }

    Status sendNotification(AnimationNotificationKind kind, double time_seconds,
                            PoseLayoutHandle observed_layout) {
        if (!is_bound) return fail(Status::invalid_handle, "evaluator is not bound");
        if (!std::isfinite(time_seconds) || kind > AnimationNotificationKind::layout_generation_mismatch)
            return fail(Status::invalid_argument, "animation notification is invalid");
        auto request = descriptor<AnimationNotificationDescV1>();
        request.kind = kind;
        request.sink = sink;
        request.source = source;
        request.observed_layout = observed_layout;
        request.time_seconds = time_seconds;
        request.notification_revision = ++notification_revision;
        const auto status = service.notify(service.context, &request);
        if (status != Status::ok) return fail(status, "animation notification failed");
        resetClockState();
        return Status::ok;
    }

    static Status phase(void *user, const AnimationPhaseContextV1 *context) {
        if (!user || !context || context->phase != Phase::base_pose_and_root_modifier)
            return Status::invalid_argument;
        auto &self = *static_cast<Impl *>(user);
        if (context->sink.identity != self.sink.identity ||
            context->sink.generation != self.sink.generation)
            return Status::ok;
        if (!self.tick_pending || context->frame_revision != self.pending_revision)
            return self.fail(Status::invalid_argument, "phase revision does not match prepared tick");
        return self.evaluate();
    }

    Status acquire(PoseArenaHandle arena, PoseViewV1 &view) {
        view = descriptor<PoseViewV1>();
        auto request = descriptor<AcquirePoseDescV1>();
        request.arena = arena;
        request.layout = layout;
        request.joint_count = joint_count;
        request.out_view = &view;
        return service.acquire_pose(service.context, &request);
    }

    PoseBytes capture(const PoseViewV1 &view, std::uint64_t revision) const {
        PoseBytes bytes;
        bytes.translations.assign(view.translations, view.translations + view.joint_count);
        bytes.rotations.assign(view.rotations, view.rotations + view.joint_count);
        bytes.scales.assign(view.scales, view.scales + view.joint_count);
        bytes.layout = view.layout;
        bytes.revision = revision;
        auto hash = 1469598103934665603ull;
        hash = fnv1a(hash, bytes.translations.data(), bytes.translations.size() * sizeof(Vec4fV1));
        hash = fnv1a(hash, bytes.rotations.data(), bytes.rotations.size() * sizeof(QuatfV1));
        hash = fnv1a(hash, bytes.scales.data(), bytes.scales.size() * sizeof(Vec4fV1));
        bytes.hash = hash;
        bytes.valid = true;
        return bytes;
    }

    void restore(const PoseBytes &bytes, PoseViewV1 &view) const {
        std::memcpy(view.translations, bytes.translations.data(), bytes.translations.size() * sizeof(Vec4fV1));
        std::memcpy(view.rotations, bytes.rotations.data(), bytes.rotations.size() * sizeof(QuatfV1));
        std::memcpy(view.scales, bytes.scales.data(), bytes.scales.size() * sizeof(Vec4fV1));
    }

    std::vector<std::pair<std::size_t, float>> weights(const RuntimeState &state) const {
        if (state.definition->kind == StateKind::clip) return {{0, 1.0f}};
        const auto value = parameters.at(state.definition->parameter);
        const auto &clips = state.clips;
        if (value <= clips.front().node.threshold) return {{0, 1.0f}};
        if (value >= clips.back().node.threshold) return {{clips.size() - 1, 1.0f}};
        for (std::size_t i = 1; i < clips.size(); ++i) {
            if (value > clips[i].node.threshold) continue;
            const auto span = clips[i].node.threshold - clips[i - 1].node.threshold;
            const auto upper = static_cast<float>((value - clips[i - 1].node.threshold) / span);
            return {{i - 1, 1.0f - upper}, {i, upper}};
        }
        return {{clips.size() - 1, 1.0f}};
    }

    std::size_t leader(const RuntimeState &state, const std::vector<std::pair<std::size_t, float>> &ws) const {
        // Stable first-in-array tie break.
        return std::max_element(ws.begin(), ws.end(), [](const auto &a, const auto &b) {
            return a.second < b.second;
        })->first;
    }

    static TypedSideband blendTyped(
        const std::vector<std::pair<const TypedSideband *, float>> &inputs) {
        TypedSideband result;
        float dominant_weight = -1.0f;
        bool any_gaze = false;
        for (const auto &[input, weight] : inputs)
            any_gaze = any_gaze ||
                       (input && input->gaze_present && weight > 0.0f);
        QuatfV1 reference{0.0f, 0.0f, 0.0f, 1.0f};
        bool have_reference = false;
        float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 0.0f;
        for (const auto &[input, weight] : inputs) {
            if (!input || !(weight > 0.0f)) continue;
            for (const auto &[name, value] : input->expressions)
                result.expressions[name] += weight * value;
            if (input->typed_source && weight > dominant_weight) {
                dominant_weight = weight;
                result.identity = input->identity;
                result.typed_source = true;
            }
            if (!any_gaze) continue;
            auto value = input->gaze_present
                             ? input->gaze
                             : QuatfV1{0.0f, 0.0f, 0.0f, 1.0f};
            if (!have_reference) {
                reference = value;
                have_reference = true;
            }
            const auto dot = reference.x * value.x + reference.y * value.y +
                             reference.z * value.z + reference.w * value.w;
            const auto sign = dot < 0.0f ? -1.0f : 1.0f;
            qx += weight * sign * value.x;
            qy += weight * sign * value.y;
            qz += weight * sign * value.z;
            qw += weight * sign * value.w;
        }
        if (any_gaze) {
            const auto length = std::sqrt(qx * qx + qy * qy + qz * qz +
                                          qw * qw);
            if (length > 0.0f) {
                result.gaze = {qx / length, qy / length, qz / length,
                               qw / length};
                result.gaze_present = true;
            }
        }
        return result;
    }

    Status ensureApplicationService(
        const std::vector<RuntimeState> &runtime_states) {
        const auto typed = std::any_of(
            runtime_states.begin(), runtime_states.end(), [](const auto &state) {
                return std::any_of(
                    state.clips.begin(), state.clips.end(), [](const auto &clip) {
                        return clip.metadata.typed_channel_flags != 0;
                    });
            });
        if (!typed || application.set_typed_animation_inputs)
            return Status::ok;
        application = Vrm::ApplicationServiceV1{};
        if (const auto status = Vrm::getApplicationServiceV1(
                Vrm::applicationServiceVersionV1, &application);
            status != Status::ok)
            return fail(status, "VRM application service negotiation failed");
        if ((application.capability_bits &
             Vrm::application_service_typed_animation_sink) == 0 ||
            !application.set_typed_animation_inputs)
            return fail(Status::unsupported_version,
                        "VRM typed animation sink is unavailable");
        return Status::ok;
    }

    Status submitTyped(const TypedSideband &sideband) {
        const auto clearing = !sideband.typed_source && typed_sink_active;
        if (!sideband.typed_source && !clearing) return Status::ok;
        if (!application.set_typed_animation_inputs)
            return fail(Status::unsupported_version,
                        "VRM typed animation sink is unavailable");
        const auto identity = sideband.typed_source ? sideband.identity
                                                    : last_typed_identity;
        std::vector<Vrm::ExpressionWeightV1> weights;
        weights.reserve(sideband.expressions.size());
        for (const auto &[name, value] : sideband.expressions) {
            Vrm::ExpressionWeightV1 weight;
            weight.name = name.data();
            weight.name_size = static_cast<std::uint32_t>(name.size());
            weight.value = value;
            weights.push_back(weight);
        }
        Vrm::SetTypedAnimationInputDescV1 request;
        request.instance = instance;
        request.weights = weights.data();
        request.weight_count = static_cast<std::uint32_t>(weights.size());
        request.source_ordinal = source_ordinal;
        request.frame_revision = pending_revision;
        request.asset_identity = identity.asset_identity;
        request.asset_generation = identity.asset_generation;
        request.profile_version = identity.profile_version;
        if (sideband.gaze_present) {
            const auto &q = sideband.gaze;
            const auto forward_x = -2.0f * (q.x * q.z + q.w * q.y);
            const auto forward_y = 2.0f * (q.w * q.x - q.y * q.z);
            const auto forward_z =
                -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
            request.look_at_yaw_degrees =
                std::atan2(-forward_x, -forward_z) * 180.0f /
                3.14159265358979323846f;
            request.look_at_pitch_degrees =
                -std::asin(std::clamp(forward_y, -1.0f, 1.0f)) * 180.0f /
                3.14159265358979323846f;
            request.flags |= Vrm::expression_input_look_at;
        }
        const auto status = application.set_typed_animation_inputs(
            application.context, &request);
        if (status != Status::ok)
            return fail(status, "VRM typed animation input failed");
        typed_sink_active = sideband.typed_source;
        if (sideband.typed_source) last_typed_identity = sideband.identity;
        return Status::ok;
    }

    Status sampleState(std::size_t index, PoseArenaHandle arena, PoseViewV1 &output,
                       std::vector<PoseViewV1> &scratch,
                       TypedSideband &output_sideband) {
        auto &state = states[index];
        const auto ws = weights(state);
        const auto leader_index = leader(state, ws);
        const auto &leader_clip = state.clips[leader_index];
        const auto leader_duration = leader_clip.metadata.end_seconds - leader_clip.metadata.start_seconds;
        if (!(leader_duration > 0.0)) return fail(Status::invalid_argument, "zero-duration clip is not evaluable");
        state.phase += dt * leader_clip.node.speed / leader_duration;
        if (leader_clip.node.loop) {
            state.phase = std::fmod(state.phase, 1.0);
            if (state.phase < 0.0) state.phase += 1.0;
        } else {
            state.phase = std::clamp(state.phase, 0.0, 1.0);
        }

        scratch.resize(ws.size());
        std::vector<BlendLayerV1> layers(ws.size());
        std::vector<TypedSideband> typed_samples(ws.size());
        for (std::size_t i = 0; i < ws.size(); ++i) {
            auto &clip = state.clips[ws[i].first];
            const auto duration = clip.metadata.end_seconds - clip.metadata.start_seconds;
            if (!(duration > 0.0)) return fail(Status::invalid_argument, "zero-duration clip is not evaluable");
            const auto unwrapped = clip.metadata.start_seconds + clip.node.start_offset + state.phase * duration;
            clip.time = canonicalTime(unwrapped, clip.metadata.start_seconds, clip.metadata.end_seconds, clip.node.loop);
            if (const auto status = acquire(arena, scratch[i]); status != Status::ok) return status;
            auto sample = descriptor<SampleAnimationSourceAtDescV1>();
            sample.clip = clip.handle;
            sample.time_seconds = clip.time;
            sample.output_pose = scratch[i].pose;
            std::vector<AnimationExpressionSampleV1> expressions(
                clip.metadata.expression_channel_count);
            sample.expressions = expressions.data();
            sample.expression_capacity =
                static_cast<std::uint32_t>(expressions.size());
            sample.gaze = descriptor<AnimationGazeSampleV1>();
            if (!service.sample_animation_source_at)
                return fail(Status::unsupported_version,
                            "typed AnimationSource sampler is unavailable");
            if (const auto status = service.sample_animation_source_at(
                    service.context, &sample);
                status != Status::ok)
                return status;
            auto &typed = typed_samples[i];
            typed.typed_source = clip.metadata.typed_channel_flags != 0;
            if (typed.typed_source) {
                typed.identity = {
                    clip.metadata.asset_identity,
                    clip.metadata.asset_generation,
                    clip.metadata.profile_version};
            }
            for (std::uint32_t expression = 0;
                 expression < sample.expression_count; ++expression) {
                const auto &value = expressions[expression];
                typed.expressions.emplace(
                    std::string{value.name ? value.name : "", value.name_size},
                    value.weight);
            }
            if (sample.gaze.present != 0) {
                typed.gaze_present = true;
                typed.gaze = sample.gaze.rotation;
            }
            layers[i] = descriptor<BlendLayerV1>();
            layers[i].pose = scratch[i].pose;
            layers[i].weight = ws[i].second;
            layers[i].mode = BlendMode::normal;
            layers[i].additive_space = AdditiveSpace::local;
            layers[i].rest_fallback = RestFallback::use_rest_pose;
            layers[i].root_policy = SidebandPolicy::suppress;
            layers[i].curve_policy = SidebandPolicy::suppress;
            layers[i].event_marker_policy = SidebandPolicy::suppress;
        }
        std::vector<std::pair<const TypedSideband *, float>> typed_inputs;
        typed_inputs.reserve(ws.size());
        for (std::size_t i = 0; i < ws.size(); ++i)
            typed_inputs.emplace_back(&typed_samples[i], ws[i].second);
        output_sideband = blendTyped(typed_inputs);
        if (ws.size() == 1) {
            std::memcpy(output.translations, scratch[0].translations, joint_count * sizeof(Vec4fV1));
            std::memcpy(output.rotations, scratch[0].rotations, joint_count * sizeof(QuatfV1));
            std::memcpy(output.scales, scratch[0].scales, joint_count * sizeof(Vec4fV1));
            return Status::ok;
        }
        auto blend = descriptor<BlendNormalDescV1>();
        blend.rig = rig;
        blend.layers = layers.data();
        blend.layer_count = static_cast<std::uint32_t>(layers.size());
        blend.output_pose = output.pose;
        return service.blend_normal(service.context, &blend);
    }

    const TransitionV1 *chooseTransition(std::size_t from) {
        if (!forced.empty()) {
            const auto selected = std::min_element(forced.begin(), forced.end(), [](const auto &a, const auto &b) {
                return a.sequence < b.sequence;
            });
            static TransitionV1 forced_transition;
            forced_transition = {};
            forced_transition.from = states[from].definition->name;
            forced_transition.to = states[selected->target].definition->name;
            forced_transition.priority = std::numeric_limits<std::int32_t>::max();
            forced_transition.interrupt = InterruptMode::always;
            forced_transition.duration = 0.0;
            forced_transition.declaration_index = 0;
            forced.erase(selected);
            return &forced_transition;
        }
        std::vector<const TransitionV1 *> candidates;
        for (const auto &candidate : document.transitions) {
            if (candidate.from != "*" && candidate.from != states[from].definition->name) continue;
            bool matches = true;
            for (const auto &condition : candidate.conditions)
                matches = matches && conditionTrue(parameters.at(condition.parameter), condition);
            if (matches) candidates.push_back(&candidate);
        }
        if (candidates.empty()) return nullptr;
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto *a, const auto *b) {
            if (a->priority != b->priority) return a->priority > b->priority;
            return a->declaration_index < b->declaration_index;
        });
        return candidates.front();
    }

    bool mayInterrupt(const TransitionV1 &candidate) const {
        if (!transition) return true;
        if (candidate.duration == 0.0) return true;
        if (candidate.interrupt == InterruptMode::always) return true;
        return candidate.interrupt == InterruptMode::higher_priority &&
               candidate.priority > transition->definition->priority;
    }

    Status evaluate() {
        tick_pending = false;
        auto current_rig = descriptor<ResolveAnimationRigDescV1>();
        current_rig.instance = instance;
        if (const auto status = service.resolve_rig(service.context, &current_rig); status != Status::ok)
            return fail(status, "rig became stale");
        auto current_layout = descriptor<ResolvePoseLayoutDescV1>();
        current_layout.rig = current_rig.rig;
        if (const auto status = service.resolve_layout(service.context, &current_layout); status != Status::ok)
            return fail(status, "layout became stale");
        if (current_layout.layout.identity != layout.identity ||
            current_layout.layout.generation != layout.generation) {
            const auto notification = sendNotification(AnimationNotificationKind::layout_generation_mismatch,
                                                       absolute_time, layout);
            if (notification != Status::ok) return notification;
            return fail(Status::incompatible_layout, "pose layout generation changed; evaluator reset required");
        }
        auto begin = descriptor<BeginPoseArenaFrameDescV1>();
        begin.owner = owner;
        begin.frame_revision = pending_revision;
        if (const auto status = service.begin_pose_frame(service.context, &begin); status != Status::ok) return status;

        PoseViewV1 source_pose{}, target_pose{}, output_pose{}, model_pose{};
        if (auto status = acquire(begin.arena, source_pose); status != Status::ok) return status;
        if (auto status = acquire(begin.arena, target_pose); status != Status::ok) return status;
        if (auto status = acquire(begin.arena, output_pose); status != Status::ok) return status;
        if (auto status = acquire(begin.arena, model_pose); status != Status::ok) return status;
        std::vector<PoseViewV1> scratch;
        TypedSideband source_sideband, target_sideband, output_sideband;

        // Materialize the current output before considering an interrupt.
        if (transition) {
            if (transition->source_is_snapshot) {
                restore(transition->snapshot, source_pose);
                source_sideband = {};
            } else if (const auto status = sampleState(
                           transition->source, begin.arena, source_pose,
                           scratch, source_sideband);
                     status != Status::ok) return status;
            if (const auto status = sampleState(
                    transition->target, begin.arena, target_pose, scratch,
                    target_sideband);
                status != Status::ok) return status;
            const auto alpha = transition->definition->duration == 0.0
                                   ? 1.0f
                                   : static_cast<float>(std::clamp(transition->elapsed / transition->definition->duration, 0.0, 1.0));
            if (alpha == 0.0f && transition->source_is_snapshot) {
                restore(transition->snapshot, output_pose);
                output_sideband = {};
            }
            else {
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
                layers[0].pose = source_pose.pose; layers[0].weight = 1.0f - alpha;
                layers[1].pose = target_pose.pose; layers[1].weight = alpha;
                auto blend = descriptor<BlendNormalDescV1>();
                blend.rig = rig; blend.layers = layers.data(); blend.layer_count = 2; blend.output_pose = output_pose.pose;
                if (const auto status = service.blend_normal(service.context, &blend); status != Status::ok) return status;
                output_sideband = blendTyped({
                    {&source_sideband, 1.0f - alpha},
                    {&target_sideband, alpha},
                });
            }
        } else if (const auto status = sampleState(
                       current, begin.arena, output_pose, scratch,
                       output_sideband);
                   status != Status::ok) {
            return status;
        }

        bool transition_started = false;
        const auto *decision = chooseTransition(transition ? transition->target : current);
        if (decision && mayInterrupt(*decision)) {
            const auto target = state_by_name.at(decision->to);
            if (decision->duration == 0.0) {
                current = target;
                states[current].phase = 0.0; // same-tick exit -> enter reset
                transition.reset();
                if (const auto status = sampleState(
                        current, begin.arena, output_pose, scratch,
                        output_sideband);
                    status != Status::ok)
                    return status;
            } else {
                ActiveTransition next;
                next.definition = decision;
                next.target = target;
                next.elapsed = 0.0;
                next.source = transition ? transition->target : current;
                next.source_is_snapshot = transition.has_value();
                states[target].phase = 0.0;
                if (transition) {
                    next.snapshot = capture(output_pose, pending_revision);
                    // alpha=0 is restored, not re-blended, so bytes are exact.
                    restore(next.snapshot, output_pose);
                    // Transition snapshots intentionally contain pose only;
                    // expression/gaze sidebands are not re-emitted.
                    output_sideband = {};
                }
                transition = std::move(next);
                transition_started = true;
            }
        }

        if (transition && !transition_started) {
            transition->elapsed += dt;
            if (transition->elapsed >= transition->definition->duration) {
                current = transition->target;
                transition.reset();
            }
        }

        if (const auto status = submitTyped(output_sideband);
            status != Status::ok)
            return status;

        std::vector<Matrix4fV1> matrices(joint_count);
        auto local_to_model = descriptor<LocalToModelDescV1>();
        local_to_model.rig = rig;
        local_to_model.local_pose = output_pose.pose;
        local_to_model.model_pose = model_pose.pose;
        local_to_model.model_matrices = matrices.data();
        local_to_model.model_matrix_capacity = static_cast<std::uint32_t>(matrices.size());
        if (const auto status = service.local_to_model(service.context, &local_to_model); status != Status::ok)
            return status;
        std::vector<Matrix4fV1> palette(palette_count);
        auto build = descriptor<BuildSkinPaletteDescV1>();
        build.skin_binding = binding;
        build.model_matrices = matrices.data();
        build.model_matrix_count = local_to_model.model_matrix_count;
        build.palette = palette.data();
        build.palette_capacity = static_cast<std::uint32_t>(palette.size());
        if (const auto status = service.build_skin_palette(service.context, &build); status != Status::ok)
            return status;

        auto publish = descriptor<PublishAnimationFrameFromSourceDescV1>();
        publish.source = source;
        publish.frame = descriptor<PublishAnimationFrameDescV1>();
        publish.frame.instance = instance;
        publish.frame.local_pose = output_pose.pose;
        publish.frame.model_pose = model_pose.pose;
        publish.frame.palette = palette.data();
        publish.frame.palette_count = static_cast<std::uint32_t>(palette.size());
        publish.frame.frame_revision = pending_revision;
        publish.frame.root_delta.rotation.w = 1.0f;
        if (const auto status = service.publish_animation_frame_from_source(service.context, &publish);
            status != Status::ok) return status;

        last_pose = capture(output_pose, pending_revision);
        trace = {};
        trace.current_state = states[current].definition->name;
        trace.frame_revision = pending_revision;
        trace.semantic_pose_hash = last_pose.hash;
        trace.source_reset_count = source_reset_count;
        trace.authority = AnimationSourceAuthorityV1::graph_apply;
        if (transition) {
            trace.transition_active = true;
            trace.transition_target = states[transition->target].definition->name;
            trace.transition_progress = transition->definition->duration == 0.0 ? 1.0
                : std::clamp(transition->elapsed / transition->definition->duration, 0.0, 1.0);
            if (transition->snapshot.valid) {
                trace.snapshot_revision = transition->snapshot.revision;
                trace.snapshot_layout_identity = transition->snapshot.layout.identity;
                trace.snapshot_layout_generation = transition->snapshot.layout.generation;
                trace.snapshot_pose_hash = transition->snapshot.hash;
            }
        }
        for (const auto &state : states) {
            for (const auto &clip : state.clips) {
                CursorStatusV1 cursor;
                cursor.clip = clip.node.clip;
                cursor.time_seconds = clip.time;
                cursor.normalized_phase = state.phase;
                cursor.asset_identity = clip.metadata.asset_identity;
                cursor.asset_generation = clip.metadata.asset_generation;
                cursor.profile_version = clip.metadata.profile_version;
                if (clip.metadata.clip_kind ==
                    AnimationClipKindV1::vrma_retargeted_clip) {
                    cursor.source_rig_sha256 =
                        sha256Hex(clip.metadata.source_rig_sha256);
                    cursor.target_rig_sha256 =
                        sha256Hex(clip.metadata.target_rig_sha256);
                }
                trace.cursors.push_back(std::move(cursor));
            }
        }
        error.clear();
        return Status::ok;
    }

    Status rebind() {
        if (!is_bound)
            return fail(Status::invalid_handle, "evaluator is not bound");

        auto next_instance = descriptor<ResolveAnimationInstanceDescV1>();
        next_instance.sink = sink;
        if (const auto status = service.resolve_instance(service.context,
                                                         &next_instance);
            status != Status::ok)
            return fail(status, "animation instance rebind failed");
        auto next_rig = descriptor<ResolveAnimationRigDescV1>();
        next_rig.instance = next_instance.instance;
        if (const auto status = service.resolve_rig(service.context, &next_rig);
            status != Status::ok)
            return fail(status, "animation rig rebind failed");
        auto next_layout = descriptor<ResolvePoseLayoutDescV1>();
        next_layout.rig = next_rig.rig;
        if (const auto status = service.resolve_layout(service.context,
                                                       &next_layout);
            status != Status::ok)
            return fail(status, "animation layout rebind failed");
        if (next_layout.layout.identity != layout.identity)
            return fail(Status::incompatible_layout,
                        "pose layout identity changed; evaluator reset required");

        auto next_states = states;
        bool vrma_generation_changed = false;
        std::vector<CursorHandle> created_cursors;
        const auto discard_created = [&]() {
            for (const auto cursor_handle : created_cursors) {
                auto destroy = descriptor<DestroyClipCursorDescV1>();
                destroy.owner = owner;
                destroy.cursor = cursor_handle;
                (void)service.destroy_cursor(service.context, &destroy);
            }
        };
        for (auto &state : next_states) {
            for (auto &clip : state.clips) {
                const auto previous_metadata = clip.metadata;
                auto resolve = descriptor<ResolveAnimationClipDescV1>();
                resolve.rig = next_rig.rig;
                resolve.clip_name = clip.node.clip.data();
                resolve.clip_name_size =
                    static_cast<std::uint32_t>(clip.node.clip.size());
                if (const auto status = service.resolve_clip(service.context,
                                                             &resolve);
                    status != Status::ok) {
                    discard_created();
                    return fail(status, "clip rebind failed: " + clip.node.clip);
                }
                auto metadata = descriptor<ClipMetadataV1>();
                metadata.clip = resolve.clip;
                if (const auto status = service.get_clip_metadata(service.context,
                                                                  &metadata);
                    status != Status::ok) {
                    discard_created();
                    return fail(status,
                                "clip metadata rebind failed: " + clip.node.clip);
                }
                if (!(metadata.end_seconds > metadata.start_seconds)) {
                    discard_created();
                    return fail(Status::invalid_argument,
                                "zero-duration clip after rebind: " +
                                    clip.node.clip);
                }
                auto cursor = descriptor<CreateClipCursorDescV1>();
                cursor.owner = owner;
                cursor.clip = resolve.clip;
                if (const auto status = service.create_cursor(service.context,
                                                               &cursor);
                    status != Status::ok) {
                    discard_created();
                    return fail(status,
                                "cursor rebind failed: " + clip.node.clip);
                }
                created_cursors.push_back(cursor.cursor);
                clip.handle = resolve.clip;
                clip.metadata = metadata;
                clip.cursor = cursor.cursor;
                const auto vrma_before =
                    previous_metadata.clip_kind ==
                    AnimationClipKindV1::vrma_retargeted_clip;
                const auto vrma_after =
                    metadata.clip_kind ==
                    AnimationClipKindV1::vrma_retargeted_clip;
                if ((vrma_before || vrma_after) &&
                    (previous_metadata.asset_identity !=
                         metadata.asset_identity ||
                     previous_metadata.asset_generation !=
                         metadata.asset_generation ||
                     previous_metadata.profile_version !=
                         metadata.profile_version ||
                     std::memcmp(previous_metadata.source_rig_sha256,
                                 metadata.source_rig_sha256, 32) != 0 ||
                     std::memcmp(previous_metadata.target_rig_sha256,
                                 metadata.target_rig_sha256, 32) != 0))
                    vrma_generation_changed = true;
            }
        }

        if (const auto status = ensureApplicationService(next_states);
            status != Status::ok) {
            discard_created();
            return status;
        }

        for (auto &state : states) {
            for (auto &clip : state.clips) {
                if (!isValid(clip.cursor)) continue;
                auto destroy = descriptor<DestroyClipCursorDescV1>();
                destroy.owner = owner;
                destroy.cursor = clip.cursor;
                (void)service.destroy_cursor(service.context, &destroy);
            }
        }
        instance = next_instance.instance;
        rig = next_rig.rig;
        layout = next_layout.layout;
        binding = next_layout.skin_binding;
        joint_count = next_layout.joint_count;
        palette_count = next_layout.palette_count;
        states = std::move(next_states);
        if (vrma_generation_changed) {
            ++source_reset_count;
            resetClockState();
        } else {
            if (last_pose.valid) last_pose.layout = layout;
            if (transition && transition->snapshot.valid)
                transition->snapshot.layout = layout;
        }
        error.clear();
        return Status::ok;
    }

    Status bind() {
        if (is_bound) return Status::ok;
        api = descriptor<ApiV1>();
        if (const auto status = getApiV1(abiVersionV1, &api); status != Status::ok)
            return fail(status, "getApiV1 failed");
        if (!api.get_animation_service) return fail(Status::unsupported_version, "animation service is unavailable");
        service = descriptor<AnimationServiceV1>();
        if (const auto status = api.get_animation_service(api.context, animationServiceVersionV1, &service);
            status != Status::ok) return fail(status, "AnimationServiceV1 negotiation failed");
        if ((service.capability_bits & animationServiceCapabilitiesV1) != animationServiceCapabilitiesV1)
            return fail(Status::unsupported_version, "AnimationServiceV1 capabilities are incomplete");
        auto owner_request = descriptor<CurrentAnimationOwnerDescV1>();
        if (const auto status = service.get_current_owner(service.context, &owner_request); status != Status::ok)
            return fail(status, "owner resolution failed");
        owner = owner_request.owner;
        auto sink_request = descriptor<ResolveAnimationSinkDescV1>();
        sink_request.object_name = object_name.data();
        sink_request.object_name_size = static_cast<std::uint32_t>(object_name.size());
        sink_request.sink_kind = AnimationSinkKind::skeletal_pose;
        if (const auto status = service.resolve_sink(service.context, &sink_request); status != Status::ok)
            return fail(status, "skeletal sink resolution failed");
        sink = sink_request.sink;
        auto instance_request = descriptor<ResolveAnimationInstanceDescV1>(); instance_request.sink = sink;
        if (const auto status = service.resolve_instance(service.context, &instance_request); status != Status::ok)
            return fail(status, "instance resolution failed");
        instance = instance_request.instance;
        auto rig_request = descriptor<ResolveAnimationRigDescV1>(); rig_request.instance = instance;
        if (const auto status = service.resolve_rig(service.context, &rig_request); status != Status::ok)
            return fail(status, "rig resolution failed");
        rig = rig_request.rig;
        auto layout_request = descriptor<ResolvePoseLayoutDescV1>(); layout_request.rig = rig;
        if (const auto status = service.resolve_layout(service.context, &layout_request); status != Status::ok)
            return fail(status, "layout resolution failed");
        layout = layout_request.layout; binding = layout_request.skin_binding;
        joint_count = layout_request.joint_count; palette_count = layout_request.palette_count;

        states.clear();
        for (const auto &definition : document.states) {
            RuntimeState state; state.definition = &definition;
            for (const auto &node : definition.clips) {
                RuntimeClip clip; clip.node = node;
                auto resolve = descriptor<ResolveAnimationClipDescV1>();
                resolve.rig = rig; resolve.clip_name = node.clip.data();
                resolve.clip_name_size = static_cast<std::uint32_t>(node.clip.size());
                if (const auto status = service.resolve_clip(service.context, &resolve); status != Status::ok)
                    return fail(status, "clip resolution failed: " + node.clip);
                clip.handle = resolve.clip;
                clip.metadata = descriptor<ClipMetadataV1>(); clip.metadata.clip = clip.handle;
                if (const auto status = service.get_clip_metadata(service.context, &clip.metadata); status != Status::ok)
                    return fail(status, "clip metadata failed: " + node.clip);
                if (!(clip.metadata.end_seconds > clip.metadata.start_seconds))
                    return fail(Status::invalid_argument, "zero-duration clip: " + node.clip);
                auto cursor = descriptor<CreateClipCursorDescV1>(); cursor.owner = owner; cursor.clip = clip.handle;
                if (const auto status = service.create_cursor(service.context, &cursor); status != Status::ok)
                    return fail(status, "cursor creation failed: " + node.clip);
                clip.cursor = cursor.cursor;
                state.clips.push_back(std::move(clip));
            }
            states.push_back(std::move(state));
        }
        if (const auto status = ensureApplicationService(states);
            status != Status::ok)
            return status;
        auto claim = descriptor<ClaimAnimationSourceDescV1>();
        claim.owner = owner; claim.sink = sink; claim.source_ordinal = source_ordinal;
        if (const auto status = service.claim_source(service.context, &claim); status != Status::ok)
            return fail(status, "source claim failed");
        source = claim.source;
        auto registration = descriptor<RegisterAnimationPhaseDescV1>();
        registration.owner = owner;
        registration.registration = descriptor<PhaseRegistrationV1>();
        registration.registration.phase = Phase::base_pose_and_root_modifier;
        registration.registration.priority = 0;
        registration.registration.registration_identity = 0x414e494d47524150ull ^ source.identity;
        registration.registration.registration_generation = owner.generation;
        registration.registration.source_ordinal = source_ordinal;
        registration.callback = &Impl::phase;
        registration.user_context = this;
        if (const auto status = service.register_phase(service.context, &registration); status != Status::ok)
            return fail(status, "phase registration failed");
        phase_registration = registration.registration_handle;
        is_bound = true;
        return Status::ok;
    }
};

EvaluatorV1::EvaluatorV1(DocumentV1 document, std::string object_name, std::uint32_t source_ordinal)
    : impl_(std::make_unique<Impl>(std::move(document), std::move(object_name), source_ordinal)) {}
EvaluatorV1::~EvaluatorV1() = default;
EvaluatorV1::EvaluatorV1(EvaluatorV1 &&) noexcept = default;
EvaluatorV1 &EvaluatorV1::operator=(EvaluatorV1 &&) noexcept = default;
Status EvaluatorV1::bind() { return impl_->bind(); }
Status EvaluatorV1::rebind() { return impl_->rebind(); }
bool EvaluatorV1::bound() const noexcept { return impl_->is_bound; }
Status EvaluatorV1::prepareTick(double absolute_time, double delta_seconds, std::uint64_t frame_revision) {
    if (!impl_->is_bound) return impl_->fail(Status::invalid_handle, "evaluator is not bound");
    if (!std::isfinite(absolute_time) || !std::isfinite(delta_seconds) || frame_revision == 0)
        return impl_->fail(Status::invalid_argument, "tick time/revision is invalid");
    impl_->absolute_time = absolute_time;
    impl_->dt = delta_seconds;
    impl_->pending_revision = frame_revision;
    impl_->tick_pending = true;
    return Status::ok;
}
Status EvaluatorV1::setParameter(std::string_view name, double value) {
    if (!std::isfinite(value)) return impl_->fail(Status::invalid_argument, "parameter must be finite");
    const auto found = impl_->parameters.find(std::string{name});
    if (found == impl_->parameters.end()) return impl_->fail(Status::not_found, "parameter is unknown");
    found->second = value;
    return Status::ok;
}
Status EvaluatorV1::forceState(std::string_view state) {
    const auto found = impl_->state_by_name.find(std::string{state});
    if (found == impl_->state_by_name.end()) return impl_->fail(Status::not_found, "state is unknown");
    impl_->forced.push_back({found->second, impl_->next_force_sequence++});
    return Status::ok;
}
Status EvaluatorV1::notify(AnimationNotificationKind kind, double time_seconds,
                           PoseLayoutHandle observed_layout) {
    return impl_->sendNotification(kind, time_seconds, observed_layout);
}
StatusTraceV1 EvaluatorV1::getStatus() const { return impl_->trace; }
std::vector<std::byte> EvaluatorV1::lastPoseBytes() const {
    std::vector<std::byte> result;
    if (!impl_->last_pose.valid) return result;
    const auto append = [&](const auto &values) {
        const auto *first = reinterpret_cast<const std::byte *>(values.data());
        result.insert(result.end(), first, first + values.size() * sizeof(values[0]));
    };
    append(impl_->last_pose.translations); append(impl_->last_pose.rotations); append(impl_->last_pose.scales);
    return result;
}
std::string EvaluatorV1::lastError() const { return impl_->error; }

} // namespace Pelican::AnimationGraph
