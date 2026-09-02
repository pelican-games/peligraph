#include "../src/core/communication/editorprojectionplan.hpp"
#include "../src/core/communication/editorruntimefactory.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/componentinfo.hpp"
#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/log.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/userpublic/behavior.hpp"
#include "../src/core/userpublic/details/component/registerer.hpp"
#include "../src/core/userpublic/details/ecs/componentdeclare.hpp"
#include "../src/core/userpublic/details/ecs/coretemplate.hpp"
#include "authoringscenetestsupport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <vector>

namespace Pelican {

struct Wp375FooComponent {
    std::int32_t value = 0;
    friend bool operator==(const Wp375FooComponent &,
                           const Wp375FooComponent &) = default;
};

DECLARE_COMPONENT(Wp375FooComponent, 62);

namespace {

using Catch::Matchers::ContainsSubstring;
using Json = nlohmann::ordered_json;

struct Wp375AmbientBehaviorParams {
    std::int32_t value = 0;
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Wp375AmbientBehaviorParams::value>("value"),
                  std::int32_t{0}));
};

class Wp375AmbientBehavior final : public Behavior {
  public:
    using Params = Wp375AmbientBehaviorParams;
};

std::string canonicalMatrixBehavior(const nlohmann::json &params) {
    return Json{params}.dump();
}

EditorProjectionContext projectionContext(
    EditorProjectionMode mode,
    std::vector<ComponentCodec> codecs =
        std::vector<ComponentCodec>{componentCodecs().begin(),
                                    componentCodecs().end()},
    std::vector<EditorBehaviorPolicy> behaviors = {
        EditorBehaviorPolicy{.stable_name = "wp375_matrix_behavior",
                             .schema_version = 1,
                             .canonicalize = canonicalMatrixBehavior,
                             .owner_generation = 7}},
    EditorBehaviorAvailability availability =
        EditorBehaviorAvailability::Active,
    ComponentCodecCanonicalDefaults defaults = {}) {
    const auto layout_mode =
        std::any_of(codecs.begin(), codecs.end(), [](const ComponentCodec &codec) {
            return codec.name == "ray_instance";
        })
            ? EditorLayoutSemanticMode::LayoutV2
            : EditorLayoutSemanticMode::Legacy;
    const auto codec_generation =
        editorProjectionCodecGeneration(codecs, layout_mode, defaults);
    const auto behavior_generation =
        editorProjectionBehaviorGeneration(behaviors, availability);
    return EditorProjectionContext{
        "main", mode, std::move(codecs), layout_mode, codec_generation,
        availability, std::move(behaviors), behavior_generation, defaults};
}

AuthoringSceneDocument matrixDocument() {
    const Json scene{
        {"schema", "pelican.scene"},
        {"version", 2},
        {"scenes",
         {{"main",
           {{"objects",
             Json::array({{{"name", "Root"},
                           {"components",
                            Json::array({{{"name", "transform"},
                                          {"pos", {0.0, 0.0, 0.0}},
                                          {"rotation", {0.0, 0.0, 0.0, 1.0}},
                                          {"scale", {1.0, 1.0, 1.0}}}})}}})}}}}},
    };
    return AuthoringSceneDocument::load(scene.dump(), SceneRevision{11});
}

AuthoringObjectId matrixObjectId(
    const test_support::SceneProjectionTarget &target) {
    const auto &scenes = target.projectionState().resolved().scenes();
    REQUIRE(scenes.size() == 1);
    REQUIRE(scenes.front().objects.size() == 1);
    return scenes.front().objects.front().authoring_object_id;
}

std::size_t contextCodecIndex(const EditorProjectionContext &context,
                              std::string_view name) {
    const auto catalog = context.codecCatalog();
    const auto found = std::find_if(
        catalog.begin(), catalog.end(),
        [name](const ComponentCodec &codec) { return codec.name == name; });
    REQUIRE(found != catalog.end());
    return static_cast<std::size_t>(found - catalog.begin());
}

