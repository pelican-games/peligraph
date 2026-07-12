#include "animationprobe.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Pelican::Animation {
namespace {

constexpr std::size_t headerSize = sizeof(DescriptorHeaderV1);

struct PoseRegistryRecord {
    std::uint32_t generation{};
    bool active{};
};

std::atomic_uint64_t nextPoseIdentity{1};
std::mutex poseRegistryMutex;
std::unordered_map<std::uint64_t, PoseRegistryRecord> poseRegistry;

void invalidateRegisteredPoses(const std::unordered_set<std::uint64_t> &identities) {
    std::scoped_lock lock{poseRegistryMutex};
    for (const auto identity : identities)
        if (const auto found = poseRegistry.find(identity); found != poseRegistry.end()) found->second.active = false;
}

template <class T> Status validateDescriptor(const T &value, std::size_t minimum_size = headerSize) {
    if (value.struct_size < minimum_size) return Status::invalid_argument;
    if (value.version != descriptorVersionV1) return Status::unsupported_version;
    if (value.reserved0 != 0 || value.reserved1 != 0) return Status::reserved_not_zero;
    return Status::ok;
}

bool fieldAvailable(std::uint32_t size, std::size_t offset, std::size_t field_size) {
    return size >= offset + field_size;
}

double wrapTime(double time, double duration, WrapMode wrap) {
    if (wrap == WrapMode::clamp) return std::clamp(time, 0.0, duration);
    double value = std::fmod(time, duration);
    if (value < 0.0) value += duration;
    return value;
}

glm::mat4 transformMatrix(const TransformV1 &value) {
    const glm::vec3 t{value.translation.x, value.translation.y, value.translation.z};
    const glm::quat r{value.rotation.w, value.rotation.x, value.rotation.y, value.rotation.z};
    const glm::vec3 s{value.scale.x, value.scale.y, value.scale.z};
    return glm::translate(glm::mat4{1.0f}, t) * glm::mat4_cast(r) * glm::scale(glm::mat4{1.0f}, s);
}

Matrix4fV1 toPublic(const glm::mat4 &value) {
    Matrix4fV1 result{};
    std::memcpy(result.column_major, &value[0][0], sizeof(result.column_major));
    return result;
}

} // namespace

struct ProbeRuntime::Impl {
    struct CursorRecord {
        std::uint32_t generation{1};
        bool active{true};
        double duration{};
        double time{};
        WrapMode wrap{WrapMode::repeat};
        std::vector<ProbeAnnotation> annotations;
    };
    struct CommitRecord {
        std::uint32_t generation{};
        std::uint64_t current{};
        std::uint64_t previous{};
    };

    explicit Impl(std::size_t capacity)
        : storage(capacity + poseAlignmentV1), capacity(capacity), owner(std::this_thread::get_id()) {
        const auto raw = reinterpret_cast<std::uintptr_t>(storage.data());
        constexpr auto alignment = static_cast<std::uintptr_t>(poseAlignmentV1);
        aligned_base = reinterpret_cast<std::byte *>((raw + alignment - 1) & ~(alignment - 1));
    }

    std::vector<std::byte> storage;
    std::byte *aligned_base{};
    std::size_t capacity{};
    std::size_t used{};
    std::uint32_t arena_generation{};
    std::uint64_t frame_revision{};
    std::vector<std::uint64_t> pose_slot_identities;
    std::size_t pose_slot_cursor{};
    std::uint64_t next_cursor_identity{1};
    std::thread::id owner;
    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, CursorRecord> cursors;
    std::unordered_set<std::uint64_t> pose_identities;
    std::unordered_map<std::uint64_t, CommitRecord> commits;
    std::uint64_t last_temporal_revision{};

    std::byte *allocate(std::size_t size) {
        const auto aligned = (used + poseAlignmentV1 - 1) & ~(poseAlignmentV1 - 1);
        if (aligned > capacity || size > capacity - aligned) return nullptr;
        used = aligned + size;
        return aligned_base + aligned;
    }
};

ProbeRuntime::ProbeRuntime(std::size_t arena_capacity) : impl_(std::make_unique<Impl>(arena_capacity)) {}
ProbeRuntime::~ProbeRuntime() {
    if (impl_) invalidateRegisteredPoses(impl_->pose_identities);
}
ProbeRuntime::ProbeRuntime(ProbeRuntime &&) noexcept = default;
ProbeRuntime &ProbeRuntime::operator=(ProbeRuntime &&other) noexcept {
    if (this == &other) return *this;
    if (impl_) invalidateRegisteredPoses(impl_->pose_identities);
    impl_ = std::move(other.impl_);
    return *this;
}

