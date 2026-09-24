#include "gamecontext.hpp"

#include "../appflow/enginetime.hpp"
#include "../build_features.hpp"
#include "../ecs/core.hpp"
#include "../ecs/predefined/animationsystem.hpp"
#include "../renderingpass/computetask.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "components/predefined.hpp"
#include "deterministicrng.hpp"
#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif
#include "../ecs/predefined/transform.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "../launchconfig.hpp"
#include "../loader/scene.hpp"
#include "../loader/effecttransaction.hpp"
#include "../light/lightcontainer.hpp"
#include "../log.hpp"
#include "../os/inputstate.hpp"
#include "../persistence/persistence.hpp"
#if PELICAN_WITH_PHYSICS
#include "../phys/physworld.hpp"
#endif
#include "../renderer/camera.hpp"
#include "../renderer/debugtext.hpp"
#include "../renderingpass/computetask.hpp"
#include "../ui/module.hpp"
#include "valuegraph.hpp"

#include <cmath>
#include <optional>
#include <type_traits>
#include <utility>

namespace Pelican {

bool GameContext::actionsConfigured() const {
    return Actions::isConfigured();
}

bool GameContext::actionPressed(std::string_view action_name) const {
    return Actions::isPressed(action_name);
}

bool GameContext::actionReleased(std::string_view action_name) const {
    return Actions::isReleased(action_name);
}

bool GameContext::actionHeld(std::string_view action_name) const {
    return Actions::isHeld(action_name);
}

float GameContext::actionAxis1(std::string_view action_name) const {
    return Actions::axis1(action_name);
}

ActionAxis2 GameContext::actionAxis2(std::string_view action_name) const {
    return Actions::axis2(action_name);
}

ActionPose GameContext::actionPose(std::string_view action_name) const {
    return Actions::pose(action_name);
}

double GameContext::time() const {
    return GET_MODULE(EngineTime).now();
}

double GameContext::deltaTime() const {
    return GET_MODULE(EngineTime).dt();
}

std::uint64_t GameContext::frameIndex() const {
    return GET_MODULE(EngineTime).frameIndex();
}

void GameContext::logInfo(std::string_view message) const {
    LOG_INFO(logger, "{}", std::string{message});
}

void GameContext::logWarning(std::string_view message) const {
    LOG_WARNING(logger, "{}", std::string{message});
}

void GameContext::logError(std::string_view message) const {
    LOG_ERROR(logger, "{}", std::string{message});
}

GameObjectId GameContext::createObject(const LocalTransformComponent &transform) const {
    if (internal::behaviorCallbackActive()) {
        GET_MODULE(BehaviorAttachmentArena).deferCreateObject(transform);
        return invalidGameObjectId;
    }
    const auto id = GameObjects::add()
                        .addComponent<TransformComponent>()
                        .addComponent<LocalTransformComponent>(transform)
                        .finish();
    (void)GameObjects::setLocalTransform(id, transform);
    return id;
}

GameObjectId GameContext::createSpriteObject(const LocalTransformComponent &transform,
                                             const SpriteViewComponent &sprite) const {
    sprite.validate();
    if (internal::behaviorCallbackActive()) {
        GET_MODULE(BehaviorAttachmentArena).deferCreateSpriteObject(transform,
                                                                    sprite);
        return invalidGameObjectId;
    }
    const auto id = GameObjects::add()
                        .addComponent<TransformComponent>()
                        .addComponent<LocalTransformComponent>(transform)
                        .addComponent<SpriteViewComponent>(sprite)
                        .finish();
    (void)GameObjects::setLocalTransform(id, transform);
    return id;
}

bool GameContext::removeObject(GameObjectId id) const {
    return GameObjects::remove(id);
}

LocalTransformComponent GameContext::localTransform(GameObjectId id) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        if (const auto *value = effect->retiredComponent(
                id, ComponentIdByType<LocalTransformComponent>::value)) {
            return *static_cast<const LocalTransformComponent *>(value);
        }
    }
    return GameObjects::localTransform(id);
}