EditorProjectionRouteKind expectedPolicyRoute(EditorProjectionPolicy policy) {
    switch (policy) {
    case EditorProjectionPolicy::TransformClosure:
        return EditorProjectionRouteKind::TransformClosure;
    case EditorProjectionPolicy::RendererModel:
        return EditorProjectionRouteKind::RendererModel;
    case EditorProjectionPolicy::EcsCodec:
        return EditorProjectionRouteKind::EcsCodec;
    case EditorProjectionPolicy::Camera:
        return EditorProjectionRouteKind::Camera;
    case EditorProjectionPolicy::Light:
        return EditorProjectionRouteKind::Light;
    case EditorProjectionPolicy::Collider:
        return EditorProjectionRouteKind::Collider;
    }
    throw std::logic_error("WP375 test oracle received an unknown policy");
}

EditorProjectionRoute policyRoute(const EditorProjectionContext &context,
                                  const ComponentCodec &codec,
                                  AuthoringObjectId object_id,
                                  bool full_projection = false) {
    return EditorProjectionRoute{
        .kind = expectedPolicyRoute(codec.editor_policy),
        .operation_index = 0,
        .scene_id = "main",
        .object_id = object_id,
        .component = std::string{codec.name},
        .codec_index = contextCodecIndex(context, codec.name),
        .full_component_projection = full_projection,
    };
}

EditorProjectionPlan planOne(const Json &operation,
                             const test_support::SceneProjectionTarget &target,
    const EditorProjectionContext &context) {
    const std::array operations{operation};
    const auto canonical_operations = freezeEditorCanonicalOperations(
        operations, target.projectionState());
    return planEditorProjection(
        EditorProjectionPlanningRequest{
            .base_revision = target.projectionState().revision(),
            .commands = {},
            .operations = canonical_operations,
        },
        target.projectionState(), context);
}

void requirePreviewRejected(
    const Json &operation,
    const test_support::SceneProjectionTarget &target,
    const EditorProjectionContext &context) {
    try {
        (void)planOne(operation, target, context);
        FAIL("preview operation unexpectedly produced a plan");
    } catch (const EditorProjectionPlanError &error) {
        REQUIRE(error.name() == "editor_preview_unsupported@1");
    }
}

Json setOperation(AuthoringObjectId object_id, std::string_view component) {
    Json authored{{"name", component}};
    if (component == "behavior") {
        authored["type"] = "wp375_matrix_behavior";
        authored["params"] = Json{{"value", 4}};
    }
    return Json{{"op", "set_component_value"},
                {"object_id", object_id.value},
                {"component_slot", component},
                {"authored_component", std::move(authored)}};
}

Json addOperation(AuthoringObjectId object_id, std::string_view component) {
    auto result = setOperation(object_id, component);
    result["op"] = "add_component";
    result["component"] = std::move(result["authored_component"]);
    result.erase("authored_component");
    result["component_index"] = 1;
    return result;
}

Json removeOperation(AuthoringObjectId object_id,
                     std::string_view component) {
    return Json{{"op", "remove_component"},
                {"object_id", object_id.value},
                {"component_slot", component},
                {"component_index", 1}};
}

std::vector<EditorProjectionRoute> lifecycleRoutes(
    AuthoringObjectId object_id) {
    std::vector<EditorProjectionRoute> routes;
    for (const auto kind : {
             EditorProjectionRouteKind::ObjectLifecycle,
             EditorProjectionRouteKind::LifecycleRenderer,
             EditorProjectionRouteKind::LifecycleCamera,
             EditorProjectionRouteKind::LifecycleLight,
             EditorProjectionRouteKind::LifecycleCollider,
             EditorProjectionRouteKind::LifecycleBehavior,
         }) {
        routes.push_back(EditorProjectionRoute{.kind = kind,
                                               .operation_index = 0,
                                               .scene_id = "main",
                                               .object_id = object_id});
    }
    return routes;
}

