#include "vrmapplication.hpp"

#include "animationservice.hpp"
#include "../container.hpp"
#include "../log.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../userpublic/animation/pose_staging_v1.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/quaternion.hpp>

namespace Pelican::Vrm {
namespace {

using Animation::Status;

template <class T> Status validateDescriptor(const T &value) {
    if (value.struct_size < sizeof(T)) return Status::invalid_argument;
    if (value.version != applicationDescriptorVersionV1)
        return Status::unsupported_version;
    if (value.reserved0 != 0 || value.reserved1 != 0)
        return Status::reserved_not_zero;
    return Status::ok;
}

bool sameInstance(Animation::InstanceHandle left,
                  Animation::InstanceHandle right) noexcept {
    return left.identity == right.identity && left.generation == right.generation &&
           left.reserved == 0 && right.reserved == 0;
}

bool sameApplicationFrame(ApplicationFrameHandle left,
                          ApplicationFrameHandle right) noexcept {
    return left.identity == right.identity && left.generation == right.generation;
}

bool isExpression(const VrmSemanticData &semantic, std::string_view name) {
    return semantic.preset_expressions.contains(std::string{name}) ||
           semantic.custom_expressions.contains(std::string{name});
}

template <class Function>
void forEachExpression(const VrmSemanticData &semantic, Function &&function) {
    for (const auto &[name, expression] : semantic.preset_expressions)
        function(name, expression);
    for (const auto &[name, expression] : semantic.custom_expressions)
        function(name, expression);
}

enum class ProceduralGroup { none, mouth, blink, look_at };

ProceduralGroup proceduralGroup(std::string_view name) {
    if (name == "aa" || name == "ih" || name == "ou" || name == "ee" ||
        name == "oh")
        return ProceduralGroup::mouth;
    if (name == "blink" || name == "blinkLeft" || name == "blinkRight")
        return ProceduralGroup::blink;
    if (name == "lookUp" || name == "lookDown" || name == "lookLeft" ||
        name == "lookRight")
        return ProceduralGroup::look_at;
    return ProceduralGroup::none;
}

std::string_view overrideFor(const VrmExpression &expression,
                             ProceduralGroup target) {
    switch (target) {
    case ProceduralGroup::mouth: return expression.override_mouth;
    case ProceduralGroup::blink: return expression.override_blink;
    case ProceduralGroup::look_at: return expression.override_look_at;
    case ProceduralGroup::none: break;
    }
    return "none";
}

float rangeMapWeight(float value, const std::optional<VrmLookAtRangeMap> &range) {
    const auto input_max = static_cast<float>(
        range && range->input_max_value ? *range->input_max_value : 90.0);
    const auto output_scale = static_cast<float>(
        range && range->output_scale ? *range->output_scale : 1.0);
    const auto magnitude = std::abs(value);
    if (input_max == 0.0f)
        return magnitude == 0.0f ? 0.0f : std::clamp(output_scale, 0.0f, 1.0f);
    return std::clamp(std::min(magnitude, std::abs(input_max)) /
                          std::abs(input_max) * output_scale,
                      0.0f, 1.0f);
}

float rangeMapDegrees(float value,
                      const std::optional<VrmLookAtRangeMap> &range) {
    const auto input_max = static_cast<float>(
        range && range->input_max_value ? *range->input_max_value : 90.0);
    const auto output_scale = static_cast<float>(
        range && range->output_scale ? *range->output_scale : 1.0);
    const auto magnitude = std::abs(value);
    if (input_max == 0.0f)
        return magnitude == 0.0f ? 0.0f : output_scale;
    return std::min(magnitude, std::abs(input_max)) /
           std::abs(input_max) * output_scale;
}

const VrmHumanBone *humanBone(const VrmSemanticData &semantic,
                              std::string_view name) {
    const auto found = std::find_if(
        semantic.human_bones.begin(), semantic.human_bones.end(),
        [&](const VrmHumanBone &bone) { return bone.name == name; });
    return found == semantic.human_bones.end() ? nullptr : &*found;
}

const SourceMaterialInitialValues *initialMaterial(
    const SourceMaterialInitialValueTable *table, std::uint32_t index) {
    if (!table || index >= table->values.size() ||
        table->values[index].source_material_index != index)
        return nullptr;
    return &table->values[index];
}

bool finite(const ResolvedMaterialOverride &value) {
    const auto vector_finite = [](const auto &vector) {
        for (glm::length_t index = 0; index < vector.length(); ++index)
            if (!std::isfinite(vector[index])) return false;
        return true;
    };
    return vector_finite(value.base_color_factor) &&
           vector_finite(value.emissive_factor) &&
           vector_finite(value.uv_offset) && vector_finite(value.uv_scale) &&
           std::isfinite(value.uv_rotation);
}

void copyString(std::string_view source, char *destination,
                std::uint32_t capacity) {
    if (!destination || capacity == 0) return;
    const auto count = std::min<std::size_t>(source.size(), capacity - 1);
    std::memcpy(destination, source.data(), count);
    destination[count] = '\0';
}

} // namespace

Status evaluateExpressionLookAt(const VrmSemanticData &semantic,
                                ExpressionInputSnapshot &snapshot) noexcept {
    try {
        if (!snapshot.look_at_enabled || !semantic.look_at ||
            semantic.look_at->type.value_or("bone") != "expression")
            return Status::ok;
        const auto &look_at = *semantic.look_at;
        const auto set = [&](std::string_view name, float value) {
            if (isExpression(semantic, name))
                snapshot.expression_weights[std::string{name}] = value;
        };
        if (snapshot.look_at_yaw_degrees > 0.0f) {
            set("lookLeft", rangeMapWeight(snapshot.look_at_yaw_degrees,
                                           look_at.horizontal_outer));
            set("lookRight", 0.0f);
        } else {
            set("lookLeft", 0.0f);
            set("lookRight", rangeMapWeight(snapshot.look_at_yaw_degrees,
                                             look_at.horizontal_outer));
        }
        if (snapshot.look_at_pitch_degrees > 0.0f) {
            set("lookDown", rangeMapWeight(snapshot.look_at_pitch_degrees,
                                           look_at.vertical_down));
            set("lookUp", 0.0f);
        } else {
            set("lookDown", 0.0f);
            set("lookUp", rangeMapWeight(snapshot.look_at_pitch_degrees,
                                         look_at.vertical_up));
        }
        return Status::ok;
    } catch (...) {
        return Status::out_of_memory;
    }
}

Status evaluateBoneLookAt(const VrmSemanticData &semantic,
                          const ExpressionInputSnapshot &snapshot,
                          BoneLookAtRotations &rotations) noexcept {
    try {
        rotations = {};
        if (!snapshot.look_at_enabled || !semantic.look_at ||
            semantic.look_at->type.value_or("bone") != "bone")
            return Status::ok;
        const auto &look_at = *semantic.look_at;
        const auto yaw = snapshot.look_at_yaw_degrees;
        const auto pitch = snapshot.look_at_pitch_degrees;
        const auto left_yaw = yaw > 0.0f
                                  ? rangeMapDegrees(yaw, look_at.horizontal_outer)
                                  : -rangeMapDegrees(yaw, look_at.horizontal_inner);
        const auto right_yaw = yaw > 0.0f
                                   ? rangeMapDegrees(yaw, look_at.horizontal_inner)
                                   : -rangeMapDegrees(yaw, look_at.horizontal_outer);
        const auto mapped_pitch =
            pitch > 0.0f
                ? rangeMapDegrees(pitch, look_at.vertical_down)
                : -rangeMapDegrees(pitch, look_at.vertical_up);
        const auto rotation = [&](float yaw_degrees) {
            const auto yaw_rotation = glm::angleAxis(
                glm::radians(yaw_degrees), glm::vec3{0.0f, 1.0f, 0.0f});
            const auto pitch_rotation = glm::angleAxis(
                glm::radians(mapped_pitch), glm::vec3{1.0f, 0.0f, 0.0f});
            return glm::normalize(yaw_rotation * pitch_rotation);
        };
        rotations.active = true;
        rotations.has_left_eye = humanBone(semantic, "leftEye") != nullptr;
        rotations.has_right_eye = humanBone(semantic, "rightEye") != nullptr;
        rotations.left_eye = rotation(left_yaw);
        rotations.right_eye = rotation(right_yaw);
        return Status::ok;
    } catch (...) {
        return Status::out_of_memory;
    }
}

Status resolveExpressionFrame(
    const VrmSemanticData &semantic, const MorphTargetLayout *morph_layout,
    const SourceMaterialInitialValueTable *material_initial_values,
    const ExpressionInputSnapshot &snapshot,
    ResolvedExpressionFrame &resolved) noexcept {
    try {
        ResolvedExpressionFrame next;
        next.instance = snapshot.instance;
        next.input_revision = snapshot.input_revision;
        next.frame_revision = snapshot.frame_revision;
        next.source_ordinal = snapshot.source_ordinal;
        next.flags = snapshot.flags;
        next.morph_layout_generation = morph_layout ? morph_layout->generation : 0;

        forEachExpression(semantic, [&](const std::string &name,
                                        const VrmExpression &expression) {
            const auto found = snapshot.expression_weights.find(name);
            auto weight = found == snapshot.expression_weights.end()
                              ? 0.0f
                              : std::clamp(found->second, 0.0f, 1.0f);
            if (expression.is_binary) weight = weight >= 0.5f ? 1.0f : 0.0f;
            next.expression_weights.emplace(name, weight);
        });

        // Override contributors use their post-binary values. A binary target
        // is completely suppressed by any non-zero received effect.
        const auto override_contributor_weights = next.expression_weights;
        for (auto &[target_name, target_weight] : next.expression_weights) {
            const auto target_group = proceduralGroup(target_name);
            if (target_group == ProceduralGroup::none) continue;
            bool block = false;
            float blend = 0.0f;
            forEachExpression(semantic, [&](const std::string &source_name,
                                            const VrmExpression &source) {
                if (proceduralGroup(source_name) == target_group) return;
                const auto weight =
                    override_contributor_weights.at(source_name);
                if (weight <= 0.0f) return;
                const auto mode = overrideFor(source, target_group);
                if (mode == "block") block = true;
                if (mode == "blend") blend += weight;
            });
            const auto effect = block || blend > 0.0f;
            const auto *target_expression = [&]() -> const VrmExpression * {
                if (const auto found = semantic.preset_expressions.find(target_name);
                    found != semantic.preset_expressions.end())
                    return &found->second;
                const auto found = semantic.custom_expressions.find(target_name);
                return found == semantic.custom_expressions.end() ? nullptr
                                                                   : &found->second;
            }();
            if (effect && target_expression && target_expression->is_binary) {
                target_weight = 0.0f;
            } else if (block) {
                target_weight = 0.0f;
            } else {
                target_weight *= 1.0f - std::clamp(blend, 0.0f, 1.0f);
            }
        }

        if (morph_layout) {
            next.morph_weights.assign(morph_layout->default_weights.size(), 0.0f);
        }
        std::map<std::uint32_t, ResolvedMaterialOverride> materials;
        const auto material = [&](std::uint32_t index)
            -> ResolvedMaterialOverride * {
            if (auto found = materials.find(index); found != materials.end())
                return &found->second;
            const auto *initial = initialMaterial(material_initial_values, index);
            if (!initial) return nullptr;
            auto [inserted, _] = materials.emplace(
                index,
                ResolvedMaterialOverride{
                    .source_material_index = index,
                    .base_color_factor = initial->base_color_factor,
                    .emissive_factor = initial->emissive_factor,
                    .uv_offset = initial->uv_offset,
                    .uv_scale = initial->uv_scale,
                    .uv_rotation = initial->uv_rotation,
                });
            return &inserted->second;
        };

        Status resolve_status = Status::ok;
        forEachExpression(semantic, [&](const std::string &name,
                                        const VrmExpression &expression) {
            if (resolve_status != Status::ok) return;
            const auto expression_weight = next.expression_weights.at(name);
            for (const auto &bind : expression.morph_target_binds) {
                if (!morph_layout) {
                    resolve_status = Status::incompatible_layout;
                    return;
                }
                std::set<std::uint32_t> offsets;
                for (const auto &primitive : morph_layout->primitives) {
                    if (primitive.node_index != static_cast<std::uint32_t>(bind.node) ||
                        bind.index < 0 ||
                        static_cast<std::size_t>(bind.index) >= primitive.delta_ranges.size())
                        continue;
                    offsets.insert(primitive.weight_offset);
                }
                if (offsets.empty()) {
                    resolve_status = Status::incompatible_layout;
                    return;
                }
                for (const auto offset : offsets) {
                    const auto index = offset + static_cast<std::uint32_t>(bind.index);
                    if (index >= next.morph_weights.size()) {
                        resolve_status = Status::incompatible_layout;
                        return;
                    }
                    next.morph_weights[index] +=
                        static_cast<float>(bind.weight) * expression_weight;
                }
            }

            for (std::uint32_t ordinal = 0;
                 ordinal < expression.material_color_binds.size(); ++ordinal) {
                const auto &bind = expression.material_color_binds[ordinal];
                if (bind.material < 0) {
                    resolve_status = Status::incompatible_layout;
                    return;
                }
                if (bind.type != "color" && bind.type != "emissionColor") {
                    next.diagnostics.push_back(ApplicationDiagnostic{
                        .expression_name = name,
                        .material_color_type = bind.type,
                        .source_material_index =
                            static_cast<std::uint32_t>(bind.material),
                        .bind_ordinal = ordinal,
                    });
                    continue;
                }
                auto *output = material(static_cast<std::uint32_t>(bind.material));
                const auto *base = initialMaterial(
                    material_initial_values,
                    static_cast<std::uint32_t>(bind.material));
                if (!output || !base) {
                    resolve_status = Status::incompatible_layout;
                    return;
                }
                if (bind.type == "color") {
                    output->mask |= materialOverrideBaseColor;
                    for (glm::length_t component = 0; component < 4; ++component) {
                        output->base_color_factor[component] +=
                            (static_cast<float>(bind.target_value[component]) -
                             base->base_color_factor[component]) *
                            expression_weight;
                    }
                } else {
                    output->mask |= materialOverrideEmissive;
                    for (glm::length_t component = 0; component < 3; ++component) {
                        output->emissive_factor[component] +=
                            (static_cast<float>(bind.target_value[component]) -
                             base->emissive_factor[component]) *
                            expression_weight;
                    }
                }
            }

            for (const auto &bind : expression.texture_transform_binds) {
                if (bind.material < 0) {
                    resolve_status = Status::incompatible_layout;
                    return;
                }
                auto *output = material(static_cast<std::uint32_t>(bind.material));
                const auto *base = initialMaterial(
                    material_initial_values,
                    static_cast<std::uint32_t>(bind.material));
                if (!output || !base) {
                    resolve_status = Status::incompatible_layout;
                    return;
                }
                output->mask |= materialOverrideUvTransform;
                for (glm::length_t component = 0; component < 2; ++component) {
                    output->uv_offset[component] +=
                        (static_cast<float>(bind.offset[component]) -
                         base->uv_offset[component]) * expression_weight;
                    output->uv_scale[component] +=
                        (static_cast<float>(bind.scale[component]) -
                         base->uv_scale[component]) * expression_weight;
                }
            }
        });
        if (resolve_status != Status::ok) return resolve_status;
        if (std::any_of(next.morph_weights.begin(), next.morph_weights.end(),
                        [](float value) { return !std::isfinite(value); }))
            return Status::invalid_argument;
        for (auto &[_, value] : materials) {
            if (!finite(value)) return Status::invalid_argument;
            next.material_overrides.push_back(value);
        }
        resolved = std::move(next);
        return Status::ok;
    } catch (...) {
        return Status::out_of_memory;
    }
}

struct ApplicationServiceRuntime::Impl {
    enum class FrameStage { snapshot, look_at, resolved };
    struct InputState {
        Animation::InstanceHandle instance{};
        std::map<std::string, float, std::less<>> weights;
        std::uint32_t source_ordinal = 0;
        std::uint64_t input_revision = 0;
        float look_at_yaw_degrees = 0.0f;
        float look_at_pitch_degrees = 0.0f;
        std::uint32_t flags = expression_input_none;
        std::uint64_t asset_identity = 0;
        std::uint32_t asset_generation = 0;
        std::uint32_t profile_version = 0;
    };
    struct FrameState {
        ApplicationFrameHandle handle{};
        FrameStage stage = FrameStage::snapshot;
        ExpressionInputSnapshot snapshot;
        ResolvedExpressionFrame resolved;
    };