bool GameContext::setLocalTransform(GameObjectId id, const LocalTransformComponent &transform) const {
    if (auto *effect = internal::activeEffectTransaction();
        effect != nullptr && effect->rejectRetiredMutation(
                                 "setLocalTransform", id)) return false;
    return GameObjects::setLocalTransform(id, transform);
}

std::optional<SpriteViewComponent> GameContext::spriteView(GameObjectId id) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        if (const auto *value = effect->retiredComponent(
                id, ComponentIdByType<SpriteViewComponent>::value)) {
            return *static_cast<const SpriteViewComponent *>(value);
        }
    }
    const auto *sprite = GET_MODULE(ECSCore).getTemplatePublicModule().tryComponent<SpriteViewComponent>(id);
    if (sprite == nullptr) return std::nullopt;
    return *sprite;
}

bool GameContext::setSpriteView(GameObjectId id, const SpriteViewComponent &sprite) const {
    if (auto *effect = internal::activeEffectTransaction();
        effect != nullptr && effect->rejectRetiredMutation(
                                 "setSpriteView", id)) return false;
    sprite.validate();
    return GET_MODULE(ECSCore).getTemplatePublicModule().setComponent<SpriteViewComponent>(id, sprite);
}

bool GameContext::setSpriteTexture(GameObjectId id, std::string_view texture) const {
    if (auto *effect = internal::activeEffectTransaction();
        effect != nullptr && effect->rejectRetiredMutation(
                                 "setSpriteTexture", id)) return false;
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    const auto *current = ecs.tryComponent<SpriteViewComponent>(id);
    if (current == nullptr) return false;
    auto replacement = *current;
    replacement.texture = std::string{texture};
    replacement.validate();
    return ecs.setComponent<SpriteViewComponent>(id, replacement);
}

namespace {

// Whether a ui.* sink of the published render config owns (path, property).
// The published generation is the one whose graph writes the UI each frame.
bool uiSinkClaims(const RenderingPassContainer &passes, std::string_view path, ui::UiProperty property) {
    const auto generation = passes.snapshot();
    if (generation == nullptr) return false;
    const auto wanted = property == ui::UiProperty::value ? ValueGraph::UiSinkProperty::value
                      : property == ui::UiProperty::visible ? ValueGraph::UiSinkProperty::visible
                                                            : std::optional<ValueGraph::UiSinkProperty>{};
    if (!wanted) return false;
    for (const auto &[id, program] : generation->programs) {
        (void)id;
        const auto &graph = program.frame_graph.value_graph;
        if (!graph) continue;
        for (const auto &sink : graph->uiSinkBindings())
            if (sink.property == *wanted && sink.path == path) return true;
    }
    return false;
}

UiWriteResult writeUi(std::string_view document_key, std::string_view path, ui::UiWrite write) {
    auto &passes = GET_MODULE(RenderingPassContainer);
    // A project has a UI when its render config references the ui feature;
    // the module is not even created otherwise.
    if (!passes.isFeatureEnabled("ui")) return UiWriteError::feature_unreferenced;
    auto &module = GET_MODULE(ui::UiModule);
    const auto status = module.queueWrite(document_key, path, std::move(write),
        [&](std::string_view claimed_path, ui::UiProperty property) {
            return uiSinkClaims(passes, claimed_path, property);
        });
    switch (status) {
    case ui::UiWriteStatus::ok: return {};
    case ui::UiWriteStatus::node_unknown: return UiWriteError::node_unknown;
    case ui::UiWriteStatus::property_mismatch: return UiWriteError::property_mismatch;
    case ui::UiWriteStatus::sink_conflict: return UiWriteError::sink_conflict;
    }
    return UiWriteError::node_unknown;
}

} // namespace

UiContext::UiContext(std::string_view document_key) noexcept : document_key_{document_key} {}

UiWriteResult UiContext::setText(std::string_view path, std::string_view text) const {
    return writeUi(document_key_, path, {.property = ui::UiProperty::text, .text = std::string{text}});
}