std::vector<EditorProjectionRoute> fullPrefabRoutes(
    const EditorProjectionContext &context, AuthoringObjectId object_id) {
    std::vector<EditorProjectionRoute> routes{
        EditorProjectionRoute{
            .kind = EditorProjectionRouteKind::Prefab,
            .operation_index = 0,
            .scene_id = "main",
            .object_id = object_id,
            .full_component_projection = true,
        },
    };
    for (const auto &codec : context.codecCatalog()) {
        routes.push_back(policyRoute(context, codec, object_id, true));
    }
    for (const auto kind : {
             EditorProjectionRouteKind::PrefabRenderer,
             EditorProjectionRouteKind::PrefabCamera,
             EditorProjectionRouteKind::PrefabLight,
             EditorProjectionRouteKind::PrefabCollider,
         }) {
        routes.push_back(EditorProjectionRoute{
            .kind = kind,
            .operation_index = 0,
            .scene_id = "main",
            .object_id = object_id,
            .full_component_projection = true,
        });
    }
    routes.push_back(EditorProjectionRoute{
        .kind = EditorProjectionRouteKind::PrefabBehavior,
        .operation_index = 0,
        .scene_id = "main",
        .object_id = object_id,
        .full_component_projection = true,
    });
    return routes;
}

ComponentCodecValue decodeFoo(const nlohmann::json &authored) {
    return Wp375FooComponent{
        .value = authored.at("value").get<std::int32_t>()};
}

Json encodeFoo(const ComponentCodecValue &value) {
    return Json{{"name", "foo"},
                {"value", std::any_cast<const Wp375FooComponent &>(value)
                              .value}};
}

std::span<const StructFieldSchema> fooSchema() {
    static const std::array fields{
        StructFieldSchema{"value", StructFieldType::I32, std::monostate{}},
    };
    return fields;
}

void applyFoo(const ComponentCodecValue &value, void *target) {
    *static_cast<Wp375FooComponent *>(target) =
        std::any_cast<const Wp375FooComponent &>(value);
}

ComponentCodecValue projectFoo(const void *source) {
    return *static_cast<const Wp375FooComponent *>(source);
}

ComponentEditorSwapToken prepareFooSwap(
    void *raw_ecs, std::uint32_t entity_index,
    std::uint32_t entity_generation, const ComponentCodecValue &value) {
    auto &ecs = *static_cast<ECSCoreTemplatePublic *>(raw_ecs);
    using Token =
        ECSCoreTemplatePublic::PreparedComponentSwap<Wp375FooComponent>;
    return ComponentEditorSwapToken::own(std::make_unique<Token>(
        ecs.prepareComponentSwap<Wp375FooComponent>(
            EntityId{entity_index, entity_generation},
            std::any_cast<const Wp375FooComponent &>(value))));
}

void swapFooToken(ComponentEditorSwapToken &erased,
                  EditorCodecSwapPhase phase) noexcept {
    using Token =
        ECSCoreTemplatePublic::PreparedComponentSwap<Wp375FooComponent>;
    auto &token = erased.get<Token>();
    switch (phase) {
    case EditorCodecSwapPhase::Publish:
        token.publish();
        break;
    case EditorCodecSwapPhase::Rollback:
        token.rollback();
        break;
    case EditorCodecSwapPhase::Finish:
        token.finish();
        break;
    }
}

ComponentCodec fooCodec() {
    return ComponentCodec{
        "foo", ComponentCodecRuntimeKind::Ecs,
        EditorProjectionPolicy::EcsCodec, decodeFoo, encodeFoo, fooSchema,
        applyFoo, projectFoo, prepareFooSwap, swapFooToken};
}

AuthoringSceneDocument fooDocument() {
    return AuthoringSceneDocument::load(
        Json{{"schema", "pelican.scene"},
             {"version", 2},
             {"scenes",
              {{"main",
                {{"objects",
                  Json::array({{{"name", "Root"},
                                {"components",
                                 Json::array({{{"name", "foo"},
                                               {"value", 1}}})}}})}}}}}}
            .dump(),
        SceneRevision{31});
}

