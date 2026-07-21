#include "animationservice.hpp"

#include "animationjobs.hpp"
#include "animationprobe.hpp"
#include "animationserviceabi.hpp"
#include "vrmaretarget.hpp"
#include "../asset/model.hpp"
#include "../container.hpp"
#include "../ecs/core.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../loader/scene.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../userpublic/components/predefined.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican::AnimationGraph::Internal {
void linkAnchor() noexcept;
}

namespace Pelican::Animation {
namespace {

using Internal::checkedString;
using Internal::decodeSha256;
using Internal::descriptorHeaderSize;
using Internal::sameHandle;
using Internal::validateDescriptor;

} // namespace

struct AnimationServiceRuntime::Impl {
    struct ObjectRecord {
        std::string name;
        const SkeletalModelData *model{};
        const AnimationAsset *asset{};
        ModelAssetId logical_asset{};
        std::optional<ModelInstanceId> renderer_instance;
    };
    struct SinkRecord {
        AnimationSinkHandle handle{};
        ObjectRecord *object{};
        AnimationSinkKind kind{AnimationSinkKind::skeletal_pose};
        InstanceHandle instance{};
        AnimationSourceHandle active_source{};
        bool reset_history{};
        std::uint64_t last_notification_revision{};
    };
    struct OwnerRecord {
        std::uint32_t generation{1};
        bool active{true};
    };
    struct ArenaRecord {
        PoseArenaHandle handle{};
        AnimationOwnerHandle owner{};
        std::thread::id thread;
        std::unique_ptr<ProbeRuntime> runtime;
        PoseArenaHandle internal_arena{};
        std::vector<std::uint64_t> poses;
    };
    struct PoseRecord {
        PoseViewV1 view{};
        std::uint64_t arena_identity{};
    };
    struct CursorRecord {
        CursorHandle handle{};
        AnimationOwnerHandle owner{};
        ClipHandle clip{};
        bool active{true};
    };
    struct VrmaClipRecord {
        ClipHandle handle{};
        ObjectRecord *object{};
        std::string name;
        std::shared_ptr<const VrmaRetargetedClip> clip;
        RigHandle target_rig{};
        PoseLayoutHandle target_layout{};
    };
    struct SourceRecord {
        AnimationSourceHandle handle{};
        AnimationOwnerHandle owner{};
        AnimationSinkHandle sink{};
        std::uint32_t ordinal{};
        AnimationSourceAuthorityV1 authority{
            AnimationSourceAuthorityV1::graph_apply};
        bool active{true};
    };
    struct PhaseRecord {
        PhaseRegistrationHandle handle{};
        AnimationOwnerHandle owner{};
        PhaseRegistrationV1 registration{};
        AnimationPhaseCallbackV1 callback{};
        void *user_context{};
        bool active{true};
    };
    struct BlockedCommit {
        std::uint64_t instance_identity{};
        std::uint32_t instance_generation{};
        std::uint64_t revision{};
    };
    struct StagedFrame {
        PoseStageHandle handle{};
        AnimationSinkHandle sink{};
        ObjectRecord *object{};
        PublishAnimationFrameDescV1 frame{};
        std::vector<Matrix4fV1> palette;
        bool pose_accessed{};
    };

    std::recursive_mutex mutex;
    AnimationAssetRegistry assets;
    ProbeRuntime legacy_runtime;
    std::unordered_map<std::string, std::unique_ptr<ObjectRecord>> objects;
    std::unordered_map<std::uint64_t, ObjectRecord *> rigs;
    std::unordered_map<std::uint64_t, std::pair<ObjectRecord *, const AnimationClipResource *>> clips;
    std::unordered_map<std::uint64_t, std::pair<ObjectRecord *, const AnimationSkinBinding *>> bindings;
    // Includes tombstones so a handle from a replaced/removed asset reports
    // stale_generation rather than degrading to invalid_handle.
    Internal::ResourceGenerationLedger resource_generations;
    std::unordered_map<std::uint64_t, SinkRecord> sinks;
    std::unordered_map<std::uint64_t, std::uint64_t> instances;
    std::unordered_map<std::uint64_t, OwnerRecord> owners;
    std::unordered_map<std::uint64_t, ArenaRecord> arenas;
    std::unordered_map<std::uint64_t, PoseRecord> poses;
    std::unordered_map<std::uint64_t, std::uint32_t> stale_poses;
    std::unordered_map<std::uint64_t, CursorRecord> cursors;
    std::unordered_map<std::uint64_t, VrmaClipRecord> vrma_clips;
    std::unordered_map<std::uint64_t, SourceRecord> sources;
    std::unordered_map<std::uint64_t, PhaseRecord> phases;
    std::vector<BlockedCommit> blocked_commits;
    std::optional<AnimationSinkHandle> active_phase_sink;
    std::uint64_t active_phase_revision{};
    Phase active_phase{Phase::parameter_snapshot};
    std::optional<StagedFrame> staged_frame;
    std::uint64_t next_identity{1};
    std::uint64_t next_vrma_identity{1ull << 63u};
    std::uint64_t registration_generation{1};

    AnimationOwnerHandle ensureOwner(internal::RegistrationOwner internal_owner) {
        const auto identity = internal_owner + 1;
        auto &owner = owners[identity];
        if (!owner.active) return {identity, owner.generation, 0};
        return {identity, owner.generation, 0};
    }

    Status validateOwner(AnimationOwnerHandle owner) const {
        if (!isValid(owner)) return Status::invalid_handle;
        const auto found = owners.find(owner.identity);
        if (found == owners.end()) return Status::invalid_handle;
        if (!found->second.active || found->second.generation != owner.generation)
            return Status::stale_generation;
        return Status::ok;
    }

    Status validateResource(std::uint64_t identity, std::uint32_t generation) const {
        return resource_generations.validate(identity, generation);
    }

    Status resolveRigResource(RigHandle rig, ObjectRecord *&out) {
        out = nullptr;
        if (!isValid(rig)) return Status::invalid_handle;
        if (const auto status = validateResource(rig.identity, rig.generation);
            status != Status::ok)
            return status;
        const auto found = rigs.find(rig.identity);
        if (found == rigs.end() || !found->second->asset)
            return Status::invalid_handle;
        const auto &current = found->second->asset->rig;
        if (!sameHandle(current.handle, rig) ||
            current.generation_state->current.load() != rig.generation)
            return Status::stale_generation;
        out = found->second;
        return Status::ok;
    }

    Status resolveClipResource(
        ClipHandle clip,
        std::pair<ObjectRecord *, const AnimationClipResource *> *&out) {
        out = nullptr;
        if (!isValid(clip)) return Status::invalid_handle;
        if (const auto status = validateResource(clip.identity, clip.generation);
            status != Status::ok)
            return status;
        const auto found = clips.find(clip.identity);
        if (found == clips.end()) return Status::invalid_handle;
        const auto *resource = found->second.second;
        if (!sameHandle(resource->handle, clip) ||
            resource->generation_state->current.load() != clip.generation)
            return Status::stale_generation;
        out = &found->second;
        return Status::ok;
    }

    Status resolveVrmaClipResource(ClipHandle clip, VrmaClipRecord *&out) {
        out = nullptr;
        if (!isValid(clip)) return Status::invalid_handle;
        const auto found = vrma_clips.find(clip.identity);
        if (found == vrma_clips.end()) return Status::invalid_handle;
        auto &resource = found->second;
        if (resource.handle.generation != clip.generation)
            return Status::stale_generation;
        if (!resource.clip || !resource.object || !resource.object->asset)
            return Status::stale_generation;
        const auto &rig = resource.object->asset->rig;
        if (!sameHandle(resource.target_rig, rig.handle) ||
            !sameHandle(resource.target_layout, rig.layout))
            return Status::stale_generation;
        out = &resource;
        return Status::ok;
    }

    Status validateVrmaRegistration(
        const ObjectRecord &object,
        const std::shared_ptr<const VrmaRetargetedClip> &clip) const {
        if (!clip || !object.asset || !object.model ||
            !(clip->end > clip->start) ||
            clip->target_rest_pose.size() != object.model->nodes.size() ||
            clip->profile.version == 0 ||
            clip->profile.provenance.profile_version == 0)
            return Status::invalid_argument;
        std::uint8_t source_hash[32]{};
        std::uint8_t target_hash[32]{};
        if (!decodeSha256(clip->profile.provenance.source_rig_sha256,
                          source_hash) ||
            !decodeSha256(clip->profile.provenance.target_rig_sha256,
                          target_hash))
            return Status::invalid_argument;
        for (const auto &channel : clip->body_channels) {
            if (channel.target_node < 0 ||
                static_cast<std::size_t>(channel.target_node) >=
                    object.model->nodes.size())
                return Status::incompatible_layout;
        }
        return Status::ok;
    }