PoseArenaHandle ProbeRuntime::beginFrame(std::uint64_t frame_revision) {
    std::scoped_lock lock{impl_->mutex};
    if (std::this_thread::get_id() != impl_->owner) return invalidHandle<PoseArenaHandle>();
    impl_->used = 0;
    invalidateRegisteredPoses(impl_->pose_identities);
    impl_->pose_identities.clear();
    impl_->pose_slot_cursor = 0;
    impl_->frame_revision = frame_revision;
    if (++impl_->arena_generation == 0) ++impl_->arena_generation;
    return PoseArenaHandle{1, impl_->arena_generation, 0};
}

Status ProbeRuntime::acquirePose(PoseArenaHandle arena, PoseLayoutHandle layout, std::uint32_t joint_count,
                                 PoseViewV1 &out_view) {
    std::scoped_lock lock{impl_->mutex};
    if (std::this_thread::get_id() != impl_->owner) return Status::wrong_thread;
    if (!isValid(arena) || arena.identity != 1) return Status::invalid_handle;
    if (arena.generation != impl_->arena_generation) return Status::stale_generation;
    if (!isValid(layout) || joint_count == 0) return Status::invalid_argument;
    const auto status = validateDescriptor(out_view);
    if (status != Status::ok) return status;

    const auto array_size = static_cast<std::size_t>(joint_count) * sizeof(Vec4fV1);
    const auto saved_used = impl_->used;
    auto *translations = impl_->allocate(array_size);
    auto *rotations = impl_->allocate(array_size);
    auto *scales = impl_->allocate(array_size);
    if (!translations || !rotations || !scales) {
        impl_->used = saved_used;
        return Status::out_of_memory;
    }

    std::memset(translations, 0, array_size);
    std::memset(rotations, 0, array_size);
    std::memset(scales, 0, array_size);
    auto *rotation_values = reinterpret_cast<QuatfV1 *>(rotations);
    auto *scale_values = reinterpret_cast<Vec4fV1 *>(scales);
    for (std::uint32_t i = 0; i < joint_count; ++i) {
        rotation_values[i].w = 1.0f;
        scale_values[i] = {1.0f, 1.0f, 1.0f, 0.0f};
    }

    const auto caller_size = out_view.struct_size;
    PoseViewV1 produced{};
    produced.struct_size = sizeof(PoseViewV1);
    produced.version = descriptorVersionV1;
    if (impl_->pose_slot_cursor == impl_->pose_slot_identities.size())
        impl_->pose_slot_identities.push_back(nextPoseIdentity.fetch_add(1, std::memory_order_relaxed));
    produced.pose = PoseHandle{impl_->pose_slot_identities[impl_->pose_slot_cursor++], impl_->arena_generation, 0};
    impl_->pose_identities.insert(produced.pose.identity);
    {
        std::scoped_lock registry_lock{poseRegistryMutex};
        poseRegistry[produced.pose.identity] = {produced.pose.generation, true};
    }
    produced.layout = layout;
    produced.joint_count = joint_count;
    produced.element_stride = sizeof(Vec4fV1);
    produced.translations = reinterpret_cast<Vec4fV1 *>(translations);
    produced.rotations = rotation_values;
    produced.scales = scale_values;
    std::memcpy(&out_view, &produced, std::min<std::size_t>(caller_size, sizeof(produced)));
    return Status::ok;
}

Status ProbeRuntime::validatePose(PoseHandle pose) const {
    std::scoped_lock lock{impl_->mutex};
    if (const auto status = validatePoseHandle(pose); status != Status::ok) return status;
    return impl_->pose_identities.contains(pose.identity) ? Status::ok : Status::invalid_handle;
}

Status ProbeRuntime::validatePoseHandle(PoseHandle pose) {
    if (!isValid(pose)) return Status::invalid_handle;
    std::scoped_lock lock{poseRegistryMutex};
    const auto found = poseRegistry.find(pose.identity);
    if (found == poseRegistry.end()) return Status::invalid_handle;
    if (!found->second.active || found->second.generation != pose.generation)
        return Status::stale_generation;
    return Status::ok;
}