EditorProjectionCommand eraseFooCommand() {
    return EditorProjectionCommand{
        .object_path = "/scenes/main/objects/0/components/0",
        .apply = [](nlohmann::json &document) {
            auto &components = document.at("scenes")
                                   .at("main")
                                   .at("objects")
                                   .at(0)
                                   .at("components");
            const auto found = std::find_if(
                components.begin(), components.end(), [](const auto &value) {
                    return value.value("name", std::string{}) == "foo";
                });
            if (found == components.end()) {
                throw std::runtime_error("foo component is absent");
            }
            components.erase(found);
        },
    };
}

EditorProjectionCommand insertFooCommand(std::int32_t value) {
    return EditorProjectionCommand{
        .object_path = "/scenes/main/objects/0/components/0",
        .apply = [value](nlohmann::json &document) {
            document.at("scenes")
                .at("main")
                .at("objects")
                .at(0)
                .at("components")
                .push_back({{"name", "foo"}, {"value", value}});
        },
    };
}

} // namespace

TEST_CASE("WP375 committed route matrix is exact for every canonical operation",
          "[WP375][plan][route-matrix][mutation]") {
    FastModuleContainer modules;
    GET_MODULE(RayLayoutRuntimeState).configure(RayLayoutSemanticMode::LayoutV2);
    test_support::SceneProjectionTarget target{matrixDocument()};
    const auto object_id = matrixObjectId(target);
    const auto context = projectionContext(EditorProjectionMode::Committed);

    for (const auto &codec : context.codecCatalog()) {
        DYNAMIC_SECTION("set " << codec.name) {
            const auto plan = planOne(setOperation(object_id, codec.name),
                                      target, context);
            REQUIRE(plan.routes == std::vector{
                                       policyRoute(context, codec, object_id)});
        }
        DYNAMIC_SECTION("add " << codec.name) {
            const auto plan = planOne(addOperation(object_id, codec.name),
                                      target, context);
            auto expected =
                std::vector{policyRoute(context, codec, object_id)};
            expected.push_back(EditorProjectionRoute{
                .kind = EditorProjectionRouteKind::ComponentLifecycle,
                .operation_index = 0,
                .scene_id = "main",
                .object_id = object_id,
                .component = std::string{codec.name},
                .codec_index = contextCodecIndex(context, codec.name),
            });
            REQUIRE(plan.routes == expected);
        }
        DYNAMIC_SECTION("remove " << codec.name) {
            const auto plan = planOne(removeOperation(object_id, codec.name),
                                      target, context);
            auto expected =
                std::vector{policyRoute(context, codec, object_id)};
            expected.push_back(EditorProjectionRoute{
                .kind = EditorProjectionRouteKind::ComponentLifecycle,
                .operation_index = 0,
                .scene_id = "main",
                .object_id = object_id,
                .component = std::string{codec.name},
                .codec_index = contextCodecIndex(context, codec.name),
            });
            REQUIRE(plan.routes == expected);
        }
    }

    const auto behavior_route = EditorProjectionRoute{
        .kind = EditorProjectionRouteKind::BehaviorBatch,
        .operation_index = 0,
        .scene_id = "main",
        .object_id = object_id,
        .component = "behavior",
    };
    REQUIRE(planOne(setOperation(object_id, "behavior"), target, context)
                .routes == std::vector{behavior_route});
    REQUIRE(planOne(addOperation(object_id, "behavior"), target, context)
                .routes == std::vector{behavior_route});
    REQUIRE(planOne(removeOperation(object_id, "behavior"), target, context)
                .routes == std::vector{behavior_route});

    REQUIRE(
        planOne(Json{{"op", "reparent"}, {"object_id", object_id.value}},
                target, context)
            .routes ==
        std::vector{
            policyRoute(context, *context.findCodec("transform"), object_id),
            EditorProjectionRoute{.kind = EditorProjectionRouteKind::Reparent,
                                  .operation_index = 0,
                                  .scene_id = "main",
                                  .object_id = object_id}});

    const AuthoringObjectId spawned_id{900};
    REQUIRE(planOne(Json{{"op", "spawn"},
                         {"object_id", spawned_id.value},
                         {"scene_id", "main"}},
                    target, context)
                .routes == lifecycleRoutes(spawned_id));
    for (const auto op : {"destroy", "remove_objects"}) {
        DYNAMIC_SECTION(op) {
            REQUIRE(planOne(Json{{"op", op},
                                 {"object_ids", Json::array({object_id.value})}},
                            target, context)
                        .routes == lifecycleRoutes(object_id));
        }
    }
    const AuthoringObjectId restored_id{901};
    REQUIRE(planOne(Json{{"op", "restore_objects"},
                         {"closures",
                          Json::array({{{"authoring_object_id",
                                        restored_id.value},
                                       {"scene_id", "main"}}})}},
                    target, context)
                .routes == lifecycleRoutes(restored_id));

    for (const auto op : {"set_prefab_parameter",
                          "unset_prefab_parameter"}) {
        DYNAMIC_SECTION(op) {
            REQUIRE(planOne(Json{{"op", op}, {"object", object_id.value}},
                            target, context)
                        .routes == fullPrefabRoutes(context, object_id));
        }
    }

    const OriginTransitionMap transition{
        .direction = OriginTransitionDirection::GeneratedToAuthored,
        .entries = {{.scene_id = "main",
                     .authoring_object_id = object_id,
                     .gid = "generated",
                     .component_key = "behavior:id",
                     .final_authored_index = 2}},
    };
    for (const auto op : {"unpack_prefab", "restore_prefab"}) {
        DYNAMIC_SECTION(op) {
            const auto plan = planOne(
                Json{{"op", op},
                     {"object", object_id.value},
                     {"scene_id", "main"},
                     {"origin_transition", originTransitionMapJson(transition)}},
                target, context);
            REQUIRE(plan.routes ==
                    std::vector{
                        EditorProjectionRoute{
                            .kind = EditorProjectionRouteKind::Prefab,
                            .operation_index = 0,
                            .scene_id = "main",
                            .object_id = object_id,
                            .full_component_projection = false,
                        },
                        EditorProjectionRoute{
                            .kind = EditorProjectionRouteKind::PrefabBehavior,
                            .operation_index = 0,
                            .scene_id = "main",
                            .object_id = object_id,
                            .full_component_projection = false,
                        },
                        EditorProjectionRoute{
                            .kind =
                                EditorProjectionRouteKind::OriginTransition,
                            .operation_index = 0,
                            .scene_id = "main",
                            .object_id = object_id,
                            .origin_transition = transition,
                        }});
        }
    }
}