    std::recursive_mutex mutex;
    std::unordered_map<std::uint64_t, InputState> inputs;
    std::unordered_map<std::uint64_t, FrameState> frames;
    std::unordered_map<std::uint64_t, ApplicationFrameHandle> phase_frames;
    std::unordered_map<std::uint64_t, std::uint64_t> last_published_revisions;
    std::unordered_set<std::string> emitted_warnings;
    std::uint64_t next_frame_identity = 1;
    std::uint64_t registered_animation_generation = 0;

    static Impl *self(void *context) { return static_cast<Impl *>(context); }

    std::optional<VrmApplicationModelView> model(
        Animation::InstanceHandle instance) const {
        if (!FastModuleContainer::isInitialized<PolygonInstanceContainer>())
            return std::nullopt;
        return GET_MODULE(PolygonInstanceContainer).vrmApplicationModel(instance);
    }

    FrameState *find(ApplicationFrameHandle handle) {
        if (!isValid(handle)) return nullptr;
        const auto found = frames.find(handle.identity);
        return found != frames.end() && found->second.handle.generation == handle.generation
                   ? &found->second
                   : nullptr;
    }

    static Status setInputs(void *context,
                            const SetExpressionInputDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok)
            return status;
        if (desc->reserved2 != 0 || !Animation::isValid(desc->instance) ||
            desc->input_revision == 0 ||
            (desc->weight_count != 0 && !desc->weights) ||
            !std::isfinite(desc->look_at_yaw_degrees) ||
            !std::isfinite(desc->look_at_pitch_degrees) ||
            (desc->flags & ~(expression_input_look_at |
                             expression_input_reset_history |
                             expression_input_discontinuity)) != 0)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        const auto view = runtime->model(desc->instance);
        if (!view) return Status::stale_generation;
        const auto previous = runtime->inputs.find(desc->instance.identity);
        if (previous != runtime->inputs.end() &&
            sameInstance(previous->second.instance, desc->instance) &&
            desc->input_revision <= previous->second.input_revision)
            return Status::duplicate_revision;