CursorHandle ProbeRuntime::createCursor(double duration_seconds, WrapMode wrap_mode,
                                        std::span<const ProbeAnnotation> annotations) {
    if (!std::isfinite(duration_seconds) || duration_seconds <= 0.0) return invalidHandle<CursorHandle>();
    std::scoped_lock lock{impl_->mutex};
    const auto identity = impl_->next_cursor_identity++;
    Impl::CursorRecord record;
    record.duration = duration_seconds;
    record.wrap = wrap_mode;
    record.annotations.assign(annotations.begin(), annotations.end());
    for (std::size_t i = 0; i < record.annotations.size(); ++i) {
        const auto &annotation = record.annotations[i];
        if (!std::isfinite(annotation.time_seconds) || annotation.time_seconds < 0.0 ||
            annotation.time_seconds >= duration_seconds)
            return invalidHandle<CursorHandle>();
        for (std::size_t j = 0; j < i; ++j) {
            if (record.annotations[j].source == annotation.source && record.annotations[j].ordinal == annotation.ordinal)
                return invalidHandle<CursorHandle>();
        }
    }
    std::stable_sort(record.annotations.begin(), record.annotations.end(), [](const auto &a, const auto &b) {
        if (a.time_seconds != b.time_seconds) return a.time_seconds < b.time_seconds;
        if (a.source != b.source) return a.source < b.source;
        return a.ordinal < b.ordinal;
    });
    impl_->cursors.emplace(identity, std::move(record));
    return CursorHandle{identity, 1, 0};
}

Status ProbeRuntime::destroyCursor(CursorHandle cursor) {
    std::scoped_lock lock{impl_->mutex};
    const auto it = impl_->cursors.find(cursor.identity);
    if (it == impl_->cursors.end() || !isValid(cursor)) return Status::invalid_handle;
    if (cursor.generation != it->second.generation || !it->second.active) return Status::stale_generation;
    it->second.active = false;
    if (++it->second.generation == 0) ++it->second.generation;
    return Status::ok;
}