TEST_CASE("WP375 preview semantic gate exists only in the planner and covers every row",
          "[WP375][plan][preview][route-matrix]") {
    FastModuleContainer modules;
    GET_MODULE(RayLayoutRuntimeState).configure(RayLayoutSemanticMode::LayoutV2);
    test_support::SceneProjectionTarget target{matrixDocument()};
    const auto object_id = matrixObjectId(target);
    const auto context = projectionContext(EditorProjectionMode::Preview);

    for (const auto &codec : context.codecCatalog()) {
        DYNAMIC_SECTION("preview set " << codec.name) {
            const auto operation = setOperation(object_id, codec.name);
            if (codec.editor_policy ==
                    EditorProjectionPolicy::TransformClosure ||
                codec.editor_policy == EditorProjectionPolicy::Light) {
                REQUIRE(planOne(operation, target, context).routes ==
                        std::vector{
                            policyRoute(context, codec, object_id)});
            } else {
                requirePreviewRejected(operation, target, context);
            }
        }
        DYNAMIC_SECTION("preview add " << codec.name) {
            requirePreviewRejected(addOperation(object_id, codec.name),
                                   target, context);
        }
        DYNAMIC_SECTION("preview remove " << codec.name) {
            requirePreviewRejected(removeOperation(object_id, codec.name),
                                   target, context);
        }
    }
    requirePreviewRejected(setOperation(object_id, "behavior"), target,
                           context);

    const OriginTransitionMap transition{};
    const std::array rejected{
        Json{{"op", "reparent"}, {"object_id", object_id.value}},
        Json{{"op", "spawn"},
             {"object_id", std::uint64_t{900}},
             {"scene_id", "main"}},
        Json{{"op", "destroy"},
             {"object_ids", Json::array({object_id.value})}},
        Json{{"op", "remove_objects"},
             {"object_ids", Json::array({object_id.value})}},
        Json{{"op", "restore_objects"},
             {"closures", Json::array({{{"authoring_object_id",
                                         std::uint64_t{901}},
                                         {"scene_id", "main"}}})}},
        Json{{"op", "set_prefab_parameter"}, {"object", object_id.value}},
        Json{{"op", "unset_prefab_parameter"}, {"object", object_id.value}},
        Json{{"op", "unpack_prefab"},
             {"object", object_id.value},
             {"origin_transition", originTransitionMapJson(transition)}},
        Json{{"op", "restore_prefab"},
             {"object", object_id.value},
             {"origin_transition", originTransitionMapJson(transition)}},
    };
    for (const auto &operation : rejected) {
        DYNAMIC_SECTION("preview rejects " << operation.at("op")) {
            requirePreviewRejected(operation, target, context);
        }
    }

    std::vector<EditorProjectionRoute> restore_routes;
    for (std::size_t index = 0; index < context.codecCatalog().size(); ++index) {
        const auto &codec = context.codecCatalog()[index];
        if (codec.editor_policy != EditorProjectionPolicy::TransformClosure &&
            codec.editor_policy != EditorProjectionPolicy::Light) {
            continue;
        }
        restore_routes.push_back(EditorProjectionRoute{
            .kind = expectedPolicyRoute(codec.editor_policy),
            .scene_id = "main",
            .component = std::string{codec.name},
            .codec_index = index,
        });
    }
    const auto restore = planEditorProjection(
        EditorProjectionPlanningRequest{
            .base_revision = target.projectionState().revision(),
            .restore_committed = true,
        },
        target.projectionState(), context);
    REQUIRE(restore.routes == restore_routes);
}