    Status registerVrmaSource(
        std::string object_name, std::string source_name,
        std::shared_ptr<const VrmaRetargetedClip> clip) {
        std::scoped_lock lock{mutex};
        if (source_name.empty()) return Status::invalid_argument;
        const auto object = objects.find(object_name);
        if (object == objects.end()) return Status::not_found;
        if (const auto status = validateVrmaRegistration(*object->second, clip);
            status != Status::ok)
            return status;
        for (const auto &native : object->second->asset->clips)
            if (native.source && native.source->name == source_name)
                return Status::invalid_argument;
        for (const auto &[_, source] : vrma_clips)
            if (source.object == object->second.get() &&
                source.name == source_name)
                return Status::invalid_argument;
        VrmaClipRecord record;
        record.handle = {next_vrma_identity++, 1, 0};
        record.object = object->second.get();
        record.name = std::move(source_name);
        record.clip = std::move(clip);
        record.target_rig = record.object->asset->rig.handle;
        record.target_layout = record.object->asset->rig.layout;
        vrma_clips.emplace(record.handle.identity, std::move(record));
        return Status::ok;
    }

    Status reloadVrmaSource(
        std::string_view object_name, std::string_view source_name,
        std::shared_ptr<const VrmaRetargetedClip> replacement) {
        std::scoped_lock lock{mutex};
        const auto object = objects.find(std::string{object_name});
        if (object == objects.end()) return Status::not_found;
        if (const auto status =
                validateVrmaRegistration(*object->second, replacement);
            status != Status::ok)
            return status;
        auto found = std::find_if(
            vrma_clips.begin(), vrma_clips.end(), [&](const auto &entry) {
                return entry.second.object == object->second.get() &&
                       entry.second.name == source_name;
            });
        if (found == vrma_clips.end()) return Status::not_found;
        auto &resource = found->second;
        for (auto &[_, cursor] : cursors) {
            if (!cursor.active ||
                cursor.clip.identity != resource.handle.identity)
                continue;
            (void)legacy_runtime.destroyCursor(cursor.handle);
            cursor.active = false;
        }
        if (++resource.handle.generation == 0)
            ++resource.handle.generation;
        resource.clip = std::move(replacement);
        resource.target_rig = object->second->asset->rig.handle;
        resource.target_layout = object->second->asset->rig.layout;
        return Status::ok;
    }

    SinkRecord *findSink(AnimationSinkHandle sink) {
        if (!isValid(sink)) return nullptr;
        const auto found = sinks.find(sink.identity);
        return found != sinks.end() && sameHandle(found->second.handle, sink) ? &found->second : nullptr;
    }

    SourceRecord *findSource(AnimationSourceHandle source) {
        if (!isValid(source)) return nullptr;
        const auto found = sources.find(source.identity);
        if (found == sources.end()) return nullptr;
        return found->second.active && sameHandle(found->second.handle, source) ? &found->second : nullptr;
    }

    PoseRecord *findPose(PoseHandle pose) {
        if (!isValid(pose)) return nullptr;
        const auto found = poses.find(pose.identity);
        if (found == poses.end() || !sameHandle(found->second.view.pose, pose)) return nullptr;
        return &found->second;
    }

    Status resolvePose(PoseHandle pose, PoseRecord *&out) {
        out = findPose(pose);
        if (out) return Status::ok;
        if (const auto stale = stale_poses.find(pose.identity);
            stale != stale_poses.end() && stale->second == pose.generation)
            return Status::stale_generation;
        return ProbeRuntime::validatePoseHandle(pose);
    }

    Status sampleVrma(
        const VrmaClipRecord &resource, double time_seconds, PoseRecord &pose,
        AnimationExpressionSampleV1 *expressions,
        std::uint32_t expression_capacity,
        std::uint32_t *expression_count,
        AnimationGazeSampleV1 *gaze) {
        if (!std::isfinite(time_seconds) || !resource.clip ||
            !resource.object || !resource.object->asset)
            return Status::invalid_argument;
        const auto required = static_cast<std::uint32_t>(
            resource.clip->expression_channels.size());
        if (expression_count) {
            *expression_count = required;
            if (required > expression_capacity ||
                (required != 0 && expressions == nullptr))
                return Status::buffer_too_small;
        }
        const auto &rig = resource.object->asset->rig;
        if (!sameHandle(pose.view.layout, resource.target_layout) ||
            pose.view.joint_count != rig.layout_to_original.size() ||
            !pose.view.translations || !pose.view.rotations ||
            !pose.view.scales)
            return Status::incompatible_layout;
        try {
            const auto sample = resource.clip->sample(
                static_cast<float>(time_seconds));
            if (sample.local_transforms.size() !=
                resource.object->model->nodes.size())
                return Status::incompatible_layout;
            for (std::size_t layout_node = 0;
                 layout_node < rig.layout_to_original.size(); ++layout_node) {
                const auto original_node = rig.layout_to_original[layout_node];
                if (original_node >= sample.local_transforms.size())
                    return Status::incompatible_layout;
                const auto &value = sample.local_transforms[original_node];
                pose.view.translations[layout_node] = {
                    value.translation.x, value.translation.y,
                    value.translation.z, 0.0f};
                pose.view.rotations[layout_node] = {
                    value.rotation.x, value.rotation.y, value.rotation.z,
                    value.rotation.w};
                pose.view.scales[layout_node] = {
                    value.scale.x, value.scale.y, value.scale.z, 0.0f};
            }
            if (sample.expressions.size() != required)
                return Status::invalid_argument;
            if (expression_count) {
                for (std::size_t index = 0;
                     index < sample.expressions.size(); ++index) {
                    const auto &value = sample.expressions[index];
                    const auto &stable_name =
                        resource.clip->expression_channels[index].expression;
                    expressions[index] = {
                        .element_size = sizeof(AnimationExpressionSampleV1),
                        .version = descriptorVersionV1,
                        .name = stable_name.data(),
                        .name_size = static_cast<std::uint32_t>(
                            stable_name.size()),
                        .weight = value.weight,
                        .preset = value.preset ? 1u : 0u,
                        .reserved0 = 0,
                    };
                }
            }
            if (gaze) {
                AnimationGazeSampleV1 produced{};
                produced.struct_size = sizeof(produced);
                produced.version = descriptorVersionV1;
                produced.rotation.w = 1.0f;
                if (sample.gaze) {
                    produced.present = 1;
                    produced.rotation = {
                        sample.gaze->rotation.x, sample.gaze->rotation.y,
                        sample.gaze->rotation.z, sample.gaze->rotation.w};
                    if (sample.gaze->offset_from_head_bone) {
                        produced.offset_present = 1;
                        produced.offset_from_head_bone = {
                            (*sample.gaze->offset_from_head_bone)[0],
                            (*sample.gaze->offset_from_head_bone)[1],
                            (*sample.gaze->offset_from_head_bone)[2], 0.0f};
                    }
                }
                *gaze = produced;
            }
            return Status::ok;
        } catch (const std::bad_alloc &) {
            return Status::out_of_memory;
        } catch (...) {
            return Status::invalid_argument;
        }
    }

    void registerObjectLocked(std::string name, const SkeletalModelData &model,
                              std::optional<ModelInstanceId> renderer_instance = std::nullopt,
                              ModelAssetId logical_asset = {}) {
        if (name.empty()) throw std::runtime_error("animation object name must not be empty");
        if (objects.contains(name)) throw std::runtime_error("animation object name is already registered");
        auto object = std::make_unique<ObjectRecord>();
        object->name = std::move(name);
        object->model = &model;
        object->asset = &assets.getOrCreate(model);
        object->logical_asset = logical_asset;
        object->renderer_instance = renderer_instance;
        auto *record = object.get();
        resource_generations.remember(record->asset->rig.handle.identity,
                                      record->asset->rig.handle.generation);
        resource_generations.remember(record->asset->rig.layout.identity,
                                      record->asset->rig.layout.generation);
        rigs.emplace(record->asset->rig.handle.identity, record);
        for (const auto &clip : record->asset->clips) {
            resource_generations.remember(clip.handle.identity,
                                          clip.handle.generation);
            clips.emplace(clip.handle.identity, std::pair{record, &clip});
        }
        for (const auto &binding : record->asset->skin_bindings) {
            resource_generations.remember(binding.handle.identity,
                                          binding.handle.generation);
            bindings.emplace(binding.handle.identity, std::pair{record, &binding});
        }
        objects.emplace(record->name, std::move(object));
    }