UiWriteResult UiContext::setVisible(std::string_view path, bool visible) const {
    return writeUi(document_key_, path, {.property = ui::UiProperty::visible, .visible = visible});
}

UiWriteResult UiContext::setValue(std::string_view path, float value) const {
    return writeUi(document_key_, path, {.property = ui::UiProperty::value, .value = value});
}

Resolved<ObjectRef> ObjectsContext::resolve(std::string_view name) const {
    return GET_MODULE(SceneLoader).resolveObject(name);
}

Resolved<GameObjectId> ObjectsContext::materialize(const ObjectRef &ref) const {
    return GET_MODULE(SceneLoader).materializeObject(ref);
}

Resolved<WorldTransform> ObjectsContext::worldTransform(const ObjectRef &ref) const {
    const auto object = materialize(ref);
    if (!object) return object.error();
    const auto *world = GET_MODULE(ECSCore).getTemplatePublicModule()
                            .tryComponent<TransformComponent>(*object.get());
    if (world == nullptr) return ObjectError::missing_capability;
    return WorldTransform{
        .pos = vec3{world->pos.x, world->pos.y, world->pos.z},
        .rotation = quat{world->rotation.x, world->rotation.y,
                         world->rotation.z, world->rotation.w},
        .scale = vec3{world->scale.x, world->scale.y, world->scale.z},
    };
}

bool RenderContext::setDirectionalLightDirection(std::string_view name, vec3 direction) const {
    return GET_MODULE(LightContainer).setDirectionalLightDirection(
        std::string{name}, glm::vec3{direction.x, direction.y, direction.z});
}

void RenderContext::setRayTracingDispatchExtent(
    std::string_view task_name, std::uint32_t width, std::uint32_t height,
    std::uint32_t depth) const {
    if (internal::activeEffectTransaction() != nullptr)
        throw SceneTransactionMutationRejected{"setRayTracingDispatchExtent"};
    auto &tasks = GET_MODULE(ComputeTaskContainer);
    const auto task = tasks.getComputeTaskIdByName(std::string{task_name});
    if (task.value < 0) {
        throw std::invalid_argument(
            "pelican.ray_tracing.task_not_found@1: task '" + std::string{task_name} + "'");
    }
    tasks.setRayTracingDispatchExtent(task, width, height, depth);
}

void RenderContext::setRayTracingCallableRecord(
    std::string_view task_name, std::uint32_t index, const nlohmann::json &values) const {
    if (internal::activeEffectTransaction() != nullptr) {
        throw SceneTransactionMutationRejected{"setRayTracingCallableRecord"};
    }
    auto &tasks = GET_MODULE(ComputeTaskContainer);
    const auto task = tasks.getComputeTaskIdByName(std::string{task_name});
    if (task.value < 0) {
        throw std::invalid_argument(
            "pelican.ray_tracing.task_not_found@1: task '" + std::string{task_name} + "'");
    }
    tasks.setCallableRecord(task, index, values);
}

bool RenderContext::setDirectionalLightIntensity(std::string_view name, float intensity) const {
    return GET_MODULE(LightContainer).setDirectionalLightIntensity(std::string{name}, intensity);
}

bool RenderContext::setPointLightPosition(std::string_view name, vec3 position) const {
    return GET_MODULE(LightContainer).setPointLightPosition(
        std::string{name}, glm::vec3{position.x, position.y, position.z});
}

bool RenderContext::setSpotLightDirection(std::string_view name, vec3 direction) const {
    return GET_MODULE(LightContainer).setSpotLightDirection(
        std::string{name}, glm::vec3{direction.x, direction.y, direction.z});
}