Status ProbeRuntime::advanceCursor(const AdvanceDescV1 &desc, IntervalResultV1 &result) {
    constexpr auto minimum_desc = offsetof(AdvanceDescV1, reserved2) + sizeof(std::uint32_t);
    constexpr auto minimum_result = offsetof(IntervalResultV1, crossing_count) + sizeof(std::uint32_t);
    if (const auto status = validateDescriptor(desc, minimum_desc); status != Status::ok) return status;
    if (const auto status = validateDescriptor(result, minimum_result); status != Status::ok) return status;
    if (desc.reserved2 != 0) return Status::reserved_not_zero;
    if (!std::isfinite(desc.delta_seconds) || !std::isfinite(desc.absolute_seconds)) return Status::invalid_argument;
    if ((desc.flags & ~(advance_absolute_seek | advance_discontinuity)) != 0) return Status::invalid_argument;

    std::scoped_lock lock{impl_->mutex};
    const auto it = impl_->cursors.find(desc.cursor.identity);
    if (it == impl_->cursors.end() || !isValid(desc.cursor)) return Status::invalid_handle;
    auto &cursor = it->second;
    if (!cursor.active || desc.cursor.generation != cursor.generation) return Status::stale_generation;

    const bool seek = (desc.flags & advance_absolute_seek) != 0;
    const double start = cursor.time;
    const double end_unwrapped = seek ? desc.absolute_seconds : start + desc.delta_seconds;
    const double end = wrapTime(end_unwrapped, cursor.duration, cursor.wrap);
    const double traversal_end = cursor.wrap == WrapMode::clamp ? end : end_unwrapped;
    std::vector<CrossingV1> crossings;
    std::int64_t loop_count = 0;

    if (!seek && desc.delta_seconds != 0.0) {
        const double low = std::min(start, traversal_end);
        const double high = std::max(start, traversal_end);
        const auto first_loop = static_cast<std::int64_t>(std::floor(low / cursor.duration)) - 1;
        const auto last_loop = static_cast<std::int64_t>(std::ceil(high / cursor.duration)) + 1;
        const auto actual_first_loop = cursor.wrap == WrapMode::repeat ? first_loop : std::int64_t{0};
        const auto actual_last_loop = cursor.wrap == WrapMode::repeat ? last_loop : std::int64_t{0};
        for (std::int64_t loop = actual_first_loop; loop <= actual_last_loop; ++loop) {
            for (const auto &annotation : cursor.annotations) {
                const double occurrence = annotation.time_seconds + static_cast<double>(loop) * cursor.duration;
                const bool crossed = desc.delta_seconds > 0.0 ? (occurrence > start && occurrence <= traversal_end)
                                                                  : (occurrence >= traversal_end && occurrence < start);
                if (!crossed) continue;
                crossings.push_back({sizeof(CrossingV1), descriptorVersionV1, annotation.kind, annotation.source,
                                     annotation.ordinal, 0, annotation.time_seconds, loop, annotation.identity});
            }
        }
        std::stable_sort(crossings.begin(), crossings.end(), [reverse = desc.delta_seconds < 0.0,
                                                               duration = cursor.duration](const auto &a,
                                                                                            const auto &b) {
            const double ta = a.clip_time_seconds + static_cast<double>(a.loop_index) * duration;
            const double tb = b.clip_time_seconds + static_cast<double>(b.loop_index) * duration;
            if (ta != tb) return reverse ? ta > tb : ta < tb;
            if (a.source != b.source) return a.source < b.source;
            return a.ordinal < b.ordinal;
        });
        if (cursor.wrap == WrapMode::repeat)
            loop_count = static_cast<std::int64_t>(std::floor(end_unwrapped / cursor.duration));
    }

    const auto required = static_cast<std::uint32_t>(crossings.size());
    result.crossing_count = required;
    if (fieldAvailable(result.struct_size, offsetof(IntervalResultV1, value_count), sizeof(result.value_count)))
        result.value_count = 0;
    if (required > result.crossing_capacity || (required != 0 && result.crossings == nullptr))
        return Status::buffer_too_small;
    if (fieldAvailable(result.struct_size, offsetof(IntervalResultV1, value_capacity), sizeof(result.value_capacity)) &&
        result.value_capacity != 0 && result.values == nullptr)
        return Status::invalid_argument;

    for (std::uint32_t i = 0; i < required; ++i) result.crossings[i] = crossings[i];
    result.result_flags = interval_none;
    if (seek) result.result_flags |= interval_seeked | interval_discontinuous;
    if (desc.flags & advance_discontinuity) result.result_flags |= interval_discontinuous;
    if (!seek && desc.delta_seconds < 0.0) result.result_flags |= interval_reverse;
    if (!seek && loop_count != 0) result.result_flags |= interval_looped;
    result.normalized_phase = static_cast<float>(end / cursor.duration);
    result.root_delta = {};
    result.root_delta.rotation.w = 1.0f;
    if (!seek)
        result.root_delta.translation.x =
            static_cast<float>(cursor.wrap == WrapMode::clamp ? end - start : desc.delta_seconds);
    cursor.time = end;
    return Status::ok;
}

double ProbeRuntime::cursorTime(CursorHandle cursor) const {
    std::scoped_lock lock{impl_->mutex};
    const auto it = impl_->cursors.find(cursor.identity);
    if (it == impl_->cursors.end() || !it->second.active || cursor.generation != it->second.generation)
        return std::numeric_limits<double>::quiet_NaN();
    return it->second.time;
}

QuatfV1 ProbeRuntime::blendQuaternions(std::span<const QuatfV1> values, std::span<const float> weights) {
    if (values.size() != weights.size()) return {0, 0, 0, 1};
    std::size_t reference_index = values.size();
    for (std::size_t i = 0; i < weights.size(); ++i) {
        if (std::isfinite(weights[i]) && weights[i] > 0.0f) { reference_index = i; break; }
    }
    if (reference_index == values.size()) return {0, 0, 0, 1};
    const auto &reference = values[reference_index];
    double x = 0, y = 0, z = 0, w = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double weight = std::isfinite(weights[i]) && weights[i] > 0.0f ? weights[i] : 0.0;
        const auto &q = values[i];
        const double dot = static_cast<double>(q.x) * reference.x + static_cast<double>(q.y) * reference.y +
                           static_cast<double>(q.z) * reference.z + static_cast<double>(q.w) * reference.w;
        const double sign = dot < 0.0 ? -1.0 : 1.0;
        x += weight * sign * q.x; y += weight * sign * q.y;
        z += weight * sign * q.z; w += weight * sign * q.w;
    }
    const double length = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(length > 1e-12) || !std::isfinite(length)) return {0, 0, 0, 1};
    x /= length; y /= length; z /= length; w /= length;
    if (w < 0.0 || (w == 0.0 && (z < 0.0 || (z == 0.0 && (y < 0.0 || (y == 0.0 && x < 0.0)))))) {
        x = -x; y = -y; z = -z; w = -w;
    }
    // Signed zero is numerically equal but not byte-identical. Canonicalize it
    // after the frozen hemisphere/sign algorithm so source permutation cannot
    // leak a reference-dependent -0.0 into the published quaternion.
    if (x == 0.0) x = 0.0;
    if (y == 0.0) y = 0.0;
    if (z == 0.0) z = 0.0;
    if (w == 0.0) w = 0.0;
    return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)};
}