    void registerObject(std::string name, const SkeletalModelData &model) {
        std::scoped_lock lock{mutex};
        registerObjectLocked(std::move(name), model);
    }

    void reloadAsset(ModelAssetId logical_asset,
                     const SkeletalModelData *previous,
                     const SkeletalModelData *replacement) {
        if (!previous && !replacement) return;
        std::scoped_lock lock{mutex};
        std::unordered_set<ObjectRecord *> affected;
        for (auto &[_, object] : objects) {
            if ((previous != nullptr && object->model == previous) ||
                (isValidModelAssetId(logical_asset) &&
                 object->logical_asset == logical_asset)) {
                affected.insert(object.get());
            }
        }
        if (affected.empty()) return;

        const AnimationAsset *old_asset = nullptr;
        for (const auto *object : affected) {
            if (object->asset) {
                old_asset = object->asset;
                break;
            }
        }
        const auto generation_state =
            old_asset ? old_asset->rig.generation_state : nullptr;
        const auto old_layout_identity =
            old_asset ? old_asset->rig.layout.identity : 0;
        std::vector<std::uint64_t> old_resources;
        std::unordered_set<std::uint64_t> old_clips;
        if (old_asset) {
            old_resources = {old_asset->rig.handle.identity,
                             old_layout_identity};
            for (const auto &clip : old_asset->clips) {
                old_resources.push_back(clip.handle.identity);
                old_clips.insert(clip.handle.identity);
            }
            for (const auto &binding : old_asset->skin_bindings)
                old_resources.push_back(binding.handle.identity);
        }
        for (auto &[identity, source] : vrma_clips) {
            if (!affected.contains(source.object)) continue;
            old_clips.insert(identity);
            if (++source.handle.generation == 0)
                ++source.handle.generation;
        }

        const AnimationAsset *next_asset = nullptr;
        if (replacement) {
            if (old_asset) {
                const auto *previous_model =
                    previous ? previous : old_asset->source;
                next_asset = &assets.reloadAsset(*previous_model, *replacement);
            } else {
                next_asset = &assets.getOrCreate(*replacement);
            }
        } else if (old_asset) {
            const auto *previous_model = previous ? previous : old_asset->source;
            assets.invalidateAsset(*previous_model);
        }
        if (generation_state) {
            const auto current_generation =
                generation_state->current.load(std::memory_order_acquire);
            for (const auto identity : old_resources)
                resource_generations.remember(identity, current_generation);
        }

        for (auto &[_, cursor] : cursors) {
            if (!cursor.active || !old_clips.contains(cursor.clip.identity)) continue;
            (void)legacy_runtime.destroyCursor(cursor.handle);
            cursor.active = false;
        }
        for (auto iterator = poses.begin(); iterator != poses.end();) {
            if (old_layout_identity == 0 ||
                iterator->second.view.layout.identity != old_layout_identity) {
                ++iterator;
                continue;
            }
            stale_poses[iterator->first] = iterator->second.view.pose.generation;
            iterator = poses.erase(iterator);
        }

        std::erase_if(rigs, [&](const auto &entry) {
            return affected.contains(entry.second);
        });
        std::erase_if(clips, [&](const auto &entry) {
            return affected.contains(entry.second.first);
        });
        std::erase_if(bindings, [&](const auto &entry) {
            return affected.contains(entry.second.first);
        });

        for (auto *object : affected) {
            object->model = replacement;
            object->asset = next_asset;
        }
        if (next_asset) {
            resource_generations.remember(
                next_asset->rig.handle.identity,
                next_asset->rig.handle.generation);
            resource_generations.remember(
                next_asset->rig.layout.identity,
                next_asset->rig.layout.generation);
            for (auto *object : affected) {
                rigs.emplace(next_asset->rig.handle.identity, object);
                for (const auto &clip : next_asset->clips) {
                    resource_generations.remember(clip.handle.identity,
                                                  clip.handle.generation);
                    clips.emplace(clip.handle.identity, std::pair{object, &clip});
                }
                for (const auto &binding : next_asset->skin_bindings) {
                    resource_generations.remember(binding.handle.identity,
                                                  binding.handle.generation);
                    bindings.emplace(binding.handle.identity,
                                     std::pair{object, &binding});
                }
            }
        }

        std::unordered_set<std::uint64_t> affected_instances;
        for (auto &[_, sink] : sinks) {
            if (!affected.contains(sink.object)) continue;
            affected_instances.insert(sink.instance.identity);
            if (sink.object->renderer_instance) {
                if (auto *container =
                        FastModuleContainer::tryGet<PolygonInstanceContainer>()) {
                    sink.instance = container->animationInstance(
                        *sink.object->renderer_instance);
                } else if (++sink.instance.generation == 0) {
                    ++sink.instance.generation;
                }
            } else if (++sink.instance.generation == 0) {
                ++sink.instance.generation;
            }
            instances[sink.instance.identity] = sink.handle.identity;
            sink.reset_history = true;
        }
        std::erase_if(blocked_commits, [&](const auto &blocked) {
            return affected_instances.contains(blocked.instance_identity);
        });
        if (staged_frame && affected.contains(staged_frame->object))
            staged_frame.reset();
    }

    bool registerSceneObject(std::string_view name) {
        if (!FastModuleContainer::isInitialized<SceneLoader>() ||
            !FastModuleContainer::isInitialized<ECSCore>() ||
            !FastModuleContainer::isInitialized<ModelAssetContainer>() ||
            !FastModuleContainer::isInitialized<PolygonInstanceContainer>())
            return false;
        const auto object_id = GET_MODULE(SceneLoader).objectId(name);
        if (!object_id) return false;
        auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
        const auto *model_view = ecs.tryComponent<SimpleModelViewComponent>(*object_id);
        if (!model_view || !model_view->model_instance_id || model_view->model_name.empty()) return false;
        auto &model_template = GET_MODULE(ModelAssetContainer).getModelTemplateByName(model_view->model_name);
        if (!model_template.skeletal) return false;
        registerObjectLocked(std::string{name}, *model_template.skeletal,
                             *model_view->model_instance_id,
                             model_template.asset_id);
        return true;
    }

    void reset() {
        std::scoped_lock lock{mutex};
        const auto tombstone = [&](auto handle) {
            resource_generations.tombstone(handle.identity,
                                           handle.generation);
        };
        for (const auto &[_, object] : objects) {
            if (!object->asset) continue;
            tombstone(object->asset->rig.handle);
            tombstone(object->asset->rig.layout);
            for (const auto &clip : object->asset->clips) tombstone(clip.handle);
            for (const auto &binding : object->asset->skin_bindings)
                tombstone(binding.handle);
        }
        for (auto &[_, cursor] : cursors)
            if (cursor.active) legacy_runtime.destroyCursor(cursor.handle);
        cursors.clear();
        sources.clear();
        phases.clear();
        blocked_commits.clear();
        active_phase_sink.reset();
        active_phase_revision = 0;
        staged_frame.reset();
        for (const auto &[identity, pose] : poses)
            stale_poses[identity] = pose.view.pose.generation;
        poses.clear();
        arenas.clear();
        sinks.clear();
        instances.clear();
        bindings.clear();
        clips.clear();
        vrma_clips.clear();
        rigs.clear();
        objects.clear();
        assets.clear();
        owners.clear();
        ++next_identity;
        ++next_vrma_identity;
        if (++registration_generation == 0) ++registration_generation;
    }

    void releaseOwner(internal::RegistrationOwner internal_owner) noexcept {
        try {
            std::scoped_lock lock{mutex};
            const auto identity = internal_owner + 1;
            const auto found = owners.find(identity);
            if (found == owners.end() || !found->second.active) return;
            for (auto &[_, phase] : phases)
                if (phase.active && phase.owner.identity == identity) phase.active = false;
            for (auto &[_, source] : sources) {
                if (!source.active || source.owner.identity != identity) continue;
                if (auto *sink = findSink(source.sink); sink && sameHandle(sink->active_source, source.handle))
                    sink->active_source = invalidHandle<AnimationSourceHandle>();
                source.active = false;
                if (++source.handle.generation == 0) ++source.handle.generation;
            }
            for (auto &[_, cursor] : cursors) {
                if (!cursor.active || cursor.owner.identity != identity) continue;
                legacy_runtime.destroyCursor(cursor.handle);
                cursor.active = false;
            }
            std::vector<std::uint64_t> dead_arenas;
            for (const auto &[arena_identity, arena] : arenas)
                if (arena.owner.identity == identity) dead_arenas.push_back(arena_identity);
            for (const auto arena_identity : dead_arenas) {
                for (const auto pose_identity : arenas[arena_identity].poses) poses.erase(pose_identity);
                arenas.erase(arena_identity);
            }
            found->second.active = false;
            if (++found->second.generation == 0) ++found->second.generation;
        } catch (...) {
        }
    }