        InputState next;
        next.instance = desc->instance;
        next.source_ordinal = desc->source_ordinal;
        next.input_revision = desc->input_revision;
        next.look_at_yaw_degrees = desc->look_at_yaw_degrees;
        next.look_at_pitch_degrees = desc->look_at_pitch_degrees;
        next.flags = desc->flags;
        for (std::uint32_t index = 0; index < desc->weight_count; ++index) {
            const auto &weight = desc->weights[index];
            if (weight.element_size < sizeof(ExpressionWeightV1) ||
                weight.version != applicationDescriptorVersionV1 ||
                weight.reserved0 != 0 ||
                (!weight.name && weight.name_size != 0) ||
                !std::isfinite(weight.value))
                return Status::invalid_argument;
            const auto name = std::string{weight.name ? weight.name : "",
                                          weight.name_size};
            if (name.empty() || !isExpression(*view->semantic, name))
                return Status::not_found;
            if (!next.weights.emplace(name, std::clamp(weight.value, 0.0f, 1.0f))
                     .second)
                return Status::invalid_argument;
        }
        if (previous != runtime->inputs.end() &&
            !sameInstance(previous->second.instance, desc->instance)) {
            for (auto frame = runtime->frames.begin();
                 frame != runtime->frames.end();) {
                if (frame->second.snapshot.instance.identity ==
                    desc->instance.identity)
                    frame = runtime->frames.erase(frame);
                else
                    ++frame;
            }
            runtime->phase_frames.erase(desc->instance.identity);
            runtime->last_published_revisions.erase(desc->instance.identity);
        }
        runtime->inputs.insert_or_assign(desc->instance.identity,
                                         std::move(next));
        return Status::ok;
    }