// PG-2: a facade whose module the build left out answers every call with one
// failure, BuildFeatureCallDisabledError (build_feature_disabled@1 plus the
// option name). The scene effect transaction throws the same type for the
// audio calls it stages.
#if !PELICAN_WITH_PHYSICS
namespace {
[[noreturn]] void physicsDisabled(const char *entry) {
    throwBuildFeatureCallDisabled("PELICAN_WITH_PHYSICS", entry);
}
} // namespace
#endif
#if !PELICAN_WITH_AUDIO
namespace {
[[noreturn]] void audioDisabled(const char *entry) {
    throwBuildFeatureCallDisabled("PELICAN_WITH_AUDIO", entry);
}
} // namespace
#endif

// PG-2b: Behavior::onDestroy is noexcept, so a facade that fails there
// because the build left its module out, or no provider answers the query,
// would end the process. Inside a destruction callback the refusal is
// recorded instead (internal::absorbDestroyRefusal) and the facade answers
// empty: a default-constructed value -- no hits, nullopt, an invalid handle,
// false, 0. Every other failure, and every refusal outside a destruction
// callback, still throws.
namespace {
using EffectApi = internal::PreparedEffectTransaction::ErrorApi;
using EffectCode = internal::PreparedEffectTransaction::ErrorCode;
using EffectFields = internal::PreparedEffectTransaction::ErrorFields;

template <class Call>
auto answerEmptyInDestroy(EffectApi api, EffectFields fields,
                          std::uint64_t value, Call &&call) -> decltype(call()) {
    using Answer = decltype(call());
    try {
        return call();
    } catch (const BuildFeatureCallDisabledError &error) {
        if (!internal::absorbDestroyRefusal(api, EffectCode::build_feature_disabled,
                                            fields, value, error.what())) {
            throw;
        }
    } catch (const phys::ProviderUnavailableError &error) {
        if (!internal::absorbDestroyRefusal(
                api, EffectCode::physics_provider_unavailable, fields, value,
                error.what())) {
            throw;
        }
    }
    if constexpr (!std::is_void_v<Answer>) return Answer{};
}

template <class Call>
auto answerEmptyInDestroy(EffectApi api, Call &&call) -> decltype(call()) {
    return answerEmptyInDestroy(api, EffectFields::reason, 0,
                                std::forward<Call>(call));
}

template <class Call>
auto answerEmptyInDestroy(EffectApi api, SoundHandle handle, Call &&call)
    -> decltype(call()) {
    return answerEmptyInDestroy(api, EffectFields::handle_reason, handle.value,
                                std::forward<Call>(call));
}
} // namespace

std::vector<phys::RaycastQueryHit> PhysicsContext::raycastAll(
    const phys::Ray &ray, const phys::QueryFilter &filter) const {
    return answerEmptyInDestroy(EffectApi::raycast_all, [&]() -> std::vector<phys::RaycastQueryHit> {
#if PELICAN_WITH_PHYSICS
        return GET_MODULE(PhysWorld).raycastAll(ray, filter);
#else
        (void)ray;
        (void)filter;
        physicsDisabled("raycastAll");
#endif
    });
}

std::optional<phys::RaycastQueryHit> PhysicsContext::raycastClosest(
    const phys::Ray &ray, const phys::QueryFilter &filter) const {
    return answerEmptyInDestroy(EffectApi::raycast_closest, [&]() -> std::optional<phys::RaycastQueryHit> {
#if PELICAN_WITH_PHYSICS
        return GET_MODULE(PhysWorld).raycastClosest(ray, filter);
#else
        (void)ray;
        (void)filter;
        physicsDisabled("raycastClosest");
#endif
    });
}

std::vector<phys::OverlapHit> PhysicsContext::overlapAll(
    const phys::Shape &shape, const phys::QueryFilter &filter) const {
    return answerEmptyInDestroy(EffectApi::overlap_all, [&]() -> std::vector<phys::OverlapHit> {
#if PELICAN_WITH_PHYSICS
        return GET_MODULE(PhysWorld).overlapAllHits(shape, filter);
#else
        (void)shape;
        (void)filter;
        physicsDisabled("overlapAll");
#endif
    });
}