    Status runPhases(AnimationSinkHandle sink_handle, std::uint64_t revision) noexcept {
        try {
            std::scoped_lock lock{mutex};
            auto *sink = findSink(sink_handle);
            if (!sink || revision == 0) return Status::invalid_handle;
            if (active_phase_sink) return Status::phase_order_error;
            active_phase_sink = sink->handle;
            active_phase_revision = revision;
            staged_frame.reset();
            const auto clear_active = [&]() {
                staged_frame.reset();
                active_phase_sink.reset();
                active_phase_revision = 0;
            };
            std::vector<PhaseRecord *> ordered;
            for (auto &[_, phase] : phases)
                if (phase.active) ordered.push_back(&phase);
            std::sort(ordered.begin(), ordered.end(), [](const PhaseRecord *left, const PhaseRecord *right) {
                if (left->registration.phase != right->registration.phase)
                    return left->registration.phase < right->registration.phase;
                if (left->registration.priority != right->registration.priority)
                    return left->registration.priority < right->registration.priority;
                if (left->registration.registration_identity != right->registration.registration_identity)
                    return left->registration.registration_identity < right->registration.registration_identity;
                return left->registration.source_ordinal < right->registration.source_ordinal;
            });
            for (const auto *phase : ordered) {
                active_phase = phase->registration.phase;
                AnimationPhaseContextV1 context{};
                context.struct_size = sizeof(context);
                context.version = descriptorVersionV1;
                context.phase = phase->registration.phase;
                context.sink = sink->handle;
                context.instance = sink->instance;
                context.frame_revision = revision;
                Status status = Status::callback_failed;
                try {
                    internal::ScopedRegistrationOwner owner_scope{phase->owner.identity - 1};
                    status = phase->callback(phase->user_context, &context);
                } catch (...) {
                    status = Status::callback_failed;
                }
                if (status != Status::ok) {
                    blocked_commits.push_back({sink->instance.identity, sink->instance.generation, revision});
                    clear_active();
                    return status == Status::ok ? Status::callback_failed : status;
                }
            }
            const auto commit_status = commitStagedFrame();
            if (commit_status != Status::ok) {
                blocked_commits.push_back(
                    {sink->instance.identity, sink->instance.generation, revision});
                clear_active();
                return commit_status;
            }
            clear_active();
            return Status::ok;
        } catch (...) {
            staged_frame.reset();
            active_phase_sink.reset();
            active_phase_revision = 0;
            return Status::out_of_memory;
        }
    }

    Status runAllPhases(std::uint64_t revision) noexcept {
        try {
            std::vector<AnimationSinkHandle> active_sinks;
            {
                std::scoped_lock lock{mutex};
                active_sinks.reserve(sinks.size());
                for (const auto &[_, sink] : sinks) active_sinks.push_back(sink.handle);
            }
            std::sort(active_sinks.begin(), active_sinks.end(), [](const auto &left, const auto &right) {
                return left.identity < right.identity;
            });
            for (const auto sink : active_sinks) {
                const auto status = runPhases(sink, revision);
                if (status != Status::ok) return status;
            }
            return Status::ok;
        } catch (...) {
            return Status::out_of_memory;
        }
    }

    static Impl *self(void *context) { return static_cast<Impl *>(context); }

    Status publishToSink(SinkRecord &sink, PublishAnimationFrameDescV1 &frame) {
        const auto status =
            sink.object->renderer_instance &&
                    FastModuleContainer::isInitialized<PolygonInstanceContainer>()
                ? GET_MODULE(PolygonInstanceContainer)
                      .publishAnimationFrame(*sink.object->renderer_instance, frame)
                : legacy_runtime.publishAnimationFrame(frame);
        if (status == Status::ok) sink.reset_history = false;
        return status;
    }

    Status commitStagedFrame() {
        if (!staged_frame) return Status::ok;
        auto staged = std::move(*staged_frame);
        staged_frame.reset();
        auto *sink = findSink(staged.sink);
        if (!sink || sink->object != staged.object)
            return Status::stale_generation;

        if (staged.pose_accessed) {
            auto *local = findPose(staged.frame.local_pose);
            auto *model = findPose(staged.frame.model_pose);
            if (!local || !model || !staged.object || !staged.object->asset)
                return Status::stale_generation;
            std::vector<Matrix4fV1> model_matrices(
                staged.object->asset->rig.rest_pose.size());
            if (const auto status = localToModel(
                    staged.object->asset->rig, local->view, model_matrices,
                    &model->view);
                status != Status::ok)
                return status;
            staged.palette.resize(staged.object->model->joint_nodes.size());
            if (const auto status = buildSkinPalette(
                    *staged.object->asset, model_matrices, staged.palette);
                status != Status::ok)
                return status;
        }
        staged.frame.palette = staged.palette.empty() ? nullptr
                                                       : staged.palette.data();
        staged.frame.palette_count =
            static_cast<std::uint32_t>(staged.palette.size());
        return publishToSink(*sink, staged.frame);
    }

    static Status acquireStagedPose(void *context,
                                    AcquireStagedPoseDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (desc->struct_size < sizeof(*desc)) return Status::invalid_argument;
        if (desc->version != poseStagingDescriptorVersionV1)
            return Status::unsupported_version;
        if (desc->reserved0 != 0 || desc->reserved1 != 0)
            return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (!isValid(desc->instance) || desc->frame_revision == 0)
            return Status::invalid_argument;
        if (!runtime->active_phase_sink || !runtime->staged_frame)
            return Status::not_found;
        auto &stage = *runtime->staged_frame;
        if (!sameHandle(stage.frame.instance, desc->instance) ||
            stage.frame.frame_revision != desc->frame_revision)
            return Status::not_found;
        auto *local = runtime->findPose(stage.frame.local_pose);
        auto *model = runtime->findPose(stage.frame.model_pose);
        if (!local || !model) return Status::stale_generation;
        stage.pose_accessed = true;
        desc->stage = stage.handle;
        desc->local_pose = local->view;
        desc->model_pose = model->view;
        return Status::ok;
    }

    static Status resolveStagedSourceNode(
        void *context, ResolveStagedSourceNodeDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (desc->struct_size < sizeof(*desc)) return Status::invalid_argument;
        if (desc->version != poseStagingDescriptorVersionV1)
            return Status::unsupported_version;
        if (desc->reserved0 != 0 || desc->reserved1 != 0)
            return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (!runtime->staged_frame ||
            !Animation::isValid(desc->stage) ||
            !sameHandle(runtime->staged_frame->handle, desc->stage))
            return Status::stale_generation;
        const auto *object = runtime->staged_frame->object;
        if (!object || !object->asset ||
            desc->source_node_index >=
                object->asset->rig.original_to_layout.size())
            return Status::not_found;
        const auto mapped =
            object->asset->rig.original_to_layout[desc->source_node_index];
        if (mapped == std::numeric_limits<std::uint32_t>::max())
            return Status::not_found;
        desc->layout_node_index = mapped;
        return Status::ok;
    }

    static Status apiAdvance(void *context, const AdvanceDescV1 *desc,
                             IntervalResultV1 *result) noexcept {
        if (!context || !desc || !result) return Status::invalid_argument;
        try {
            return self(context)->legacy_runtime.advanceCursor(*desc, *result);
        } catch (const std::bad_alloc &) {
            return Status::out_of_memory;
        } catch (...) {
            return Status::invalid_argument;
        }
    }
    static Status apiPublish(void *context, const PublishAnimationFrameDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        return self(context)->legacy_runtime.publishAnimationFrame(*desc);
    }
    static Status apiAdvanceHistory(void *context, const AdvanceTemporalHistoryDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        return self(context)->legacy_runtime.advanceTemporalHistoryAfterRender(*desc);
    }

    static Status getCurrentOwner(void *context, CurrentAnimationOwnerDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        desc->owner = runtime->ensureOwner(internal::currentRegistrationOwner());
        return Status::ok;
    }

