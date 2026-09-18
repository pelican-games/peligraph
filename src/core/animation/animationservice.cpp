#include "animationservice.hpp"
#include "../registrationownerhost.hpp"

#include "animationjobs.hpp"
#include "animationphasescheduler.hpp"
#include "animationposearena.hpp"
#include "animationprobe.hpp"
#include "animationrecords.hpp"
#include "animationserviceabi.hpp"
#include "animationsinkregistry.hpp"
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
#include <stdexcept>
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

// The outer handlers of `runPhases` and `runAllPhases`. C-4 gave the client
// callback its own three-way mapping (`PhaseScheduler::statusForCallbackException`)
// but left these two as a flat `catch (...) -> out_of_memory`, and C-4's own
// note carried that forward as a follow-up. Everything that reaches here is
// engine code -- the callback never escapes its handler -- so the ABI's
// `callback_failed` would name the wrong party, and `phase_order_error` is not
// a candidate either: a phase-order violation is *returned* by the driver
// below (`return Status::phase_order_error` in runPhases), never thrown.
// Must be called from inside a catch block.
Status statusForEngineException() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (const std::invalid_argument &) {
        return Status::invalid_argument;
    } catch (...) {
        // Conservative default: an unrecognized engine-side exception keeps
        // the status this path has always reported.
        return Status::out_of_memory;
    }
}

} // namespace

// D-7. What is left here after the split is the part that is genuinely one
// thing: the asset / object tables, the owner and handle-generation tables,
// the cursor and .vrma clip tables, the single ProbeRuntime -- and the mutex
// that guards all of it and all three parts. `PoseArena`, `SinkRegistry` and
// `PhaseScheduler` are plain members with no locks of their own; every call
// into them below happens with `mutex` held, except across a client phase
// callback in runPhases, where C-4 deliberately releases it.
//
// D-7's follow-up proposed a fourth part, `AssetTable`, over the maps in the
// first block below. It was measured and not taken: it is not a pure move.
// Of the 24 functions the follow-up listed as asset-shaped, 9 already reach a
// second part -- `reloadAsset` (pose_arena.stalePosesForLayout,
// sink_registry.rebindInstancesForObjects), `unregisterObject` and through it
// `synchronizeSceneObjects` (sink_registry.retireObject), `samplePoseLocked`
// and `sampleAnimationSourceLocked` (pose_arena.resolvePose),
// `createCursorLocked` / `destroyCursorLocked` / `ensureOwner` /
// `validateOwner` (the `owners` table, which the follow-up's member list left
// behind) -- and a further eleven functions that were never on the list
// (`blendPosesLocked`, `localToModelLocked`, `buildPaletteLocked`,
// `acquirePoseLocked`, `beginPoseFrameLocked`, `resolveRigLocked`,
// `publishToSink`, `commitStagedFrame`, `acquireStagedPoseLocked`,
// `resolveStagedSourceNodeLocked`, `publishFromSourceLocked`) read the same
// maps. Moving the maps out therefore does not move the readers; it replaces
// nine direct members with nine public accessors on a table with no invariant
// of its own, which is a worse seam than the one it removes. `reset()` and
// `releaseOwner()` span all four groupings either way.
struct AnimationServiceRuntime::Impl {
    using ObjectRecord = Internal::ObjectRecord;
    using PoseArena = Internal::PoseArena;
    using PoseRecord = Internal::PoseArena::PoseRecord;
    using SinkRegistry = Internal::SinkRegistry;
    using SinkRecord = Internal::SinkRegistry::SinkRecord;
    using StagedFrame = Internal::SinkRegistry::StagedFrame;
    using PhaseScheduler = Internal::PhaseScheduler;

    struct OwnerRecord {
        std::uint32_t generation{1};
        bool active{true};
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
    std::unordered_map<std::uint64_t, OwnerRecord> owners;
    std::unordered_map<std::uint64_t, CursorRecord> cursors;
    std::unordered_map<std::uint64_t, VrmaClipRecord> vrma_clips;
    PoseArena pose_arena;
    SinkRegistry sink_registry;
    PhaseScheduler phase_scheduler;
    std::uint64_t next_identity{1};
    std::uint64_t next_vrma_identity{1ull << 63u};
    std::uint64_t registration_generation{1};

    // Get-or-create, and deliberately so: the identity is the engine's own
    // registration-owner slot (`currentRegistrationOwner`), never a handle a
    // client supplied, and the only caller is getCurrentOwner -- asking "which
    // owner am I?" for a slot that has not registered anything yet has to
    // answer, not fail. The inserted record is `active` with generation 1,
    // which is also what makes the tombstone in releaseOwner load-bearing: if
    // that record were erased instead, this would resurrect generation 1 and a
    // handle from the previous DLL generation would validate again.
    AnimationOwnerHandle ensureOwner(internal::RegistrationOwner internal_owner) {
        const auto identity = internal_owner + 1;
        const auto &owner = owners[identity];
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
        pose_arena.stalePosesForLayout(old_layout_identity);

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

        sink_registry.rebindInstancesForObjects(affected);
    }