    static Status setTypedInputs(
        void *context, const SetTypedAnimationInputDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok)
            return status;
        if (desc->reserved2 != 0 || !Animation::isValid(desc->instance) ||
            desc->frame_revision == 0 || desc->asset_identity == 0 ||
            desc->asset_generation == 0 || desc->profile_version == 0 ||
            (desc->weight_count != 0 && !desc->weights) ||
            !std::isfinite(desc->look_at_yaw_degrees) ||
            !std::isfinite(desc->look_at_pitch_degrees) ||
            (desc->flags & ~(expression_input_look_at |
                             expression_input_reset_history |
                             expression_input_discontinuity)) != 0)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        const auto view = runtime->model(desc->instance);
        if (!view) return Status::stale_generation;
        const auto previous = runtime->inputs.find(desc->instance.identity);
        if (previous != runtime->inputs.end() &&
            sameInstance(previous->second.instance, desc->instance) &&
            desc->frame_revision <= previous->second.input_revision)
            return Status::duplicate_revision;

        InputState next;
        next.instance = desc->instance;
        next.source_ordinal = desc->source_ordinal;
        next.input_revision = desc->frame_revision;
        next.look_at_yaw_degrees = desc->look_at_yaw_degrees;
        next.look_at_pitch_degrees = desc->look_at_pitch_degrees;
        next.flags = desc->flags;
        next.asset_identity = desc->asset_identity;
        next.asset_generation = desc->asset_generation;
        next.profile_version = desc->profile_version;
        for (std::uint32_t index = 0; index < desc->weight_count; ++index) {
            const auto &weight = desc->weights[index];
            if (weight.element_size < sizeof(ExpressionWeightV1) ||
                weight.version != applicationDescriptorVersionV1 ||
                weight.reserved0 != 0 ||
                (!weight.name && weight.name_size != 0) ||
                !std::isfinite(weight.value))
                return Status::invalid_argument;
            const auto name = std::string{weight.name ? weight.name : "",
                                          weight.name_size};
            if (name.empty() || !isExpression(*view->semantic, name))
                return Status::not_found;
            if (!next.weights.emplace(
                    name, std::clamp(weight.value, 0.0f, 1.0f)).second)
                return Status::invalid_argument;
        }