std::vector<phys::ShapeCastQueryHit> PhysicsContext::shapeCastAll(
    const phys::Shape &moving_shape, vec3 delta,
    const phys::QueryFilter &filter) const {
    return answerEmptyInDestroy(EffectApi::shape_cast_all, [&]() -> std::vector<phys::ShapeCastQueryHit> {
#if PELICAN_WITH_PHYSICS
        return GET_MODULE(PhysWorld).shapeCastAll(moving_shape, delta, filter);
#else
        (void)moving_shape;
        (void)delta;
        (void)filter;
        physicsDisabled("shapeCastAll");
#endif
    });
}

std::optional<phys::ShapeCastQueryHit> PhysicsContext::shapeCastClosest(
    const phys::Shape &moving_shape, vec3 delta,
    const phys::QueryFilter &filter) const {
    return answerEmptyInDestroy(EffectApi::shape_cast_closest, [&]() -> std::optional<phys::ShapeCastQueryHit> {
#if PELICAN_WITH_PHYSICS
        return GET_MODULE(PhysWorld).shapeCastClosest(moving_shape, delta, filter);
#else
        (void)moving_shape;
        (void)delta;
        (void)filter;
        physicsDisabled("shapeCastClosest");
#endif
    });
}

void RenderContext::setCamera(std::string_view name) const {
    GET_MODULE(Camera).setActiveCamera(name);
}

Resolved<phys::Ray, RenderError> RenderContext::screenRay(vec2 position_px) const {
    if (!std::isfinite(position_px.x) || !std::isfinite(position_px.y)) {
        return RenderError::screen_position_invalid;
    }
    // An XR frame draws each eye from its own view (openxrviewspace.cpp);
    // only the flat frame is one camera over the whole framebuffer.
    if (GET_MODULE(EngineLaunchConfig).xr_active) return RenderError::screen_ray_unavailable;
    const auto ray = GET_MODULE(Camera).rayThroughPixel(glm::vec2{position_px.x, position_px.y});
    phys::Ray result;
    result.origin = vec3{ray.origin.x, ray.origin.y, ray.origin.z};
    result.direction = vec3{ray.direction.x, ray.direction.y, ray.direction.z};
    return result;
}

PointerState InputContext::pointer() const {
    return GET_MODULE(InputState).pointer();
}

void InputContext::setCursorMode(CursorMode mode) const {
    GET_MODULE(InputState).requestCursorMode(mode);
}

double GameContext::random() {
    return GET_MODULE(DeterministicRng).random();
}

int GameContext::randomInt(int min, int max) {
    return GET_MODULE(DeterministicRng).randomInt(min, max);
}

float GameContext::randomFloat(float min, float max) {
    return GET_MODULE(DeterministicRng).randomFloat(min, max);
}

void GameContext::setSeed(std::uint64_t seed) {
    GET_MODULE(DeterministicRng).setSeed(seed);
}

std::uint64_t GameContext::seed() const {
    return GET_MODULE(DeterministicRng).seed();
}

SoundHandle AudioContext::playSound(std::string_view path) const {
    return playSound(path, "se");
}

SoundHandle AudioContext::playSound(std::string_view path, std::string_view bus) const {
    return answerEmptyInDestroy(EffectApi::play_sound, [&]() -> SoundHandle {
        if (auto *effect = internal::activeEffectTransaction()) {
            return effect->playSound(path, bus);
        }
#if PELICAN_WITH_AUDIO
        return GET_MODULE(Audio).playSound(path, audioVoiceBusFromName(bus));
#else
        (void)bus;
        audioDisabled("playSound");
#endif
    });
}

Animation::Status AnimationContext::setParameter(GameObjectId object, std::string_view name, double value) const {
    if (internal::activeEffectTransaction()) return Animation::Status::phase_order_error;
#if PELICAN_WITH_ANIMATION
    return GET_MODULE(AnimationSystem).setGraphParameter(object, name, value);
#else
    (void)object; (void)name; (void)value;
    return Animation::Status::not_found;
#endif
}