    bool unregisterObject(std::string_view name) {
        const auto found = objects.find(std::string{name});
        if (found == objects.end()) return false;
        auto *object = found->second.get();
        ObjectRecord *replacement = nullptr;
        for (const auto &[_, candidate] : objects) {
            if (candidate.get() != object && candidate->asset == object->asset) {
                replacement = candidate.get();
                break;
            }
        }
        // These indices use one representative of a shared asset. Do not
        // invalidate its handles when a different actor still uses it.
        for (auto it = rigs.begin(); it != rigs.end();) {
            if (it->second != object) { ++it; continue; }
            if (replacement) { it->second = replacement; ++it; }
            else it = rigs.erase(it);
        }
        const auto retire_index = [&](auto &index) {
            for (auto it = index.begin(); it != index.end();) {
                if (it->second.first != object) { ++it; continue; }
                if (replacement) { it->second.first = replacement; ++it; }
                else it = index.erase(it);
            }
        };
        retire_index(clips);
        retire_index(bindings);
        for (auto it = vrma_clips.begin(); it != vrma_clips.end();) {
            if (it->second.object != object) { ++it; continue; }
            for (auto &[_, cursor] : cursors) {
                if (cursor.active && cursor.clip.identity == it->first) {
                    (void)legacy_runtime.destroyCursor(cursor.handle);
                    cursor.active = false;
                }
            }
            it = vrma_clips.erase(it);
        }
        sink_registry.retireObject(object);
        objects.erase(found);
        return true;
    }

    bool sceneObjectCurrent(const ObjectRecord &object) const {
        if (!object.scene_entity) return true;
        if (!FastModuleContainer::isInitialized<SceneLoader>() ||
            !FastModuleContainer::isInitialized<ECSCore>() ||
            !FastModuleContainer::isInitialized<ModelAssetContainer>()) return false;
        auto &scene = GET_MODULE(SceneLoader);
        if (object.scene_epoch != scene.runtimeSceneEpoch() ||
            scene.objectId(object.name) != object.scene_entity) return false;
        auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
        const auto *view = ecs.tryComponent<SimpleModelViewComponent>(*object.scene_entity);
        if (!view || view->model_instance_id != object.renderer_instance || view->model_name.empty())
            return false;
        const auto &model = GET_MODULE(ModelAssetContainer).getModelTemplateByName(view->model_name);
        return model.skeletal.get() == object.model;
    }

    void synchronizeSceneObjects() {
        std::scoped_lock lock{mutex};
        for (auto it = objects.begin(); it != objects.end();) {
            auto &object = *it->second;
            ++it;
            if (!sceneObjectCurrent(object)) unregisterObject(object.name);
        }
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
        auto &object = *objects.at(std::string{name});
        object.scene_entity = object_id;
        object.scene_epoch = GET_MODULE(SceneLoader).runtimeSceneEpoch();
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
        sink_registry.clearForReset();
        phase_scheduler.clearForReset();
        pose_arena.clearForReset();
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
            phase_scheduler.retirePhasesForOwner(identity);
            sink_registry.retireSourcesForOwner(identity);
            for (auto &[_, cursor] : cursors) {
                if (!cursor.active || cursor.owner.identity != identity) continue;
                legacy_runtime.destroyCursor(cursor.handle);
                cursor.active = false;
            }
            pose_arena.retireArenasForOwner(identity);
            found->second.active = false;
            if (++found->second.generation == 0) ++found->second.generation;
            // C-4. Every retired source is dropped, not just this owner's: the
            // sinks that referred to them were cleared above (and by
            // releaseSource / handoffSource), and findSource already answers
            // nullptr for an inactive record, so an erased one is
            // indistinguishable from a tombstoned one at every call site.
            // Identities come from the monotonic next_identity, so nothing is
            // ever reused. This is what stops the table from growing by one
            // entry per source per game-DLL reload.
            //
            // owners / phases / cursors keep their tombstones on purpose:
            // validateOwner, unregisterPhase and destroyCursor each distinguish
            // invalid_handle (never existed) from stale_generation (existed,
            // released), which is contract, and a cursor record mirrors a
            // second tombstone inside ProbeRuntime that would then disagree.
            sink_registry.compactRetiredSources();
        } catch (...) {
        }
    }