        // parameter_snapshot may already have materialized the previous input
        // for this revision. Base-pose typed input replaces that immutable
        // snapshot before world_post_process, keeping all consumers on the
        // same frame revision.
        if (const auto pending = runtime->phase_frames.find(
                desc->instance.identity);
            pending != runtime->phase_frames.end()) {
            auto *frame = runtime->find(pending->second);
            if (!frame || frame->snapshot.frame_revision != desc->frame_revision)
                return Status::phase_order_error;
            if (frame->stage != FrameStage::snapshot)
                return Status::phase_order_error;
            frame->snapshot.instance = next.instance;
            frame->snapshot.input_revision = next.input_revision;
            frame->snapshot.frame_revision = desc->frame_revision;
            frame->snapshot.source_ordinal = next.source_ordinal;
            frame->snapshot.flags = next.flags;
            frame->snapshot.look_at_enabled =
                (next.flags & expression_input_look_at) != 0;
            frame->snapshot.look_at_yaw_degrees =
                next.look_at_yaw_degrees;
            frame->snapshot.look_at_pitch_degrees =
                next.look_at_pitch_degrees;
            frame->snapshot.expression_weights = next.weights;
        }
        runtime->inputs.insert_or_assign(desc->instance.identity,
                                         std::move(next));
        return Status::ok;
    }

    static Status snapshotInputs(void *context,
                                 SnapshotExpressionInputDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok)
            return status;
        if (!Animation::isValid(desc->instance) || desc->frame_revision == 0)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        const auto view = runtime->model(desc->instance);
        if (!view) return Status::stale_generation;
        const auto input = runtime->inputs.find(desc->instance.identity);
        if (input == runtime->inputs.end()) return Status::not_found;
        if (!sameInstance(input->second.instance, desc->instance))
            return Status::stale_generation;
        if (const auto last = runtime->last_published_revisions.find(
                desc->instance.identity);
            last != runtime->last_published_revisions.end() &&
            desc->frame_revision <= last->second)
            return Status::duplicate_revision;
        if (const auto pending = runtime->phase_frames.find(desc->instance.identity);
            pending != runtime->phase_frames.end()) {
            if (const auto *frame = runtime->find(pending->second);
                frame && frame->snapshot.frame_revision == desc->frame_revision)
                return Status::duplicate_revision;
        }

        FrameState frame;
        frame.handle = {runtime->next_frame_identity++, 1, 0};
        frame.snapshot.instance = desc->instance;
        frame.snapshot.input_revision = input->second.input_revision;
        frame.snapshot.frame_revision = desc->frame_revision;
        frame.snapshot.source_ordinal = input->second.source_ordinal;
        frame.snapshot.flags = input->second.flags;
        frame.snapshot.look_at_enabled =
            (input->second.flags & expression_input_look_at) != 0;
        frame.snapshot.look_at_yaw_degrees =
            input->second.look_at_yaw_degrees;
        frame.snapshot.look_at_pitch_degrees =
            input->second.look_at_pitch_degrees;
        frame.snapshot.expression_weights = input->second.weights;
        desc->snapshot = frame.handle;
        runtime->frames.emplace(frame.handle.identity, std::move(frame));
        return Status::ok;
    }

    static Status evaluateLookAt(void *context,
                                 const EvaluateExpressionLookAtDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok)
            return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        auto *frame = runtime->find(desc->snapshot);
        if (!frame) return Status::stale_generation;
        if (frame->stage != FrameStage::snapshot)
            return Status::phase_order_error;
        const auto view = runtime->model(frame->snapshot.instance);
        if (!view) return Status::stale_generation;
        const auto status = evaluateExpressionLookAt(*view->semantic,
                                                     frame->snapshot);
        if (status == Status::ok) frame->stage = FrameStage::look_at;
        return status;
    }

    static Status resolveFrame(void *context,
                               ResolveExpressionFrameDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok)
            return status;
        if (desc->reserved2 != 0) return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        auto *frame = runtime->find(desc->snapshot);
        if (!frame) return Status::stale_generation;
        if (frame->stage != FrameStage::look_at)
            return Status::phase_order_error;
        const auto view = runtime->model(frame->snapshot.instance);
        if (!view) return Status::stale_generation;
        const auto status = resolveExpressionFrame(
            *view->semantic, view->morph_layout.get(),
            view->material_initial_values.get(), frame->snapshot,
            frame->resolved);
        if (status != Status::ok) return status;
        frame->stage = FrameStage::resolved;
        desc->resolved = frame->handle;
        desc->expression_count = static_cast<std::uint32_t>(
            frame->resolved.expression_weights.size());
        desc->morph_weight_count = static_cast<std::uint32_t>(
            frame->resolved.morph_weights.size());
        desc->material_override_count = static_cast<std::uint32_t>(
            frame->resolved.material_overrides.size());
        desc->diagnostic_count = static_cast<std::uint32_t>(
            frame->resolved.diagnostics.size());
        desc->source_ordinal = frame->resolved.source_ordinal;
        desc->frame_revision = frame->resolved.frame_revision;

        for (const auto &diagnostic : frame->resolved.diagnostics) {
            const auto key = std::to_string(frame->resolved.instance.identity) + ":" +
                             std::to_string(frame->resolved.instance.generation) + ":" +
                             diagnostic.expression_name + ":" +
                             diagnostic.material_color_type + ":" +
                             std::to_string(diagnostic.source_material_index) + ":" +
                             std::to_string(diagnostic.bind_ordinal);
            if (runtime->emitted_warnings.insert(key).second && logger) {
                nlohmann::ordered_json warning{
                    {"code", "VRM_EXPRESSION_UNSUPPORTED_MATERIAL_COLOR_TYPE"},
                    {"expression", diagnostic.expression_name},
                    {"material", diagnostic.source_material_index},
                    {"type", diagnostic.material_color_type},
                    {"fallback", "bind omitted"},
                };
                LOG_WARNING(logger, "{}", warning.dump());
            }
        }
        return Status::ok;
    }

    static Status queryWeight(void *context,
                              QueryResolvedExpressionWeightDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok)
            return status;
        if (desc->reserved2 != 0 || desc->reserved3 != 0 ||
            (!desc->name && desc->name_size != 0))
            return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        const auto *frame = runtime->find(desc->resolved);
        if (!frame) return Status::stale_generation;
        if (frame->stage != FrameStage::resolved)
            return Status::phase_order_error;
        const auto name = std::string_view{desc->name ? desc->name : "",
                                           desc->name_size};
        const auto found = frame->resolved.expression_weights.find(name);
        if (found == frame->resolved.expression_weights.end())
            return Status::not_found;
        desc->value = found->second;
        return Status::ok;
    }

    static Status getDiagnostic(void *context,
                                GetApplicationDiagnosticDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok)
            return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        const auto *frame = runtime->find(desc->resolved);
        if (!frame) return Status::stale_generation;
        if (frame->stage != FrameStage::resolved)
            return Status::phase_order_error;
        if (desc->diagnostic_index >= frame->resolved.diagnostics.size())
            return Status::not_found;
        const auto &diagnostic =
            frame->resolved.diagnostics[desc->diagnostic_index];
        desc->code = diagnostic.code;
        desc->source_material_index = diagnostic.source_material_index;
        desc->bind_ordinal = diagnostic.bind_ordinal;
        desc->expression_name_size = static_cast<std::uint32_t>(
            diagnostic.expression_name.size());
        desc->material_color_type_size = static_cast<std::uint32_t>(
            diagnostic.material_color_type.size());
        const auto expression_fits = desc->expression_name &&
            desc->expression_name_capacity > diagnostic.expression_name.size();
        const auto type_fits = desc->material_color_type &&
            desc->material_color_type_capacity > diagnostic.material_color_type.size();
        if (!expression_fits || !type_fits) return Status::buffer_too_small;
        copyString(diagnostic.expression_name, desc->expression_name,
                   desc->expression_name_capacity);
        copyString(diagnostic.material_color_type, desc->material_color_type,
                   desc->material_color_type_capacity);
        return Status::ok;
    }

    static Status publish(void *context,
                          const PublishApplicationFrameDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok)
            return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        auto *frame = runtime->find(desc->resolved);
        if (!frame) return Status::stale_generation;
        if (frame->stage != FrameStage::resolved)
            return Status::phase_order_error;
        const auto view = runtime->model(frame->resolved.instance);
        if (!view) return Status::stale_generation;

        PublishVrmApplicationTransactionDescV1 transaction;
        std::vector<PublishMaterialInstanceAbsoluteOverrideDescV2> materials;
        if (view->morph_layout) {
            transaction.publish_morph = true;
            transaction.morph.layout_generation =
                frame->resolved.morph_layout_generation;
            transaction.morph.frame_revision = frame->resolved.frame_revision;
            transaction.morph.weights = frame->resolved.morph_weights.data();
            transaction.morph.weight_count = static_cast<std::uint32_t>(
                frame->resolved.morph_weights.size());
            if ((frame->resolved.flags & expression_input_reset_history) != 0)
                transaction.morph.flags |= morphCommitResetHistory;
            if ((frame->resolved.flags & expression_input_discontinuity) != 0)
                transaction.morph.flags |= morphCommitDiscontinuity;
        }
        materials.reserve(frame->resolved.material_overrides.size());
        for (const auto &source : frame->resolved.material_overrides) {
            PublishMaterialInstanceAbsoluteOverrideDescV2 output;
            output.instance = frame->resolved.instance;
            output.frame_revision = frame->resolved.frame_revision;
            output.source_material_index = source.source_material_index;
            output.values.mask = source.mask;
            output.values.base_color_factor = source.base_color_factor;
            output.values.emissive_factor = source.emissive_factor;
            output.values.uv_offset = source.uv_offset;
            output.values.uv_scale = source.uv_scale;
            output.values.uv_rotation = source.uv_rotation;
            if ((frame->resolved.flags & expression_input_reset_history) != 0)
                output.flags |= materialOverrideCommitResetHistory;
            if ((frame->resolved.flags & expression_input_discontinuity) != 0)
                output.flags |= materialOverrideCommitDiscontinuity;
            materials.push_back(output);
        }
        transaction.material_overrides = materials;
        const auto status = GET_MODULE(PolygonInstanceContainer)
                                .publishVrmApplicationTransaction(
                                    view->model_instance, transaction);
        if (status != Status::ok) return status;

        runtime->last_published_revisions[frame->resolved.instance.identity] =
            frame->resolved.frame_revision;
        if (const auto input = runtime->inputs.find(
                frame->resolved.instance.identity);
            input != runtime->inputs.end() &&
            input->second.input_revision == frame->resolved.input_revision) {
            input->second.flags &= ~(expression_input_reset_history |
                                     expression_input_discontinuity);
        }
        if (const auto phase = runtime->phase_frames.find(
                frame->resolved.instance.identity);
            phase != runtime->phase_frames.end() &&
            sameApplicationFrame(phase->second, frame->handle))
            runtime->phase_frames.erase(phase);
        runtime->frames.erase(desc->resolved.identity);
        return Status::ok;
    }

    static Status discard(void *context,
                          const DiscardApplicationFrameDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok)
            return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        auto *frame = runtime->find(desc->frame);
        if (!frame) return Status::stale_generation;
        if (const auto phase = runtime->phase_frames.find(
                frame->snapshot.instance.identity);
            phase != runtime->phase_frames.end() &&
            sameApplicationFrame(phase->second, frame->handle))
            runtime->phase_frames.erase(phase);
        runtime->frames.erase(desc->frame.identity);
        return Status::ok;
    }

    Status snapshotForPhase(const Animation::AnimationPhaseContextV1 &context) {
        SnapshotExpressionInputDescV1 request;
        request.instance = context.instance;
        request.frame_revision = context.frame_revision;
        const auto status = snapshotInputs(this, &request);
        if (status == Status::not_found || status == Status::stale_generation)
            return Status::ok;
        if (status == Status::duplicate_revision) return Status::ok;
        if (status != Status::ok) return status;
        phase_frames[context.instance.identity] = request.snapshot;
        return Status::ok;
    }

    Status lookAtForPhase(const Animation::AnimationPhaseContextV1 &context) {
        auto found = phase_frames.find(context.instance.identity);
        if (found == phase_frames.end()) {
            if (const auto status = snapshotForPhase(context);
                status != Status::ok)
                return status;
            found = phase_frames.find(context.instance.identity);
            if (found == phase_frames.end()) return Status::ok;
        }
        EvaluateExpressionLookAtDescV1 request;
        request.snapshot = found->second;
        if (const auto status = evaluateLookAt(this, &request);
            status != Status::ok)
            return status;
        auto *frame = find(found->second);
        if (!frame) return Status::stale_generation;
        const auto view = model(frame->snapshot.instance);
        if (!view) return Status::stale_generation;
        BoneLookAtRotations rotations;
        if (const auto status = evaluateBoneLookAt(
                *view->semantic, frame->snapshot, rotations);
            status != Status::ok || !rotations.active)
            return status;

        Animation::PoseStagingServiceV1 staging;
        if (const auto status = Animation::getPoseStagingServiceV1(
                Animation::poseStagingServiceVersionV1, &staging);
            status != Status::ok)
            return status;
        Animation::AcquireStagedPoseDescV1 acquire;
        acquire.instance = context.instance;
        acquire.frame_revision = context.frame_revision;
        const auto acquire_status =
            staging.acquire_staged_pose(staging.context, &acquire);
        if (acquire_status == Status::not_found) return Status::ok;
        if (acquire_status != Status::ok) return acquire_status;

        const auto apply = [&](std::string_view name, const glm::quat &rotation,
                               bool present) -> Status {
            if (!present) return Status::ok;
            const auto *bone = humanBone(*view->semantic, name);
            if (!bone || bone->node < 0) return Status::not_found;
            Animation::ResolveStagedSourceNodeDescV1 resolve;
            resolve.stage = acquire.stage;
            resolve.source_node_index =
                static_cast<std::uint32_t>(bone->node);
            if (const auto status = staging.resolve_source_node(
                    staging.context, &resolve);
                status != Status::ok)
                return status;
            if (resolve.layout_node_index >= acquire.local_pose.joint_count)
                return Status::incompatible_layout;
            acquire.local_pose.rotations[resolve.layout_node_index] = {
                rotation.x, rotation.y, rotation.z, rotation.w};
            return Status::ok;
        };
        if (const auto status = apply("leftEye", rotations.left_eye,
                                      rotations.has_left_eye);
            status != Status::ok)
            return status;
        return apply("rightEye", rotations.right_eye,
                     rotations.has_right_eye);
    }

    Status resolveForPhase(const Animation::AnimationPhaseContextV1 &context) {
        const auto found = phase_frames.find(context.instance.identity);
        if (found == phase_frames.end()) return Status::ok;
        ResolveExpressionFrameDescV1 request;
        request.snapshot = found->second;
        return resolveFrame(this, &request);
    }

    Status commitForPhase(const Animation::AnimationPhaseContextV1 &context) {
        const auto found = phase_frames.find(context.instance.identity);
        if (found == phase_frames.end()) return Status::ok;
        PublishApplicationFrameDescV1 request;
        request.resolved = found->second;
        return publish(this, &request);
    }

    static Status parameterPhase(
        void *context, const Animation::AnimationPhaseContextV1 *phase) {
        if (!context || !phase ||
            phase->phase != Animation::Phase::parameter_snapshot)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->snapshotForPhase(*phase);
    }

    static Status lookAtPhase(
        void *context, const Animation::AnimationPhaseContextV1 *phase) {
        if (!context || !phase ||
            phase->phase != Animation::Phase::world_post_process)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->lookAtForPhase(*phase);
    }

    static Status resolvePhase(
        void *context, const Animation::AnimationPhaseContextV1 *phase) {
        if (!context || !phase ||
            phase->phase != Animation::Phase::world_post_process)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->resolveForPhase(*phase);
    }

    static Status commitPhase(
        void *context, const Animation::AnimationPhaseContextV1 *phase) {
        if (!context || !phase || phase->phase != Animation::Phase::commit)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->commitForPhase(*phase);
    }

    Status ensureRegistered() {
        auto &animation_runtime = Animation::animationServiceRuntime();
        const auto generation = animation_runtime.registrationGeneration();
        if (registered_animation_generation == generation) return Status::ok;

        Animation::ApiV1 api{};
        api.struct_size = sizeof(api);
        api.version = Animation::descriptorVersionV1;
        if (const auto status = Animation::getApiV1(Animation::abiVersionV1, &api);
            status != Status::ok)
            return status;
        Animation::AnimationServiceV1 service{};
        service.struct_size = sizeof(service);
        service.version = Animation::descriptorVersionV1;
        if (!api.get_animation_service) return Status::unsupported_version;
        if (const auto status = api.get_animation_service(
                api.context, Animation::animationServiceVersionV1, &service);
            status != Status::ok)
            return status;
        Animation::CurrentAnimationOwnerDescV1 owner{};
        owner.struct_size = sizeof(owner);
        owner.version = Animation::descriptorVersionV1;
        internal::ScopedRegistrationOwner engine_owner{
            internal::engineRegistrationOwner};
        if (const auto status = service.get_current_owner(service.context, &owner);
            status != Status::ok)
            return status;

        struct Registration {
            Animation::Phase phase;
            std::int32_t priority;
            std::uint64_t identity;
            Animation::AnimationPhaseCallbackV1 callback;
        };
        const Registration registrations[]{
            {Animation::Phase::parameter_snapshot,
             parameterSnapshotPhasePriorityV1,
             parameterSnapshotRegistrationIdentityV1, parameterPhase},
            {Animation::Phase::world_post_process,
             expressionLookAtPhasePriorityV1,
             expressionLookAtRegistrationIdentityV1, lookAtPhase},
            {Animation::Phase::world_post_process,
             expressionResolvePhasePriorityV1,
             expressionResolveRegistrationIdentityV1, resolvePhase},
            {Animation::Phase::commit, applicationCommitPhasePriorityV1,
             applicationCommitRegistrationIdentityV1, commitPhase},
        };
        std::vector<Animation::PhaseRegistrationHandle> installed;
        for (const auto &item : registrations) {
            Animation::RegisterAnimationPhaseDescV1 request{};
            request.struct_size = sizeof(request);
            request.version = Animation::descriptorVersionV1;
            request.owner = owner.owner;
            request.registration.struct_size = sizeof(request.registration);
            request.registration.version = Animation::descriptorVersionV1;
            request.registration.phase = item.phase;
            request.registration.priority = item.priority;
            request.registration.registration_identity = item.identity;
            request.registration.registration_generation = owner.owner.generation;
            request.registration.source_ordinal =
                standardApplicationSourceOrdinalV1;
            request.callback = item.callback;
            request.user_context = this;
            const auto status = service.register_phase(service.context, &request);
            if (status != Status::ok) {
                for (const auto handle : installed) {
                    Animation::UnregisterAnimationPhaseDescV1 undo{};
                    undo.struct_size = sizeof(undo);
                    undo.version = Animation::descriptorVersionV1;
                    undo.owner = owner.owner;
                    undo.registration = handle;
                    (void)service.unregister_phase(service.context, &undo);
                }
                return status;
            }
            installed.push_back(request.registration_handle);
        }
        registered_animation_generation = generation;
        return Status::ok;
    }
};