Animation::Status AnimationContext::forceState(GameObjectId object, std::string_view state) const {
    if (internal::activeEffectTransaction()) return Animation::Status::phase_order_error;
#if PELICAN_WITH_ANIMATION
    return GET_MODULE(AnimationSystem).forceGraphState(object, state);
#else
    (void)object; (void)state;
    return Animation::Status::not_found;
#endif
}

void AudioContext::stopSound(SoundHandle handle) const {
    return answerEmptyInDestroy(EffectApi::stop_sound, handle, [&]() -> void {
        if (auto *effect = internal::activeEffectTransaction()) {
            effect->stopSound(handle);
            return;
        }
#if PELICAN_WITH_AUDIO
        GET_MODULE(Audio).stopSound(handle);
#else
        (void)handle;
        audioDisabled("stopSound");
#endif
    });
}

void AudioContext::setBusVolume(std::string_view bus, float volume) const {
    return answerEmptyInDestroy(EffectApi::set_bus_volume, [&]() -> void {
        if (auto *effect = internal::activeEffectTransaction()) {
            effect->setBusVolume(bus, volume);
            return;
        }
#if PELICAN_WITH_AUDIO
        GET_MODULE(Audio).setBusVolume(bus, volume);
#else
        (void)bus;
        (void)volume;
        audioDisabled("setBusVolume");
#endif
    });
}

float AudioContext::busVolume(std::string_view bus) const {
    return answerEmptyInDestroy(EffectApi::bus_volume, [&]() -> float {
        if (auto *effect = internal::activeEffectTransaction()) {
            return effect->busVolume(bus);
        }
#if PELICAN_WITH_AUDIO
        return GET_MODULE(Audio).busVolume(bus);
#else
        (void)bus;
        audioDisabled("busVolume");
#endif
    });
}

bool AudioContext::isPlaying(SoundHandle handle) const {
    return answerEmptyInDestroy(EffectApi::is_playing, handle, [&]() -> bool {
        if (auto *effect = internal::activeEffectTransaction()) {
            return effect->isPlaying(handle);
        }
#if PELICAN_WITH_AUDIO
        return GET_MODULE(Audio).isPlaying(handle);
#else
        (void)handle;
        audioDisabled("isPlaying");
#endif
    });
}

namespace {
std::shared_ptr<const ValueGraph::Graph> graphForOwnedInput(std::string_view input, auto ownerOf) {
    const auto *passes = FastModuleContainer::tryGet<RenderingPassContainer>();
    const auto generation = passes ? passes->snapshot() : nullptr;
    if (!generation) return {};
    std::shared_ptr<const ValueGraph::Graph> graph;
    for (const auto &[id, program] : generation->programs) {
        const auto &candidate = program.frame_graph.value_graph;
        if (!candidate || !(candidate.get()->*ownerOf)(input)) continue;
        if (graph && graph != candidate) return {};
        graph = candidate;
    }
    return graph;
}
}

bool RenderContext::setValueGraphParameter(GameObjectId object, std::string_view parameter, std::optional<double> value) const {
    if (internal::activeEffectTransaction()) return false;
    auto graph = graphForOwnedInput(parameter, &ValueGraph::Graph::parameterOwner);
    // A port an edge supplies has no parameter at all, so the lookup above
    // finds nothing. Look again for the shadowed port so SceneLoader can refuse
    // it by name instead of returning the same false as an unknown parameter.
    if (!graph) graph = graphForOwnedInput(parameter, &ValueGraph::Graph::shadowedParameterOwner);
    return graph && GET_MODULE(SceneLoader).setGraphParameter(std::move(graph), object, parameter, value);
}

bool RenderContext::emitValueGraphEvent(GameObjectId object, std::string_view input, Animation::CurvePlaybackCommand command) const {
    if (internal::activeEffectTransaction()) return false;
    auto graph = graphForOwnedInput(input, &ValueGraph::Graph::eventOwner);
    return graph && GET_MODULE(SceneLoader).emitGraphEvent(std::move(graph), object, input, command);
}