TEST_CASE("WP375 immutable context makes planning independent of ambient runtime state",
          "[WP375][plan][determinism][context]") {
    FastModuleContainer modules;
    test_support::SceneProjectionTarget target{matrixDocument()};
    const auto object_id = matrixObjectId(target);
    const auto context = projectionContext(EditorProjectionMode::Committed);
    const auto collider_operation = setOperation(object_id, "collider");
    const auto behavior_operation = setOperation(object_id, "behavior");
    const auto behavior_token =
        internal::getBehaviorRegisterer()
            .registerBehavior<Wp375AmbientBehavior>(
                "wp375_matrix_behavior", 1, {});
    const auto collider_before = planOne(collider_operation, target, context);
    const auto behavior_before = planOne(behavior_operation, target, context);
    internal::unregisterBehavior(behavior_token);

    GET_MODULE(RayLayoutRuntimeState).configure(RayLayoutSemanticMode::LayoutV2);
    {
        testing::ScopedComponentCodecCanonicalDefaults changed_defaults{
            ComponentCodecCanonicalDefaults{.collider_radius = 19.0F}};
        const auto collider_after =
            planOne(collider_operation, target, context);
        const auto behavior_after =
            planOne(behavior_operation, target, context);
        REQUIRE(collider_after == collider_before);
        REQUIRE(behavior_after == behavior_before);
    }
}

TEST_CASE("WP375 stale materialization inputs fail with a named error",
          "[WP375][materialize][generation][epoch]") {
    test_support::SceneProjectionTarget target{matrixDocument()};
    const auto context = projectionContext(EditorProjectionMode::Committed);
    const auto plan = planEditorProjection(
        EditorProjectionPlanningRequest{
            .base_revision = target.projectionState().revision()},
        target.projectionState(), context);
    EditorRuntimeBindingState bindings{
        .epoch = 12,
        .codec_generation = context.layoutGeneration(),
        .behavior_generation = context.behaviorGeneration(),
    };
    const auto result = materializeEditorProjection(
        plan, EditorProjectionModules{}, target, bindings, 13,
        context.layoutGeneration(), context.behaviorGeneration());
    REQUIRE(result.status == EditorProjectionStatus::Rejected);
    REQUIRE(result.error.has_value());
    REQUIRE_THAT(result.error->message,
                 ContainsSubstring("editor_projection_stale_bindings@1"));
}