ApplicationServiceRuntime::ApplicationServiceRuntime()
    : impl_(std::make_unique<Impl>()) {}
ApplicationServiceRuntime::~ApplicationServiceRuntime() = default;

Status ApplicationServiceRuntime::ensureStandardPhasesRegistered() noexcept {
    try {
        std::scoped_lock lock{impl_->mutex};
        return impl_->ensureRegistered();
    } catch (...) {
        return Status::out_of_memory;
    }
}

void ApplicationServiceRuntime::reset() noexcept {
    try {
        std::scoped_lock lock{impl_->mutex};
        impl_->inputs.clear();
        impl_->frames.clear();
        impl_->phase_frames.clear();
        impl_->last_published_revisions.clear();
        impl_->emitted_warnings.clear();
        ++impl_->next_frame_identity;
    } catch (...) {
    }
}

ApplicationServiceRuntime &applicationServiceRuntime() {
    static auto *runtime = new ApplicationServiceRuntime();
    return *runtime;
}

Status getApplicationServiceV1(std::uint32_t client_service_version,
                               ApplicationServiceV1 *out_service) noexcept {
    if (!out_service || out_service->struct_size <
                            offsetof(ApplicationServiceV1, context) +
                                sizeof(void *))
        return Status::invalid_argument;
    if (out_service->version != applicationDescriptorVersionV1)
        return Status::unsupported_version;
    if (out_service->reserved0 != 0 || out_service->reserved1 != 0)
        return Status::reserved_not_zero;
    if (client_service_version != applicationServiceVersionV1)
        return Status::unsupported_version;
    try {
        if (const auto status =
                applicationServiceRuntime().ensureStandardPhasesRegistered();
            status != Status::ok)
            return status;
        const auto caller_size = out_service->struct_size;
        ApplicationServiceV1 produced;
        produced.service_version = applicationServiceVersionV1;
        produced.minimum_client_service_version = 1;
        produced.capability_bits = applicationServiceCapabilitiesV1;
        produced.context = applicationServiceRuntime().impl_.get();
        produced.set_expression_inputs = ApplicationServiceRuntime::Impl::setInputs;
        produced.snapshot_expression_inputs =
            ApplicationServiceRuntime::Impl::snapshotInputs;
        produced.evaluate_expression_look_at =
            ApplicationServiceRuntime::Impl::evaluateLookAt;
        produced.resolve_expression_frame =
            ApplicationServiceRuntime::Impl::resolveFrame;
        produced.query_expression_weight =
            ApplicationServiceRuntime::Impl::queryWeight;
        produced.get_diagnostic =
            ApplicationServiceRuntime::Impl::getDiagnostic;
        produced.publish_application_frame =
            ApplicationServiceRuntime::Impl::publish;
        produced.discard_application_frame =
            ApplicationServiceRuntime::Impl::discard;
        produced.set_typed_animation_inputs =
            ApplicationServiceRuntime::Impl::setTypedInputs;
        std::memcpy(out_service, &produced,
                    std::min<std::size_t>(caller_size, sizeof(produced)));
        return Status::ok;
    } catch (...) {
        return Status::out_of_memory;
    }
}

} // namespace Pelican::Vrm