    // C-4. The callbacks run OUTSIDE `mutex`. The lock is taken to decide what
    // to run and to build each context, released across the call, and taken
    // again to record the result -- so a callback may re-enter the service
    // (every pose operation does) without the re-entrancy being load-bearing,
    // and a phase that takes a second lock of its own no longer inverts an
    // order against this one.
    //
    // D-7 keeps this driver on `Impl`: it spans all three parts -- the phase
    // table, the sink registry (sink lookup, blocked commits, staged frame)
    // and, through commitStagedFrame, the pose arena and the ProbeRuntime.
    //
    // Two consequences the body has to carry:
    //  - `sink` is a raw SinkRecord* and cannot survive the gap, so only the
    //    handle is kept and `findSink` re-resolves it after every callback. A
    //    sink that went away mid-run is `stale_generation`, the same answer
    //    commitStagedFrame already gives for exactly this.
    //  - `ordered` holds PhaseRecord* into a node-based unordered_map that
    //    nothing erases (see releaseOwner), so the pointers stay valid. A
    //    register_phase from inside a callback therefore does not join the
    //    pass that is running; it is picked up by the next one.
    Status runPhases(AnimationSinkHandle sink_handle, std::uint64_t revision) noexcept {
        const auto clear_active = [this]() {
            sink_registry.stagedFrame().reset();
            phase_scheduler.clearActive();
        };
        try {
            AnimationSinkHandle sink_key{};
            std::vector<PhaseScheduler::PhaseRecord *> ordered;
            {
                std::scoped_lock lock{mutex};
                auto *sink = sink_registry.findSink(sink_handle);
                if (!sink || revision == 0) return Status::invalid_handle;
                if (phase_scheduler.activeSink()) return Status::phase_order_error;
                sink_key = sink->handle;
                phase_scheduler.beginRun(sink->handle, revision);
                sink_registry.stagedFrame().reset();
                ordered = phase_scheduler.orderedActivePhases();
            }
            for (const auto *phase : ordered) {
                AnimationPhaseContextV1 context{};
                AnimationPhaseCallbackV1 callback = nullptr;
                void *user_context = nullptr;
                internal::RegistrationOwner owner_slot = 0;
                {
                    std::scoped_lock lock{mutex};
                    auto *sink = sink_registry.findSink(sink_key);
                    if (!sink) {
                        clear_active();
                        return Status::stale_generation;
                    }
                    // Read while the phase is still the active one: a callback
                    // that re-enters publish_from_source reads this back.
                    phase_scheduler.setActivePhase(phase->registration.phase);
                    context.struct_size = sizeof(context);
                    context.version = descriptorVersionV1;
                    context.phase = phase->registration.phase;
                    context.sink = sink->handle;
                    context.instance = sink->instance;
                    context.frame_revision = revision;
                    callback = phase->callback;
                    user_context = phase->user_context;
                    owner_slot = phase->owner.identity - 1;
                }
                Status status = Status::callback_failed;
                try {
                    internal::ScopedRegistrationOwner owner_scope{owner_slot};
                    status = callback(user_context, &context);
                } catch (...) {
                    status = PhaseScheduler::statusForCallbackException();
                }
                std::scoped_lock lock{mutex};
                auto *sink = sink_registry.findSink(sink_key);
                if (status != Status::ok) {
                    if (sink != nullptr)
                        sink_registry.blockCommit(sink->instance, revision);
                    clear_active();
                    return status;
                }
                if (sink == nullptr) {
                    clear_active();
                    return Status::stale_generation;
                }
            }
            std::scoped_lock lock{mutex};
            // commitStagedFrame re-resolves the sink by handle and rejects a
            // staged frame whose object is no longer the one that sink names,
            // which is also how `staged_frame.object` -- a raw ObjectRecord* --
            // is re-validated across the gap.
            const auto commit_status = commitStagedFrame();
            if (commit_status != Status::ok) {
                if (auto *sink = sink_registry.findSink(sink_key); sink != nullptr)
                    sink_registry.blockCommit(sink->instance, revision);
                clear_active();
                return commit_status;
            }
            clear_active();
            return Status::ok;
        } catch (...) {
            // Engine-side failure, not a client callback: the mapping in
            // PhaseScheduler deliberately does not apply here (see
            // statusForEngineException).
            clear_active();
            return statusForEngineException();
        }
    }