TEST_CASE("WP375 a registered foo codec reaches runtime set add and remove without adapter edits",
          "[WP375][component-codec][type-erased][materialize]") {
    setupLogger();
    FastModuleContainer modules;
    const auto entity_id_token =
        internal::getComponentRegisterer().registerComponent<EntityId>(
            "wp375_entity_id");
    const auto component_token =
        internal::getComponentRegisterer()
            .registerComponent<Wp375FooComponent>("foo");
    auto codecs = std::vector<ComponentCodec>{componentCodecs().begin(),
                                               componentCodecs().end()};
    codecs.push_back(fooCodec());
    {
        testing::ScopedComponentCodecCatalog catalog{codecs};
        test_support::SceneProjectionTarget target{fooDocument()};
        const auto object_id = matrixObjectId(target);
        ECSCoreTemplatePublic ecs;
        const std::array component_ids{
            ComponentIdByType<Wp375FooComponent>::value};
        const auto entity = ecs.createEntity(
            component_ids, [](std::span<void *> values) {
                *static_cast<Wp375FooComponent *>(values.front()) =
                    Wp375FooComponent{.value = 1};
            });
        const auto context = projectionContext(
            EditorProjectionMode::Committed, codecs, {},
            EditorBehaviorAvailability::DllUnavailable);
        EditorRuntimeBindingState bindings{
            .bindings = {{.authoring_object_id = object_id,
                          .entity = entity}},
            .epoch = 44,
            .codec_generation = context.layoutGeneration(),
            .behavior_generation = context.behaviorGeneration(),
        };
        const EditorProjectionModules projection_modules{
            .ecs = &ecs,
            .component_info = &GET_MODULE(ComponentInfoManager),
        };

        const auto execute = [&](EditorProjectionCommand command,
                                 Json operation) {
            const std::array commands{std::move(command)};
            const std::array operations{std::move(operation)};
            const auto canonical_operations = freezeEditorCanonicalOperations(
                operations, target.projectionState());
            const auto plan = planEditorProjection(
                EditorProjectionPlanningRequest{
                    .base_revision = target.projectionState().revision(),
                    .commands = commands,
                    .operations = canonical_operations,
                },
                target.projectionState(), context);
            const auto result = materializeEditorProjection(
                plan, projection_modules, target, bindings, bindings.epoch,
                context.layoutGeneration(), context.behaviorGeneration());
            INFO((result.error ? result.error->message : std::string{}));
            REQUIRE(result.committed());
        };

        execute(makeSetComponentValueCommand(
                    "main", "Root", "foo",
                    nlohmann::json{{"name", "foo"}, {"value", 2}}),
                Json{{"op", "set_component_value"},
                     {"object_id", object_id.value},
                     {"component_slot", "foo"},
                     {"authored_component",
                      Json{{"name", "foo"}, {"value", 2}}}});
        REQUIRE(ecs.component<Wp375FooComponent>(entity).value == 2);

        execute(eraseFooCommand(),
                Json{{"op", "remove_component"},
                     {"object_id", object_id.value},
                     {"component_slot", "foo"},
                     {"component_index", 0}});
        REQUIRE(ecs.tryComponent<Wp375FooComponent>(entity) == nullptr);

        execute(insertFooCommand(3),
                Json{{"op", "add_component"},
                     {"object_id", object_id.value},
                     {"component_slot", "foo"},
                     {"component_index", 0},
                     {"component", Json{{"name", "foo"}, {"value", 3}}}});
        REQUIRE(ecs.component<Wp375FooComponent>(entity).value == 3);
        ecs.clearEntities();
    }
    internal::unregisterComponent(component_token);
    internal::unregisterComponent(entity_id_token);
}

} // namespace Pelican