    static Status resolveSink(void *context, ResolveAnimationSinkDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if ((desc->object_name == nullptr && desc->object_name_size != 0) ||
            desc->sink_kind > AnimationSinkKind::expression_curve)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        const auto object_name = std::string{checkedString(desc->object_name, desc->object_name_size)};
        auto object = runtime->objects.find(object_name);
        if (object == runtime->objects.end() && runtime->registerSceneObject(object_name))
            object = runtime->objects.find(object_name);
        if (object == runtime->objects.end()) return Status::not_found;
        for (const auto &[_, sink] : runtime->sinks) {
            if (sink.object == object->second.get() && sink.kind == desc->sink_kind) {
                desc->sink = sink.handle;
                return Status::ok;
            }
        }
        SinkRecord sink;
        sink.handle = {runtime->next_identity++, 1, 0};
        sink.object = object->second.get();
        sink.kind = desc->sink_kind;
        sink.instance = object->second->renderer_instance
                            ? GET_MODULE(PolygonInstanceContainer).animationInstance(*object->second->renderer_instance)
                            : InstanceHandle{runtime->next_identity++, 1, 0};
        runtime->instances.emplace(sink.instance.identity, sink.handle.identity);
        desc->sink = sink.handle;
        runtime->sinks.emplace(sink.handle.identity, sink);
        return Status::ok;
    }

    static Status resolveInstance(void *context, ResolveAnimationInstanceDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        auto *sink = runtime->findSink(desc->sink);
        if (!sink) return Status::invalid_handle;
        desc->instance = sink->instance;
        return Status::ok;
    }

    static Status resolveRig(void *context, ResolveAnimationRigDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (!isValid(desc->instance)) return Status::invalid_handle;
        const auto found = runtime->instances.find(desc->instance.identity);
        if (found == runtime->instances.end()) return Status::invalid_handle;
        auto *sink = runtime->findSink(runtime->sinks.at(found->second).handle);
        if (!sink || !sameHandle(sink->instance, desc->instance)) return Status::stale_generation;
        if (!sink->object || !sink->object->asset) return Status::not_found;
        desc->rig = sink->object->asset->rig.handle;
        return Status::ok;
    }

    static Status resolveLayout(void *context, ResolvePoseLayoutDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        ObjectRecord *object{};
        if (const auto status = runtime->resolveRigResource(desc->rig, object);
            status != Status::ok)
            return status;
        desc->layout = object->asset->rig.layout;
        desc->joint_count = static_cast<std::uint32_t>(object->asset->rig.rest_pose.size());
        desc->palette_count = static_cast<std::uint32_t>(object->model->joint_nodes.size());
        desc->skin_binding = object->asset->skin_bindings.empty()
                                 ? invalidHandle<SkinBindingHandle>()
                                 : object->asset->skin_bindings.front().handle;
        return Status::ok;
    }

    static Status resolveClip(void *context, ResolveAnimationClipDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0 || (desc->clip_name == nullptr && desc->clip_name_size != 0))
            return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        ObjectRecord *object{};
        if (const auto status = runtime->resolveRigResource(desc->rig, object);
            status != Status::ok)
            return status;
        const auto name = checkedString(desc->clip_name, desc->clip_name_size);
        for (const auto &clip : object->asset->clips) {
            if (clip.source && clip.source->name == name) {
                desc->clip = clip.handle;
                return Status::ok;
            }
        }
        for (const auto &[_, clip] : runtime->vrma_clips) {
            if (clip.object != object || clip.name != name) continue;
            if (!sameHandle(clip.target_rig, desc->rig))
                return Status::stale_generation;
            desc->clip = clip.handle;
            return Status::ok;
        }
        return Status::not_found;
    }