bool RenderContext::setRenderValues(std::string_view buffer, std::span<const float> values) const {
    if (internal::activeEffectTransaction()) return false;
    return GET_MODULE(FrameGraphResourceContainer).stageRuntimeValues(buffer, values);
}

bool AudioContext::setSoundVolume(SoundHandle handle, float volume) const {
    return answerEmptyInDestroy(EffectApi::set_sound_volume, handle, [&]() -> bool {
        if (internal::activeEffectTransaction()) return false;
#if PELICAN_WITH_AUDIO
        return GET_MODULE(Audio).setSoundVolume(handle, volume);
#else
        audioDisabled("setSoundVolume");
#endif
    });
}

bool AudioContext::setSoundPitch(SoundHandle handle, float pitch) const {
    return answerEmptyInDestroy(EffectApi::set_sound_pitch, handle, [&]() -> bool {
        if (internal::activeEffectTransaction()) return false;
#if PELICAN_WITH_AUDIO
        return GET_MODULE(Audio).setSoundPitch(handle, pitch);
#else
        audioDisabled("setSoundPitch");
#endif
    });
}

std::optional<SoundPlaybackState> AudioContext::soundState(SoundHandle handle) const {
    return answerEmptyInDestroy(EffectApi::sound_state, handle, [&]() -> std::optional<SoundPlaybackState> {
#if PELICAN_WITH_AUDIO
        return GET_MODULE(Audio).soundState(handle);
#else
        audioDisabled("soundState");
#endif
    });
}

bool AudioContext::setSoundPaused(SoundHandle handle, bool paused) const {
    return answerEmptyInDestroy(EffectApi::set_sound_paused, handle, [&]() -> bool {
        if (internal::activeEffectTransaction()) return false;
#if PELICAN_WITH_AUDIO
        return GET_MODULE(Audio).setSoundPaused(handle, paused);
#else
        audioDisabled("setSoundPaused");
#endif
    });
}

void GameContext::loadScene(std::string_view name) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        effect->loadScene(name);
        return;
    }
    GET_MODULE(SceneLoader).requestLoad(std::string{name});
}

std::string GameContext::currentScene() const {
    return GET_MODULE(SceneLoader).currentScene();
}

void RenderContext::debugText(int x, int y, std::string_view text, vec4 color, int scale) const {
    GET_MODULE(DebugText).text(x, y, text, {color.x, color.y, color.z, color.w}, scale);
}

GameContext::Json GameContext::gameSettings() const {
    if (auto *effect = internal::activeEffectTransaction()) {
        return effect->gameSettings();
    }
    return GET_MODULE(Persistence).gameSettings();
}

void GameContext::setGameSettings(Json settings) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        effect->setGameSettings(std::move(settings));
        return;
    }
    GET_MODULE(Persistence).setGameSettings(std::move(settings));
}

void GameContext::saveSettings() const {
    if (auto *effect = internal::activeEffectTransaction()) {
        effect->saveSettings();
        return;
    }
    auto &persistence = GET_MODULE(Persistence);
#if PELICAN_WITH_AUDIO
    persistence.captureAudioSettings(GET_MODULE(Audio));
#endif
    persistence.saveSettings();
}

void SaveContext::saveData(std::string_view slot, const Json &data) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        effect->saveData(slot, data);
        return;
    }
    GET_MODULE(Persistence).saveData(slot, data);
}

std::optional<SaveContext::Json> SaveContext::loadData(std::string_view slot) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        return effect->loadData(slot);
    }
    return GET_MODULE(Persistence).loadData(slot);
}

std::vector<SaveContext::SlotInfo> SaveContext::listSaves() const {
    const auto saves = internal::activeEffectTransaction()
        ? internal::activeEffectTransaction()->listSaves()
        : GET_MODULE(Persistence).listSaves();
    std::vector<SlotInfo> result;
    result.reserve(saves.size());
    for (const auto &save : saves) {
        result.push_back(SlotInfo{.slot = save.slot, .timestamp = save.timestamp});
    }
    return result;
}

} // namespace Pelican