Status ProbeRuntime::localToModel(std::span<const TransformV1> local, std::span<const std::int32_t> parents,
                                  std::span<Matrix4fV1> model) {
    if (local.size() != parents.size() || model.size() < local.size()) return Status::invalid_argument;
    std::vector<glm::mat4> matrices(local.size());
    for (std::size_t i = 0; i < local.size(); ++i) {
        if (parents[i] < -1 || parents[i] >= static_cast<std::int32_t>(i)) return Status::invalid_argument;
        const auto local_matrix = transformMatrix(local[i]);
        matrices[i] = parents[i] < 0 ? local_matrix : matrices[parents[i]] * local_matrix;
        model[i] = toPublic(matrices[i]);
    }
    return Status::ok;
}

Status ProbeRuntime::publishAnimationFrame(const PublishAnimationFrameDescV1 &desc) {
    constexpr auto minimum = offsetof(PublishAnimationFrameDescV1, root_delta) + sizeof(RootDeltaV1);
    if (const auto status = validateDescriptor(desc, minimum); status != Status::ok) return status;
    if (!isValid(desc.instance) || desc.frame_revision == 0) return Status::invalid_argument;
    if (desc.palette_count != 0 && desc.palette == nullptr) return Status::invalid_argument;
    if ((desc.flags & ~(commit_reset_history | commit_discontinuity)) != 0) return Status::invalid_argument;
    std::scoped_lock lock{impl_->mutex};
    auto &record = impl_->commits[desc.instance.identity];
    if (record.generation != 0 && record.generation != desc.instance.generation) {
        record = {};
    }
    record.generation = desc.instance.generation;
    if (record.current != 0 && desc.frame_revision <= record.current) return Status::duplicate_revision;
    record.current = desc.frame_revision;
    if (record.previous == 0 || (desc.flags & (commit_reset_history | commit_discontinuity)) != 0)
        record.previous = record.current;
    return Status::ok;
}

Status ProbeRuntime::advanceTemporalHistoryAfterRender(const AdvanceTemporalHistoryDescV1 &desc) {
    constexpr auto minimum = offsetof(AdvanceTemporalHistoryDescV1, reserved2) + sizeof(std::uint32_t);
    if (const auto status = validateDescriptor(desc, minimum); status != Status::ok) return status;
    if (desc.reserved2 != 0 || desc.velocity_consumed != 1 || desc.rendered_frame_revision == 0)
        return Status::invalid_argument;
    std::scoped_lock lock{impl_->mutex};
    if (impl_->last_temporal_revision == desc.rendered_frame_revision) return Status::duplicate_revision;
    bool found = false;
    for (const auto &[_, record] : impl_->commits) found |= record.current == desc.rendered_frame_revision;
    if (!found) return Status::invalid_argument;
    for (auto &[_, record] : impl_->commits) {
        if (record.current == desc.rendered_frame_revision) record.previous = record.current;
    }
    impl_->last_temporal_revision = desc.rendered_frame_revision;
    return Status::ok;
}

std::uint64_t ProbeRuntime::currentRevision(InstanceHandle instance) const {
    std::scoped_lock lock{impl_->mutex};
    const auto it = impl_->commits.find(instance.identity);
    return it != impl_->commits.end() && it->second.generation == instance.generation ? it->second.current : 0;
}

std::uint64_t ProbeRuntime::previousRevision(InstanceHandle instance) const {
    std::scoped_lock lock{impl_->mutex};
    const auto it = impl_->commits.find(instance.identity);
    return it != impl_->commits.end() && it->second.generation == instance.generation ? it->second.previous : 0;
}

} // namespace Pelican::Animation