    static Status beginPoseFrame(void *context, BeginPoseArenaFrameDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->frame_revision == 0) return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (const auto status = runtime->validateOwner(desc->owner); status != Status::ok) return status;
        ArenaRecord *arena = nullptr;
        for (auto &[_, candidate] : runtime->arenas) {
            if (sameHandle(candidate.owner, desc->owner) && candidate.thread == std::this_thread::get_id()) {
                arena = &candidate;
                break;
            }
        }
        if (!arena) {
            ArenaRecord created;
            created.handle = {runtime->next_identity++, 1, 0};
            created.owner = desc->owner;
            created.thread = std::this_thread::get_id();
            created.runtime = std::make_unique<ProbeRuntime>();
            arena = &runtime->arenas.emplace(created.handle.identity, std::move(created)).first->second;
        } else {
            for (const auto pose_identity : arena->poses) runtime->poses.erase(pose_identity);
            arena->poses.clear();
            if (++arena->handle.generation == 0) ++arena->handle.generation;
        }
        arena->internal_arena = arena->runtime->beginFrame(desc->frame_revision);
        if (!isValid(arena->internal_arena)) return Status::wrong_thread;
        desc->arena = arena->handle;
        return Status::ok;
    }

    static Status acquirePose(void *context, AcquirePoseDescV1 *desc) {
        if (!context || !desc || !desc->out_view) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0) return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (const auto status = runtime->validateResource(
                desc->layout.identity, desc->layout.generation);
            status != Status::ok)
            return status;
        const auto found = runtime->arenas.find(desc->arena.identity);
        if (found == runtime->arenas.end() || !isValid(desc->arena)) return Status::invalid_handle;
        auto &arena = found->second;
        if (desc->arena.generation != arena.handle.generation) return Status::stale_generation;
        if (arena.thread != std::this_thread::get_id()) return Status::wrong_thread;
        auto produced = *desc->out_view;
        const auto status = arena.runtime->acquirePose(arena.internal_arena, desc->layout, desc->joint_count, produced);
        if (status != Status::ok) return status;
        *desc->out_view = produced;
        arena.poses.push_back(produced.pose.identity);
        runtime->poses.emplace(produced.pose.identity, PoseRecord{produced, arena.handle.identity});
        return Status::ok;
    }

    static Status getClipMetadata(void *context, ClipMetadataV1 *metadata) {
        if (!context || !metadata) return Status::invalid_argument;
        constexpr auto minimum_size =
            offsetof(ClipMetadataV1, reserved2) + sizeof(std::uint32_t);
        if (const auto status = validateDescriptor(*metadata, minimum_size);
            status != Status::ok)
            return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (auto vrma = runtime->vrma_clips.find(metadata->clip.identity);
            vrma != runtime->vrma_clips.end()) {
            VrmaClipRecord *resource{};
            if (const auto status = runtime->resolveVrmaClipResource(
                    metadata->clip, resource);
                status != Status::ok)
                return status;
            const auto caller_size = metadata->struct_size;
            ClipMetadataV1 produced{};
            produced.struct_size = sizeof(produced);
            produced.version = descriptorVersionV1;
            produced.clip = resource->handle;
            produced.source_rig = resource->target_rig;
            produced.start_seconds = resource->clip->start;
            produced.end_seconds = resource->clip->end;
            produced.wrap_mode = WrapMode::repeat;
            for (const auto &channel : resource->clip->body_channels)
                produced.channel_kind_mask |=
                    1u << static_cast<std::uint32_t>(
                        channel.path == VrmaBodyPath::translation
                            ? ChannelKind::translation
                            : ChannelKind::rotation);
            produced.annotation_identity = resource->handle.identity;
            produced.annotation_generation = resource->handle.generation;
            produced.sampling_context_generation =
                resource->handle.generation;
            produced.cursor_generation = resource->handle.generation;
            produced.clip_kind =
                AnimationClipKindV1::vrma_retargeted_clip;
            if (!resource->clip->expression_channels.empty())
                produced.typed_channel_flags |=
                    animation_typed_channel_expression;
            if (resource->clip->gaze_channel)
                produced.typed_channel_flags |= animation_typed_channel_gaze;
            produced.expression_channel_count =
                static_cast<std::uint32_t>(
                    resource->clip->expression_channels.size());
            produced.profile_version =
                resource->clip->profile.provenance.profile_version;
            produced.asset_identity = resource->handle.identity;
            produced.asset_generation = resource->handle.generation;
            if (!decodeSha256(
                    resource->clip->profile.provenance.source_rig_sha256,
                    produced.source_rig_sha256) ||
                !decodeSha256(
                    resource->clip->profile.provenance.target_rig_sha256,
                    produced.target_rig_sha256))
                return Status::invalid_argument;
            std::memcpy(metadata, &produced,
                        std::min<std::size_t>(caller_size,
                                              sizeof(produced)));
            return Status::ok;
        }
        std::pair<ObjectRecord *, const AnimationClipResource *> *found{};
        if (const auto status = runtime->resolveClipResource(metadata->clip, found);
            status != Status::ok)
            return status;
        const auto *clip = found->second;
        const auto caller_size = metadata->struct_size;
        ClipMetadataV1 produced{};
        produced.struct_size = sizeof(produced);
        produced.version = descriptorVersionV1;
        produced.clip = clip->handle;
        produced.source_rig = clip->source_rig;
        produced.start_seconds = clip->source->start;
        produced.end_seconds = clip->source->end;
        produced.wrap_mode = WrapMode::repeat;
        for (const auto &channel : clip->source->channels)
            produced.channel_kind_mask |= 1u << static_cast<std::uint32_t>(channel.path);
        produced.annotation_identity = clip->handle.identity;
        produced.annotation_generation = clip->handle.generation;
        produced.sampling_context_generation = clip->handle.generation;
        produced.cursor_generation = clip->handle.generation;
        produced.clip_kind = AnimationClipKindV1::skeletal_clip;
        produced.asset_identity = clip->handle.identity;
        produced.asset_generation = clip->handle.generation;
        std::memcpy(metadata, &produced, std::min<std::size_t>(caller_size, sizeof(produced)));
        return Status::ok;
    }

    static Status createCursor(void *context, CreateClipCursorDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (const auto status = runtime->validateOwner(desc->owner); status != Status::ok) return status;
        if (runtime->vrma_clips.contains(desc->clip.identity)) {
            VrmaClipRecord *resource{};
            if (const auto status = runtime->resolveVrmaClipResource(
                    desc->clip, resource);
                status != Status::ok)
                return status;
            const auto duration = resource->clip->end - resource->clip->start;
            const auto cursor = runtime->legacy_runtime.createCursor(
                duration, WrapMode::repeat);
            if (!isValid(cursor)) return Status::invalid_argument;
            runtime->cursors.emplace(
                cursor.identity,
                CursorRecord{cursor, desc->owner, desc->clip, true});
            desc->cursor = cursor;
            return Status::ok;
        }
        std::pair<ObjectRecord *, const AnimationClipResource *> *found{};
        if (const auto status = runtime->resolveClipResource(desc->clip, found);
            status != Status::ok)
            return status;
        const double duration = found->second->source->end - found->second->source->start;
        const auto cursor = runtime->legacy_runtime.createCursor(duration, WrapMode::repeat);
        if (!isValid(cursor)) return Status::invalid_argument;
        runtime->cursors.emplace(cursor.identity, CursorRecord{cursor, desc->owner, desc->clip, true});
        desc->cursor = cursor;
        return Status::ok;
    }

    static Status destroyCursor(void *context, const DestroyClipCursorDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (const auto status = runtime->validateOwner(desc->owner); status != Status::ok) return status;
        const auto found = runtime->cursors.find(desc->cursor.identity);
        if (found == runtime->cursors.end() || !sameHandle(found->second.handle, desc->cursor))
            return Status::invalid_handle;
        if (!found->second.active || !sameHandle(found->second.owner, desc->owner))
            return Status::stale_generation;
        const auto status = runtime->legacy_runtime.destroyCursor(desc->cursor);
        if (status == Status::ok) found->second.active = false;
        return status;
    }

    static Status samplePose(void *context, const SamplePoseAtDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (!std::isfinite(desc->time_seconds)) return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (runtime->vrma_clips.contains(desc->clip.identity)) {
            VrmaClipRecord *resource{};
            if (const auto status = runtime->resolveVrmaClipResource(
                    desc->clip, resource);
                status != Status::ok)
                return status;
            PoseRecord *pose{};
            if (const auto status = runtime->resolvePose(
                    desc->output_pose, pose);
                status != Status::ok)
                return status;
            return runtime->sampleVrma(*resource, desc->time_seconds, *pose,
                                       nullptr, 0, nullptr, nullptr);
        }
        std::pair<ObjectRecord *, const AnimationClipResource *> *clip{};
        if (const auto status = runtime->resolveClipResource(desc->clip, clip);
            status != Status::ok)
            return status;
        PoseRecord *pose{};
        if (const auto status = runtime->resolvePose(desc->output_pose, pose); status != Status::ok)
            return status;
        return samplePoseAt(*clip->first->asset, clip->second, desc->time_seconds, 1.0, true, 0.0, pose->view);
    }

    static Status sampleAnimationSource(
        void *context, SampleAnimationSourceAtDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc);
            status != Status::ok)
            return status;
        if (!std::isfinite(desc->time_seconds) ||
            (desc->expression_capacity != 0 && !desc->expressions))
            return Status::invalid_argument;
        if (const auto status = validateDescriptor(desc->gaze);
            status != Status::ok)
            return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        PoseRecord *pose{};
        if (const auto status = runtime->resolvePose(desc->output_pose, pose);
            status != Status::ok)
            return status;
        if (runtime->vrma_clips.contains(desc->clip.identity)) {
            VrmaClipRecord *resource{};
            if (const auto status = runtime->resolveVrmaClipResource(
                    desc->clip, resource);
                status != Status::ok)
                return status;
            return runtime->sampleVrma(
                *resource, desc->time_seconds, *pose, desc->expressions,
                desc->expression_capacity, &desc->expression_count,
                &desc->gaze);
        }
        std::pair<ObjectRecord *, const AnimationClipResource *> *clip{};
        if (const auto status = runtime->resolveClipResource(desc->clip, clip);
            status != Status::ok)
            return status;
        const auto status = samplePoseAt(
            *clip->first->asset, clip->second, desc->time_seconds, 1.0, true,
            0.0, pose->view);
        if (status != Status::ok) return status;
        desc->expression_count = 0;
        AnimationGazeSampleV1 gaze{};
        gaze.struct_size = sizeof(gaze);
        gaze.version = descriptorVersionV1;
        gaze.rotation.w = 1.0f;
        desc->gaze = gaze;
        return Status::ok;
    }

    static Status blendPoses(void *context, const BlendNormalDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0 || (desc->layer_count != 0 && desc->layers == nullptr))
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        ObjectRecord *object{};
        if (const auto status = runtime->resolveRigResource(desc->rig, object);
            status != Status::ok)
            return status;
        PoseRecord *output{};
        if (const auto status = runtime->resolvePose(desc->output_pose, output); status != Status::ok)
            return status;
        std::vector<NormalBlendInput> inputs;
        inputs.reserve(desc->layer_count);
        for (std::uint32_t i = 0; i < desc->layer_count; ++i) {
            const auto &layer = desc->layers[i];
            if (const auto status = validateDescriptor(layer); status != Status::ok) return status;
            if (layer.mode != BlendMode::normal || layer.additive_space != AdditiveSpace::local)
                return Status::invalid_argument;
            PoseRecord *pose{};
            if (const auto status = runtime->resolvePose(layer.pose, pose); status != Status::ok)
                return status;
            if (layer.joint_weight_count != 0 && layer.joint_weights == nullptr) return Status::invalid_argument;
            inputs.push_back({&pose->view, layer.weight,
                              {layer.joint_weights, layer.joint_weight_count}});
        }
        return blendNormal(object->asset->rig, inputs, output->view);
    }

    static Status localToModelJob(void *context, LocalToModelDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        ObjectRecord *object{};
        if (const auto status = runtime->resolveRigResource(desc->rig, object);
            status != Status::ok)
            return status;
        PoseRecord *local{};
        if (const auto status = runtime->resolvePose(desc->local_pose, local); status != Status::ok)
            return status;
        const auto required = static_cast<std::uint32_t>(object->asset->rig.rest_pose.size());
        desc->model_matrix_count = required;
        if (desc->model_matrix_capacity < required || !desc->model_matrices) return Status::buffer_too_small;
        PoseViewV1 *model_view = nullptr;
        if (isValid(desc->model_pose)) {
            PoseRecord *model{};
            if (const auto status = runtime->resolvePose(desc->model_pose, model); status != Status::ok)
                return status;
            model_view = &model->view;
        }
        return localToModel(object->asset->rig, local->view,
                            {desc->model_matrices, desc->model_matrix_capacity}, model_view);
    }

    static Status buildPalette(void *context, BuildSkinPaletteDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0) return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (!isValid(desc->skin_binding)) return Status::invalid_handle;
        if (const auto status = runtime->validateResource(
                desc->skin_binding.identity, desc->skin_binding.generation);
            status != Status::ok)
            return status;
        const auto found = runtime->bindings.find(desc->skin_binding.identity);
        if (found == runtime->bindings.end()) return Status::invalid_handle;
        if (!sameHandle(found->second.second->handle, desc->skin_binding))
            return Status::stale_generation;
        const auto required = static_cast<std::uint32_t>(found->second.first->model->joint_nodes.size());
        desc->palette_count = required;
        if (desc->model_matrix_count < found->second.first->asset->rig.rest_pose.size() || !desc->model_matrices)
            return Status::invalid_argument;
        if (desc->palette_capacity < required || !desc->palette) return Status::buffer_too_small;
        return buildSkinPalette(*found->second.first->asset,
                                {desc->model_matrices, desc->model_matrix_count},
                                {desc->palette, desc->palette_capacity});
    }

    static Status registerPhase(void *context, RegisterAnimationPhaseDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (const auto status = validateDescriptor(desc->registration); status != Status::ok) return status;
        if (!desc->callback || desc->registration.phase > Phase::commit ||
            desc->registration.registration_identity == 0 || desc->registration.registration_generation == 0)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (const auto status = runtime->validateOwner(desc->owner); status != Status::ok) return status;
        for (const auto &[_, existing] : runtime->phases) {
            if (!existing.active) continue;
            if (existing.registration.phase == desc->registration.phase &&
                existing.registration.priority == desc->registration.priority &&
                existing.registration.registration_identity == desc->registration.registration_identity &&
                existing.registration.source_ordinal == desc->registration.source_ordinal)
                return Status::phase_order_error;
        }
        PhaseRecord phase;
        phase.handle = {runtime->next_identity++, desc->owner.generation, 0};
        phase.owner = desc->owner;
        phase.registration = desc->registration;
        phase.callback = desc->callback;
        phase.user_context = desc->user_context;
        desc->registration_handle = phase.handle;
        runtime->phases.emplace(phase.handle.identity, phase);
        return Status::ok;
    }

    static Status unregisterPhase(void *context, const UnregisterAnimationPhaseDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (const auto status = runtime->validateOwner(desc->owner); status != Status::ok) return status;
        const auto found = runtime->phases.find(desc->registration.identity);
        if (found == runtime->phases.end()) return Status::invalid_handle;
        auto &phase = found->second;
        if (!phase.active || !sameHandle(phase.handle, desc->registration) ||
            !sameHandle(phase.owner, desc->owner))
            return Status::stale_generation;
        phase.active = false;
        if (++phase.handle.generation == 0) ++phase.handle.generation;
        return Status::ok;
    }

    static Status claimSource(void *context, ClaimAnimationSourceDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0) return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (const auto status = runtime->validateOwner(desc->owner); status != Status::ok) return status;
        auto *sink = runtime->findSink(desc->sink);
        if (!sink) return Status::invalid_handle;
        if (runtime->findSource(sink->active_source)) return Status::authority_conflict;
        SourceRecord source;
        source.handle = {runtime->next_identity++, desc->owner.generation, 0};
        source.owner = desc->owner;
        source.sink = desc->sink;
        source.ordinal = desc->source_ordinal;
        source.authority = AnimationSourceAuthorityV1::graph_apply;
        sink->active_source = source.handle;
        desc->source = source.handle;
        runtime->sources.emplace(source.handle.identity, source);
        return Status::ok;
    }

    static Status claimSourcePolicy(
        void *context, ClaimAnimationSourcePolicyDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc);
            status != Status::ok)
            return status;
        if (desc->authority >
            AnimationSourceAuthorityV1::timeline_extract_only)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (const auto status = runtime->validateOwner(desc->owner);
            status != Status::ok)
            return status;
        auto *sink = runtime->findSink(desc->sink);
        if (!sink) return Status::invalid_handle;
        if (desc->authority == AnimationSourceAuthorityV1::graph_apply &&
            runtime->findSource(sink->active_source))
            return Status::authority_conflict;
        SourceRecord source;
        source.handle = {runtime->next_identity++, desc->owner.generation, 0};
        source.owner = desc->owner;
        source.sink = desc->sink;
        source.ordinal = desc->source_ordinal;
        source.authority = desc->authority;
        if (source.authority == AnimationSourceAuthorityV1::graph_apply)
            sink->active_source = source.handle;
        desc->source = source.handle;
        runtime->sources.emplace(source.handle.identity, source);
        return Status::ok;
    }

    static Status releaseSource(void *context, const ReleaseAnimationSourceDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        if (const auto status = runtime->validateOwner(desc->owner); status != Status::ok) return status;
        auto *source = runtime->findSource(desc->source);
        if (!source) return Status::stale_generation;
        if (!sameHandle(source->owner, desc->owner)) return Status::authority_conflict;
        auto *sink = runtime->findSink(source->sink);
        if (!sink) return Status::authority_conflict;
        if (source->authority == AnimationSourceAuthorityV1::graph_apply) {
            if (!sameHandle(sink->active_source, source->handle))
                return Status::authority_conflict;
            sink->active_source = invalidHandle<AnimationSourceHandle>();
        }
        source->active = false;
        if (++source->handle.generation == 0) ++source->handle.generation;
        return Status::ok;
    }

    static Status handoffSource(void *context, HandoffAnimationSourceDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0) return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        auto *source = runtime->findSource(desc->current_source);
        if (!source) return Status::stale_generation;
        if (source->authority != AnimationSourceAuthorityV1::graph_apply)
            return Status::authority_conflict;
        if (const auto status = runtime->validateOwner(desc->next_owner); status != Status::ok) return status;
        auto *sink = runtime->findSink(source->sink);
        if (!sink || !sameHandle(sink->active_source, source->handle)) return Status::authority_conflict;
        SourceRecord next;
        next.handle = {runtime->next_identity++, desc->next_owner.generation, 0};
        next.owner = desc->next_owner;
        next.sink = source->sink;
        next.ordinal = desc->next_source_ordinal;
        next.authority = AnimationSourceAuthorityV1::graph_apply;
        source->active = false;
        if (++source->handle.generation == 0) ++source->handle.generation;
        sink->active_source = next.handle;
        desc->next_source = next.handle;
        runtime->sources.emplace(next.handle.identity, next);
        return Status::ok;
    }

    static Status notify(void *context, const AnimationNotificationDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0 || desc->kind > AnimationNotificationKind::layout_generation_mismatch ||
            !std::isfinite(desc->time_seconds) || desc->notification_revision == 0)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        auto *source = runtime->findSource(desc->source);
        auto *sink = runtime->findSink(desc->sink);
        if (!source || source->authority !=
                           AnimationSourceAuthorityV1::graph_apply ||
            !sink || !sameHandle(source->sink, sink->handle) ||
            !sameHandle(sink->active_source, source->handle))
            return Status::authority_conflict;
        if (desc->notification_revision <= sink->last_notification_revision)
            return Status::duplicate_revision;
        if (!sink->object) return Status::stale_generation;
        if (desc->kind == AnimationNotificationKind::layout_generation_mismatch &&
            sink->object->asset &&
            sameHandle(desc->observed_layout,
                       sink->object->asset->rig.layout))
            return Status::invalid_argument;
        sink->last_notification_revision = desc->notification_revision;
        sink->reset_history = true;
        return Status::ok;
    }

    static Status publishFromSource(void *context, const PublishAnimationFrameFromSourceDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (const auto status = validateDescriptor(desc->frame); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        auto *source = runtime->findSource(desc->source);
        if (!source) return Status::stale_generation;
        if (source->authority != AnimationSourceAuthorityV1::graph_apply)
            return Status::authority_conflict;
        auto *sink = runtime->findSink(source->sink);
        if (!sink || !sameHandle(sink->active_source, source->handle)) return Status::authority_conflict;
        if (!sameHandle(sink->instance, desc->frame.instance)) return Status::authority_conflict;
        for (const auto &blocked : runtime->blocked_commits) {
            if (blocked.instance_identity == desc->frame.instance.identity &&
                blocked.instance_generation == desc->frame.instance.generation &&
                blocked.revision == desc->frame.frame_revision)
                return Status::callback_failed;
        }
        auto frame = desc->frame;
        if (sink->reset_history) frame.flags |= commit_reset_history;
        if (runtime->active_phase_sink &&
            sameHandle(*runtime->active_phase_sink, sink->handle)) {
            if (frame.frame_revision != runtime->active_phase_revision)
                return Status::phase_order_error;
            if (runtime->active_phase != Phase::base_pose_and_root_modifier)
                return Status::phase_order_error;
            if (runtime->staged_frame) return Status::duplicate_revision;
            if (frame.palette_count != 0 && frame.palette == nullptr)
                return Status::invalid_argument;
            StagedFrame staged;
            staged.handle = {runtime->next_identity++, 1, 0};
            staged.sink = sink->handle;
            staged.object = sink->object;
            staged.frame = frame;
            if (frame.palette_count != 0)
                staged.palette.assign(frame.palette,
                                      frame.palette + frame.palette_count);
            staged.frame.palette = nullptr;
            runtime->staged_frame = std::move(staged);
            return Status::ok;
        }
        return runtime->publishToSink(*sink, frame);
    }

    static Status getService(void *context, std::uint32_t client_version, AnimationServiceV1 *out) {
        if (!context || !out || out->struct_size < descriptorHeaderSize) return Status::invalid_argument;
        if (out->version != descriptorVersionV1) return Status::unsupported_version;
        if (out->reserved0 != 0 || out->reserved1 != 0) return Status::reserved_not_zero;
        if (client_version != animationServiceVersionV1) return Status::unsupported_version;
        const auto caller_size = out->struct_size;
        AnimationServiceV1 produced{};
        produced.struct_size = sizeof(produced);
        produced.version = descriptorVersionV1;
        produced.service_version = animationServiceVersionV1;
        produced.minimum_client_service_version = 1;
        produced.capability_bits = animationServiceCapabilitiesV1;
        produced.context = context;
        produced.get_current_owner = getCurrentOwner;
        produced.resolve_sink = resolveSink;
        produced.resolve_instance = resolveInstance;
        produced.resolve_rig = resolveRig;
        produced.resolve_layout = resolveLayout;
        produced.resolve_clip = resolveClip;
        produced.begin_pose_frame = beginPoseFrame;
        produced.acquire_pose = acquirePose;
        produced.get_clip_metadata = getClipMetadata;
        produced.create_cursor = createCursor;
        produced.destroy_cursor = destroyCursor;
        produced.sample_pose_at = samplePose;
        produced.blend_normal = blendPoses;
        produced.local_to_model = localToModelJob;
        produced.build_skin_palette = buildPalette;
        produced.register_phase = registerPhase;
        produced.unregister_phase = unregisterPhase;
        produced.claim_source = claimSource;
        produced.release_source = releaseSource;
        produced.handoff_source = handoffSource;
        produced.notify = notify;
        produced.publish_animation_frame_from_source = publishFromSource;
        produced.sample_animation_source_at = sampleAnimationSource;
        produced.claim_source_policy = claimSourcePolicy;
        std::memcpy(out, &produced, std::min<std::size_t>(caller_size, sizeof(produced)));
        return Status::ok;
    }
};