    Status runAllPhases(std::uint64_t revision) noexcept {
        try {
            std::vector<AnimationSinkHandle> active_sinks;
            {
                std::scoped_lock lock{mutex};
                active_sinks = sink_registry.sinkHandles();
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
            // Only the fan-out itself can throw here: runPhases is noexcept
            // and already answered for every phase it ran.
            return statusForEngineException();
        }
    }

    static Impl *self(void *context) { return static_cast<Impl *>(context); }

    // Spans the sink registry (the sink and its active source), the pose arena
    // (the model pose whose matrices are published) and the ProbeRuntime, so it
    // stays here.
    Status publishToSink(SinkRecord &sink, PublishAnimationFrameDescV1 &frame) {
        const auto *model = pose_arena.findPose(frame.model_pose);
        const std::span<const Matrix4fV1> matrices = model
            ? std::span<const Matrix4fV1>{model->model_matrices} : std::span<const Matrix4fV1>{};
        const std::span<const std::uint32_t> mapping = matrices.empty()
            ? std::span<const std::uint32_t>{}
            : std::span<const std::uint32_t>{sink.object->asset->rig.original_to_layout};
        const auto status =
            sink.object->renderer_instance &&
                    FastModuleContainer::isInitialized<PolygonInstanceContainer>()
                ? GET_MODULE(PolygonInstanceContainer)
                      .publishAnimationFrame(*sink.object->renderer_instance,
                          {AnimationFrameWriterKind::animation_source,
                           sink.active_source.identity},
                          frame, matrices, mapping)
                : legacy_runtime.publishAnimationFrame(frame);
        if (status == Status::ok) sink.reset_history = false;
        return status;
    }

    // Same reason as publishToSink: the staged frame is the sink registry's,
    // the poses it resolves are the arena's, and the rig comes from the object
    // table here.
    Status commitStagedFrame() {
        auto &staged_slot = sink_registry.stagedFrame();
        if (!staged_slot) return Status::ok;
        auto staged = std::move(*staged_slot);
        staged_slot.reset();
        auto *sink = sink_registry.findSink(staged.sink);
        if (!sink || sink->object != staged.object)
            return Status::stale_generation;

        if (staged.pose_accessed) {
            auto *local = pose_arena.findPose(staged.frame.local_pose);
            auto *model = pose_arena.findPose(staged.frame.model_pose);
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
            model->model_matrices = std::move(model_matrices);
        }
        staged.frame.palette = staged.palette.empty() ? nullptr
                                                       : staged.palette.data();
        staged.frame.palette_count =
            static_cast<std::uint32_t>(staged.palette.size());
        return publishToSink(*sink, staged.frame);
    }

    // ---- Operations behind the C-ABI shim. Each assumes `mutex` is held. ----

    Status acquireStagedPoseLocked(AcquireStagedPoseDescV1 &desc) {
        if (!isValid(desc.instance) || desc.frame_revision == 0)
            return Status::invalid_argument;
        auto &staged_slot = sink_registry.stagedFrame();
        if (!phase_scheduler.activeSink() || !staged_slot)
            return Status::not_found;
        auto &stage = *staged_slot;
        if (!sameHandle(stage.frame.instance, desc.instance) ||
            stage.frame.frame_revision != desc.frame_revision)
            return Status::not_found;
        auto *local = pose_arena.findPose(stage.frame.local_pose);
        auto *model = pose_arena.findPose(stage.frame.model_pose);
        if (!local || !model) return Status::stale_generation;
        stage.pose_accessed = true;
        desc.stage = stage.handle;
        desc.local_pose = local->view;
        desc.model_pose = model->view;
        return Status::ok;
    }

    Status resolveStagedSourceNodeLocked(ResolveStagedSourceNodeDescV1 &desc) {
        const auto &staged_slot = sink_registry.stagedFrame();
        if (!staged_slot || !Animation::isValid(desc.stage) ||
            !sameHandle(staged_slot->handle, desc.stage))
            return Status::stale_generation;
        const auto *object = staged_slot->object;
        if (!object || !object->asset ||
            desc.source_node_index >=
                object->asset->rig.original_to_layout.size())
            return Status::not_found;
        const auto mapped =
            object->asset->rig.original_to_layout[desc.source_node_index];
        if (mapped == std::numeric_limits<std::uint32_t>::max())
            return Status::not_found;
        desc.layout_node_index = mapped;
        return Status::ok;
    }

    // The rig-scoped twin of resolveStagedSourceNode. A caller that has not
    // staged anything yet -- a pose.clip_sample node resolving its root_node at
    // attach -- cannot go through the staged frame, and the mapping it needs is
    // a property of the rig, not of any one frame. With a clip handle it also
    // reports which channel kinds that clip drives on the node, so "you called
    // node 3 the root and this clip never translates node 3" is answerable
    // before the first sample instead of showing up as a delta that is always
    // zero.
    Status resolveRigSourceNodeLocked(ResolveRigSourceNodeDescV1 &desc) {
        ObjectRecord *object{};
        if (const auto status = resolveRigResource(desc.rig, object);
            status != Status::ok)
            return status;
        const auto &rig = object->asset->rig;
        if (desc.source_node_index >= rig.original_to_layout.size())
            return Status::not_found;
        const auto mapped = rig.original_to_layout[desc.source_node_index];
        if (mapped == std::numeric_limits<std::uint32_t>::max())
            return Status::not_found;
        desc.layout_node_index = mapped;
        desc.channel_kind_mask = 0;
        if (!isValid(desc.clip)) return Status::ok;
        std::pair<ObjectRecord *, const AnimationClipResource *> *clip{};
        if (const auto status = resolveClipResource(desc.clip, clip);
            status != Status::ok)
            return status;
        if (clip->first != object) return Status::incompatible_layout;
        if (clip->second == nullptr || clip->second->source == nullptr)
            return Status::not_found;
        for (const auto &channel : clip->second->source->channels)
            if (channel.node >= 0 &&
                static_cast<std::uint32_t>(channel.node) == desc.source_node_index)
                desc.channel_kind_mask |=
                    1u << static_cast<std::uint32_t>(channel.path);
        return Status::ok;
    }

    Status getCurrentOwnerLocked(CurrentAnimationOwnerDescV1 &desc) {
        desc.owner = ensureOwner(internal::currentRegistrationOwner());
        return Status::ok;
    }

    Status resolveSinkLocked(ResolveAnimationSinkDescV1 &desc) {
        const auto object_name = std::string{checkedString(desc.object_name, desc.object_name_size)};
        auto object = objects.find(object_name);
        if (object != objects.end() && !sceneObjectCurrent(*object->second)) {
            unregisterObject(object_name);
            object = objects.end();
        }
        if (object == objects.end() && registerSceneObject(object_name))
            object = objects.find(object_name);
        if (object == objects.end()) return Status::not_found;
        return sink_registry.sinkForObject(*object->second, desc.sink_kind,
                                           next_identity, desc.sink);
    }

    Status resolveInstanceLocked(ResolveAnimationInstanceDescV1 &desc) {
        auto *sink = sink_registry.findSink(desc.sink);
        if (!sink) return Status::invalid_handle;
        desc.instance = sink->instance;
        return Status::ok;
    }

    Status resolveRigLocked(ResolveAnimationRigDescV1 &desc) {
        SinkRecord *sink{};
        if (const auto status =
                sink_registry.resolveSinkByInstance(desc.instance, sink);
            status != Status::ok)
            return status;
        if (!sink->object || !sink->object->asset) return Status::not_found;
        desc.rig = sink->object->asset->rig.handle;
        return Status::ok;
    }

    Status resolveLayoutLocked(ResolvePoseLayoutDescV1 &desc) {
        ObjectRecord *object{};
        if (const auto status = resolveRigResource(desc.rig, object);
            status != Status::ok)
            return status;
        desc.layout = object->asset->rig.layout;
        desc.joint_count = static_cast<std::uint32_t>(object->asset->rig.rest_pose.size());
        desc.palette_count = static_cast<std::uint32_t>(object->model->joint_nodes.size());
        desc.skin_binding = object->asset->skin_bindings.empty()
                                ? invalidHandle<SkinBindingHandle>()
                                : object->asset->skin_bindings.front().handle;
        return Status::ok;
    }

    Status resolveClipLocked(ResolveAnimationClipDescV1 &desc) {
        ObjectRecord *object{};
        if (const auto status = resolveRigResource(desc.rig, object);
            status != Status::ok)
            return status;
        const auto name = checkedString(desc.clip_name, desc.clip_name_size);
        for (const auto &clip : object->asset->clips) {
            if (clip.source && clip.source->name == name) {
                desc.clip = clip.handle;
                return Status::ok;
            }
        }
        for (const auto &[_, clip] : vrma_clips) {
            if (clip.object != object || clip.name != name) continue;
            if (!sameHandle(clip.target_rig, desc.rig))
                return Status::stale_generation;
            desc.clip = clip.handle;
            return Status::ok;
        }
        return Status::not_found;
    }

    Status beginPoseFrameLocked(BeginPoseArenaFrameDescV1 &desc) {
        if (const auto status = validateOwner(desc.owner); status != Status::ok) return status;
        return pose_arena.beginFrame(desc.owner, desc.frame_revision,
                                     next_identity, desc.arena);
    }

    Status acquirePoseLocked(AcquirePoseDescV1 &desc) {
        if (const auto status = validateResource(desc.layout.identity,
                                                 desc.layout.generation);
            status != Status::ok)
            return status;
        return pose_arena.acquirePose(desc.arena, desc.layout,
                                      desc.joint_count, *desc.out_view);
    }

    Status getClipMetadataLocked(ClipMetadataV1 &metadata) {
        if (auto vrma = vrma_clips.find(metadata.clip.identity);
            vrma != vrma_clips.end()) {
            VrmaClipRecord *resource{};
            if (const auto status = resolveVrmaClipResource(metadata.clip,
                                                            resource);
                status != Status::ok)
                return status;
            const auto caller_size = metadata.struct_size;
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
            std::memcpy(&metadata, &produced,
                        std::min<std::size_t>(caller_size,
                                              sizeof(produced)));
            return Status::ok;
        }
        std::pair<ObjectRecord *, const AnimationClipResource *> *found{};
        if (const auto status = resolveClipResource(metadata.clip, found);
            status != Status::ok)
            return status;
        const auto *clip = found->second;
        const auto caller_size = metadata.struct_size;
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
        std::memcpy(&metadata, &produced, std::min<std::size_t>(caller_size, sizeof(produced)));
        return Status::ok;
    }

    Status createCursorLocked(CreateClipCursorDescV1 &desc) {
        if (const auto status = validateOwner(desc.owner); status != Status::ok) return status;
        if (vrma_clips.contains(desc.clip.identity)) {
            VrmaClipRecord *resource{};
            if (const auto status = resolveVrmaClipResource(desc.clip, resource);
                status != Status::ok)
                return status;
            const auto duration = resource->clip->end - resource->clip->start;
            const auto cursor = legacy_runtime.createCursor(
                duration, WrapMode::repeat);
            if (!isValid(cursor)) return Status::invalid_argument;
            cursors.emplace(
                cursor.identity,
                CursorRecord{cursor, desc.owner, desc.clip, true});
            desc.cursor = cursor;
            return Status::ok;
        }
        std::pair<ObjectRecord *, const AnimationClipResource *> *found{};
        if (const auto status = resolveClipResource(desc.clip, found);
            status != Status::ok)
            return status;
        const double duration = found->second->source->end - found->second->source->start;
        const auto cursor = legacy_runtime.createCursor(duration, WrapMode::repeat);
        if (!isValid(cursor)) return Status::invalid_argument;
        cursors.emplace(cursor.identity, CursorRecord{cursor, desc.owner, desc.clip, true});
        desc.cursor = cursor;
        return Status::ok;
    }

    Status destroyCursorLocked(const DestroyClipCursorDescV1 &desc) {
        if (const auto status = validateOwner(desc.owner); status != Status::ok) return status;
        const auto found = cursors.find(desc.cursor.identity);
        if (found == cursors.end() || !sameHandle(found->second.handle, desc.cursor))
            return Status::invalid_handle;
        if (!found->second.active || !sameHandle(found->second.owner, desc.owner))
            return Status::stale_generation;
        const auto status = legacy_runtime.destroyCursor(desc.cursor);
        if (status == Status::ok) found->second.active = false;
        return status;
    }

    Status samplePoseLocked(const SamplePoseAtDescV1 &desc) {
        if (vrma_clips.contains(desc.clip.identity)) {
            VrmaClipRecord *resource{};
            if (const auto status = resolveVrmaClipResource(desc.clip, resource);
                status != Status::ok)
                return status;
            PoseRecord *pose{};
            if (const auto status = pose_arena.resolvePose(desc.output_pose, pose);
                status != Status::ok)
                return status;
            return sampleVrma(*resource, desc.time_seconds, *pose, nullptr, 0,
                              nullptr, nullptr);
        }
        std::pair<ObjectRecord *, const AnimationClipResource *> *clip{};
        if (const auto status = resolveClipResource(desc.clip, clip);
            status != Status::ok)
            return status;
        PoseRecord *pose{};
        if (const auto status = pose_arena.resolvePose(desc.output_pose, pose);
            status != Status::ok)
            return status;
        // `time_seconds` is the absolute clip time the caller already decided
        // on. Whoever owns the loop policy -- a pose.clip_sample node, an
        // animation graph cursor -- has wrapped or clamped it; wrapping here a
        // second time would fold a clamped final frame back to the clip's start
        // and would shift the phase of every clip whose keyframes do not begin
        // at 0. samplePoseAt is asked for that exact time and nothing else.
        const auto begin = clip->second != nullptr && clip->second->source != nullptr
            ? static_cast<double>(clip->second->source->start) : 0.0;
        return samplePoseAt(*clip->first->asset, clip->second, desc.time_seconds - begin,
                            1.0, false, 0.0, pose->view);
    }

    Status sampleAnimationSourceLocked(SampleAnimationSourceAtDescV1 &desc) {
        PoseRecord *pose{};
        if (const auto status = pose_arena.resolvePose(desc.output_pose, pose);
            status != Status::ok)
            return status;
        if (vrma_clips.contains(desc.clip.identity)) {
            VrmaClipRecord *resource{};
            if (const auto status = resolveVrmaClipResource(desc.clip, resource);
                status != Status::ok)
                return status;
            return sampleVrma(*resource, desc.time_seconds, *pose,
                              desc.expressions, desc.expression_capacity,
                              &desc.expression_count, &desc.gaze);
        }
        std::pair<ObjectRecord *, const AnimationClipResource *> *clip{};
        if (const auto status = resolveClipResource(desc.clip, clip);
            status != Status::ok)
            return status;
        const auto status = samplePoseAt(
            *clip->first->asset, clip->second, desc.time_seconds, 1.0, true,
            0.0, pose->view);
        if (status != Status::ok) return status;
        desc.expression_count = 0;
        AnimationGazeSampleV1 gaze{};
        gaze.struct_size = sizeof(gaze);
        gaze.version = descriptorVersionV1;
        gaze.rotation.w = 1.0f;
        desc.gaze = gaze;
        return Status::ok;
    }

    Status blendPosesLocked(const BlendNormalDescV1 &desc) {
        ObjectRecord *object{};
        if (const auto status = resolveRigResource(desc.rig, object);
            status != Status::ok)
            return status;
        return pose_arena.blendNormalJob(object->asset->rig, desc);
    }

    Status localToModelLocked(LocalToModelDescV1 &desc) {
        ObjectRecord *object{};
        if (const auto status = resolveRigResource(desc.rig, object);
            status != Status::ok)
            return status;
        return pose_arena.localToModelJob(object->asset->rig, desc);
    }

    Status buildPaletteLocked(BuildSkinPaletteDescV1 &desc) {
        if (!isValid(desc.skin_binding)) return Status::invalid_handle;
        if (const auto status = validateResource(desc.skin_binding.identity,
                                                 desc.skin_binding.generation);
            status != Status::ok)
            return status;
        const auto found = bindings.find(desc.skin_binding.identity);
        if (found == bindings.end()) return Status::invalid_handle;
        if (!sameHandle(found->second.second->handle, desc.skin_binding))
            return Status::stale_generation;
        const auto required = static_cast<std::uint32_t>(found->second.first->model->joint_nodes.size());
        desc.palette_count = required;
        if (desc.model_matrix_count < found->second.first->asset->rig.rest_pose.size() || !desc.model_matrices)
            return Status::invalid_argument;
        if (desc.palette_capacity < required || !desc.palette) return Status::buffer_too_small;
        return buildSkinPalette(*found->second.first->asset,
                                {desc.model_matrices, desc.model_matrix_count},
                                {desc.palette, desc.palette_capacity});
    }

    Status registerPhaseLocked(RegisterAnimationPhaseDescV1 &desc) {
        if (const auto status = validateOwner(desc.owner); status != Status::ok) return status;
        return phase_scheduler.registerPhase(desc.owner, desc.registration,
                                             desc.callback, desc.user_context,
                                             next_identity,
                                             desc.registration_handle);
    }

    Status unregisterPhaseLocked(const UnregisterAnimationPhaseDescV1 &desc) {
        if (const auto status = validateOwner(desc.owner); status != Status::ok) return status;
        return phase_scheduler.unregisterPhase(desc.owner, desc.registration);
    }

    Status claimSourceLocked(AnimationOwnerHandle owner,
                             AnimationSinkHandle sink, std::uint32_t ordinal,
                             AnimationSourceAuthorityV1 authority,
                             AnimationSourceHandle &out) {
        if (const auto status = validateOwner(owner); status != Status::ok) return status;
        return sink_registry.claimSource(owner, sink, ordinal, authority,
                                         next_identity, out);
    }

    Status releaseSourceLocked(const ReleaseAnimationSourceDescV1 &desc) {
        if (const auto status = validateOwner(desc.owner); status != Status::ok) return status;
        return sink_registry.releaseSource(desc.owner, desc.source);
    }

    // Owner validation is interleaved with the source and sink lookups -- a
    // stale current source has to answer stale_generation even when the next
    // owner is also bad -- so the ordering lives here rather than in either
    // part.
    Status handoffSourceLocked(HandoffAnimationSourceDescV1 &desc) {
        auto *source = sink_registry.findSource(desc.current_source);
        if (!source) return Status::stale_generation;
        if (source->authority != AnimationSourceAuthorityV1::graph_apply)
            return Status::authority_conflict;
        if (const auto status = validateOwner(desc.next_owner); status != Status::ok) return status;
        auto *sink = sink_registry.findSink(source->sink);
        if (!sink || !sameHandle(sink->active_source, source->handle)) return Status::authority_conflict;
        desc.next_source = sink_registry.transferSource(
            *source, *sink, desc.next_owner, desc.next_source_ordinal,
            next_identity);
        return Status::ok;
    }

    // Spans the sink registry (source/sink arbitration, the commit gate and
    // the staged frame), the phase scheduler (which phase of which sink is
    // running) and publishToSink, so it stays here.
    Status publishFromSourceLocked(const PublishAnimationFrameFromSourceDescV1 &desc) {
        auto *source = sink_registry.findSource(desc.source);
        if (!source) return Status::stale_generation;
        if (source->authority != AnimationSourceAuthorityV1::graph_apply)
            return Status::authority_conflict;
        auto *sink = sink_registry.findSink(source->sink);
        if (!sink || !sameHandle(sink->active_source, source->handle)) return Status::authority_conflict;
        if (!sameHandle(sink->instance, desc.frame.instance)) return Status::authority_conflict;
        if (sink_registry.commitBlocked(desc.frame.instance,
                                        desc.frame.frame_revision))
            return Status::callback_failed;
        auto frame = desc.frame;
        if (sink->reset_history) frame.flags |= commit_reset_history;
        if (phase_scheduler.activeSink() &&
            sameHandle(*phase_scheduler.activeSink(), sink->handle)) {
            if (frame.frame_revision != phase_scheduler.activeRevision())
                return Status::phase_order_error;
            if (phase_scheduler.activePhase() != Phase::base_pose_and_root_modifier)
                return Status::phase_order_error;
            if (sink_registry.stagedFrame()) return Status::duplicate_revision;
            if (frame.palette_count != 0 && frame.palette == nullptr)
                return Status::invalid_argument;
            StagedFrame staged;
            staged.handle = {next_identity++, 1, 0};
            staged.sink = sink->handle;
            staged.object = sink->object;
            staged.frame = frame;
            if (frame.palette_count != 0)
                staged.palette.assign(frame.palette,
                                      frame.palette + frame.palette_count);
            staged.frame.palette = nullptr;
            sink_registry.stagedFrame() = std::move(staged);
            return Status::ok;
        }
        return publishToSink(*sink, frame);
    }

    // ---- The C-ABI shim. Validate the descriptor and handles, take the
    // lock, forward. Nothing below decides anything else. ----

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
        return runtime->acquireStagedPoseLocked(*desc);
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
        return runtime->resolveStagedSourceNodeLocked(*desc);
    }

    static Status resolveRigSourceNode(void *context,
                                       ResolveRigSourceNodeDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (desc->struct_size < sizeof(*desc)) return Status::invalid_argument;
        if (desc->version != poseStagingDescriptorVersionV1)
            return Status::unsupported_version;
        if (desc->reserved0 != 0 || desc->reserved1 != 0 || desc->reserved2 != 0)
            return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->resolveRigSourceNodeLocked(*desc);
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
        return runtime->getCurrentOwnerLocked(*desc);
    }

    static Status resolveSink(void *context, ResolveAnimationSinkDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if ((desc->object_name == nullptr && desc->object_name_size != 0) ||
            desc->sink_kind > AnimationSinkKind::expression_curve)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->resolveSinkLocked(*desc);
    }

    static Status resolveInstance(void *context, ResolveAnimationInstanceDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->resolveInstanceLocked(*desc);
    }

    static Status resolveRig(void *context, ResolveAnimationRigDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->resolveRigLocked(*desc);
    }

    static Status resolveLayout(void *context, ResolvePoseLayoutDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->resolveLayoutLocked(*desc);
    }

    static Status resolveClip(void *context, ResolveAnimationClipDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0 || (desc->clip_name == nullptr && desc->clip_name_size != 0))
            return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->resolveClipLocked(*desc);
    }

    static Status beginPoseFrame(void *context, BeginPoseArenaFrameDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->frame_revision == 0) return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->beginPoseFrameLocked(*desc);
    }

    static Status acquirePose(void *context, AcquirePoseDescV1 *desc) {
        if (!context || !desc || !desc->out_view) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0) return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->acquirePoseLocked(*desc);
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
        return runtime->getClipMetadataLocked(*metadata);
    }

    static Status createCursor(void *context, CreateClipCursorDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->createCursorLocked(*desc);
    }

    static Status destroyCursor(void *context, const DestroyClipCursorDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->destroyCursorLocked(*desc);
    }

    static Status samplePose(void *context, const SamplePoseAtDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (!std::isfinite(desc->time_seconds)) return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->samplePoseLocked(*desc);
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
        return runtime->sampleAnimationSourceLocked(*desc);
    }

    static Status blendPoses(void *context, const BlendNormalDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0 || (desc->layer_count != 0 && desc->layers == nullptr))
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->blendPosesLocked(*desc);
    }

    static Status localToModelJob(void *context, LocalToModelDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->localToModelLocked(*desc);
    }

    static Status buildPalette(void *context, BuildSkinPaletteDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0) return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->buildPaletteLocked(*desc);
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
        return runtime->registerPhaseLocked(*desc);
    }

    static Status unregisterPhase(void *context, const UnregisterAnimationPhaseDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->unregisterPhaseLocked(*desc);
    }

    static Status claimSource(void *context, ClaimAnimationSourceDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0) return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->claimSourceLocked(
            desc->owner, desc->sink, desc->source_ordinal,
            AnimationSourceAuthorityV1::graph_apply, desc->source);
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
        return runtime->claimSourceLocked(desc->owner, desc->sink,
                                          desc->source_ordinal, desc->authority,
                                          desc->source);
    }

    static Status releaseSource(void *context, const ReleaseAnimationSourceDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->releaseSourceLocked(*desc);
    }

    static Status handoffSource(void *context, HandoffAnimationSourceDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0) return Status::reserved_not_zero;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->handoffSourceLocked(*desc);
    }

    static Status notify(void *context, const AnimationNotificationDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (desc->reserved2 != 0 || desc->kind > AnimationNotificationKind::layout_generation_mismatch ||
            !std::isfinite(desc->time_seconds) || desc->notification_revision == 0)
            return Status::invalid_argument;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->sink_registry.notify(*desc);
    }

    static Status publishFromSource(void *context, const PublishAnimationFrameFromSourceDescV1 *desc) {
        if (!context || !desc) return Status::invalid_argument;
        if (const auto status = validateDescriptor(*desc); status != Status::ok) return status;
        if (const auto status = validateDescriptor(desc->frame); status != Status::ok) return status;
        auto *runtime = self(context);
        std::scoped_lock lock{runtime->mutex};
        return runtime->publishFromSourceLocked(*desc);
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

bool AnimationServiceRuntime::unregisterObject(std::string_view name) {
    std::scoped_lock lock{impl_->mutex};
    return impl_->unregisterObject(name);
}

void AnimationServiceRuntime::synchronizeSceneObjects() {
    impl_->synchronizeSceneObjects();
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

AnimationServiceRuntime::HandleTableSizes
AnimationServiceRuntime::handleTableSizesForTesting() const noexcept {
    std::scoped_lock lock{impl_->mutex};
    return {impl_->sink_registry.sourceCount(),
            impl_->phase_scheduler.phaseCount(), impl_->cursors.size(),
            impl_->owners.size()};
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
        produced.resolve_rig_source_node =
            AnimationServiceRuntime::Impl::resolveRigSourceNode;
        std::memcpy(out_service, &produced,
                    std::min<std::size_t>(caller_size, sizeof(produced)));
        return Status::ok;
    } catch (...) {
        return Status::out_of_memory;
    }
}

} // namespace Pelican::Animation