AnimationServiceRuntime::AnimationServiceRuntime() : impl_(std::make_unique<Impl>()) {
    AnimationGraph::Internal::linkAnchor();
}
AnimationServiceRuntime::~AnimationServiceRuntime() = default;

void AnimationServiceRuntime::registerObject(std::string name, const SkeletalModelData &model) {
    impl_->registerObject(std::move(name), model);
}

void AnimationServiceRuntime::registerObject(
    std::string name, const SkeletalModelData &model,
    ModelAssetId logical_asset) {
    std::scoped_lock lock{impl_->mutex};
    impl_->registerObjectLocked(std::move(name), model, std::nullopt,
                                logical_asset);
}

void AnimationServiceRuntime::registerObject(
    std::string name, const SkeletalModelData &model,
    ModelInstanceId renderer_instance) {
    std::scoped_lock lock{impl_->mutex};
    impl_->registerObjectLocked(std::move(name), model, renderer_instance);
}

Status AnimationServiceRuntime::registerVrmaSource(
    std::string object_name, std::string source_name,
    std::shared_ptr<const VrmaRetargetedClip> clip) noexcept {
    try {
        return impl_->registerVrmaSource(
            std::move(object_name), std::move(source_name), std::move(clip));
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (...) {
        return Status::invalid_argument;
    }
}

Status AnimationServiceRuntime::reloadVrmaSource(
    std::string_view object_name, std::string_view source_name,
    std::shared_ptr<const VrmaRetargetedClip> replacement) noexcept {
    try {
        return impl_->reloadVrmaSource(object_name, source_name,
                                       std::move(replacement));
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (...) {
        return Status::invalid_argument;
    }
}

void AnimationServiceRuntime::reloadAsset(
    const SkeletalModelData *previous,
    const SkeletalModelData *replacement) {
    impl_->reloadAsset({}, previous, replacement);
}

void AnimationServiceRuntime::reloadAsset(
    ModelAssetId logical_asset, const SkeletalModelData *previous,
    const SkeletalModelData *replacement) {
    impl_->reloadAsset(logical_asset, previous, replacement);
}

void AnimationServiceRuntime::reset() { impl_->reset(); }

void AnimationServiceRuntime::releaseOwner(internal::RegistrationOwner owner) noexcept { impl_->releaseOwner(owner); }

Status AnimationServiceRuntime::runPhases(AnimationSinkHandle sink, std::uint64_t frame_revision) noexcept {
    return impl_->runPhases(sink, frame_revision);
}

Status AnimationServiceRuntime::runAllPhases(std::uint64_t frame_revision) noexcept {
    return impl_->runAllPhases(frame_revision);
}

std::uint64_t AnimationServiceRuntime::registrationGeneration() const noexcept {
    std::scoped_lock lock{impl_->mutex};
    return impl_->registration_generation;
}

AnimationServiceRuntime &animationServiceRuntime() {
    static auto *runtime = new AnimationServiceRuntime();
    return *runtime;
}

void releaseAnimationOwner(internal::RegistrationOwner owner) noexcept {
    animationServiceRuntime().releaseOwner(owner);
}

Status getApiV1(std::uint32_t client_abi_version, ApiV1 *out_api) noexcept {
    if (!out_api || out_api->struct_size < descriptorHeaderSize) return Status::invalid_argument;
    if (out_api->version != descriptorVersionV1) return Status::unsupported_version;
    if (out_api->reserved0 != 0 || out_api->reserved1 != 0) return Status::reserved_not_zero;
    if (client_abi_version < 1 || client_abi_version > abiVersionV1) return Status::unsupported_version;
    const auto caller_size = out_api->struct_size;
    ApiV1 produced{};
    produced.struct_size = sizeof(ApiV1);
    produced.version = descriptorVersionV1;
    produced.engine_abi_version = abiVersionV1;
    produced.minimum_client_abi_version = 1;
    produced.capability_bits = animationServiceCapabilitiesV1;
    try {
        produced.context = animationServiceRuntime().impl_.get();
    } catch (...) {
        return Status::out_of_memory;
    }
    produced.advance_cursor = AnimationServiceRuntime::Impl::apiAdvance;
    produced.publish_animation_frame = AnimationServiceRuntime::Impl::apiPublish;
    produced.advance_temporal_history_after_render = AnimationServiceRuntime::Impl::apiAdvanceHistory;
    produced.get_animation_service = AnimationServiceRuntime::Impl::getService;
    std::memcpy(out_api, &produced, std::min<std::size_t>(caller_size, sizeof(produced)));
    return Status::ok;
}

Status getPoseStagingServiceV1(
    std::uint32_t client_service_version,
    PoseStagingServiceV1 *out_service) noexcept {
    if (!out_service ||
        out_service->struct_size < sizeof(DescriptorHeaderV1))
        return Status::invalid_argument;
    if (out_service->version != poseStagingDescriptorVersionV1)
        return Status::unsupported_version;
    if (out_service->reserved0 != 0 || out_service->reserved1 != 0)
        return Status::reserved_not_zero;
    if (client_service_version != poseStagingServiceVersionV1)
        return Status::unsupported_version;
    const auto caller_size = out_service->struct_size;
    try {
        PoseStagingServiceV1 produced;
        produced.service_version = poseStagingServiceVersionV1;
        produced.minimum_client_service_version = 1;
        produced.capability_bits = poseStagingServiceCapabilitiesV1;
        produced.context = animationServiceRuntime().impl_.get();
        produced.acquire_staged_pose =
            AnimationServiceRuntime::Impl::acquireStagedPose;
        produced.resolve_source_node =
            AnimationServiceRuntime::Impl::resolveStagedSourceNode;
        std::memcpy(out_service, &produced,
                    std::min<std::size_t>(caller_size, sizeof(produced)));
        return Status::ok;
    } catch (...) {
        return Status::out_of_memory;
    }
}

} // namespace Pelican::Animation
