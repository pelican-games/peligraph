#include "../src/core/communication/editorprojectionplan.hpp"
#include "../src/core/registrationownerhost.hpp"
#include "../src/core/communication/editorruntimefactory.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/componentinfo.hpp"
#include "../src/core/ecs/enginecomponentregistrar.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
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
            return codec.name == "pelican_ray_instance";
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

AuthoringSceneDocument twoSceneMatrixDocument() {
    const Json transform{{"name", "transform"},
                         {"pos", {0.0, 0.0, 0.0}},
                         {"rotation", {0.0, 0.0, 0.0, 1.0}},
                         {"scale", {1.0, 1.0, 1.0}}};
    return AuthoringSceneDocument::load(
        Json{{"schema", "pelican.scene"},
             {"version", 2},
             {"scenes",
              {{"main",
                {{"objects",
                  Json::array({{{"name", "Main"},
                                {"components", Json::array({transform})}}})}}},
               {"secondary",
                {{"objects",
                  Json::array({{{"name", "Secondary"},
                                {"components", Json::array({transform})}}})}}}}}}
            .dump(),
        SceneRevision{12});
}

AuthoringSceneDocument modelDocument() {
    return AuthoringSceneDocument::load(
        Json{{"schema", "pelican.scene"},
             {"version", 2},
             {"scenes",
              {{"main",
                {{"objects",
                  Json::array(
                      {{{"name", "Root"},
                        {"components",
                         Json::array(
                             {{{"name", "transform"},
                               {"pos", {0.0, 0.0, 0.0}},
                               {"rotation", {0.0, 0.0, 0.0, 1.0}},
                               {"scale", {1.0, 1.0, 1.0}}},
                              {{"name", "simplemodelview"},
                               {"model", "old"}}})}}})}}}}}}
            .dump(),
        SceneRevision{13});
}

AuthoringObjectId matrixObjectId(
    const test_support::SceneProjectionTarget &target) {
    const auto &scenes = target.projectionState().resolved().scenes();
    REQUIRE(scenes.size() == 1);
    REQUIRE(scenes.front().objects.size() == 1);
    return scenes.front().objects.front().authoring_object_id;
}

AuthoringObjectId objectIdInScene(
    const test_support::SceneProjectionTarget &target,
    std::string_view scene_id) {
    const auto *scene = target.projectionState().resolved().findScene(scene_id);
    REQUIRE(scene != nullptr);
    REQUIRE(scene->objects.size() == 1);
    return scene->objects.front().authoring_object_id;
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

struct LiteralComponentRoute {
    std::string_view component;
    EditorProjectionRouteKind kind;
    bool preview_set;
};

// Normative test oracle. This table intentionally does not read
// ComponentCodec::editor_policy: changing a stock declaration must disagree
// with the expected route rather than changing both sides of the assertion.
constexpr std::array literal_component_routes{
    LiteralComponentRoute{"animation", EditorProjectionRouteKind::EcsCodec,
                          false},
    LiteralComponentRoute{"camera", EditorProjectionRouteKind::Camera,
                          false},
    LiteralComponentRoute{"collider", EditorProjectionRouteKind::Collider,
                          false},
    LiteralComponentRoute{"light", EditorProjectionRouteKind::Light, true},
    LiteralComponentRoute{"pelican_ray_instance", EditorProjectionRouteKind::EcsCodec,
                          false},
    LiteralComponentRoute{"simplemodelview",
                          EditorProjectionRouteKind::RendererModel, false},
    LiteralComponentRoute{"sprite_view", EditorProjectionRouteKind::EcsCodec,
                          false},
    LiteralComponentRoute{"transform",
                          EditorProjectionRouteKind::TransformClosure, true},
};

const LiteralComponentRoute &literalComponentRoute(std::string_view name) {
    const auto found = std::find_if(
        literal_component_routes.begin(), literal_component_routes.end(),
        [name](const LiteralComponentRoute &entry) {
            return entry.component == name;
        });
    REQUIRE(found != literal_component_routes.end());
    return *found;
}

EditorProjectionRoute literalPolicyRoute(
    const EditorProjectionContext &context,
    const LiteralComponentRoute &expected, AuthoringObjectId object_id,
    bool full_projection = false) {
    return EditorProjectionRoute{
        .kind = expected.kind,
        .operation_index = 0,
        .scene_id = "main",
        .object_id = object_id,
        .component = std::string{expected.component},
        .codec_index = contextCodecIndex(context, expected.component),
        .full_component_projection = full_projection,
    };
}

EditorProjectionPlan requireProjectionPlan(
    const EditorProjectionPlanningRequest &request,
    const SceneProjectionState &state,
    const EditorProjectionContext &context) {
    auto result = planEditorProjection(request, state, context);
    if (!result) {
        auto failure = std::move(result).error();
        if (auto *recoverable =
                std::get_if<EditorProjectionRecoverable>(&failure)) {
            throw EditorProjectionPlanError{recoverable->name,
                                            recoverable->message};
        }
        throw ResolutionFailureException{
            StoredResolutionFailure{
                std::get<UserDataViolation>(std::move(failure))}};
    }
    return std::move(result).value();
}

EditorProjectionPlan planOne(const Json &operation,
                             const test_support::SceneProjectionTarget &target,
    const EditorProjectionContext &context) {
    const std::array operations{operation};
    const auto canonical_operations = freezeEditorCanonicalOperations(
        operations, target.projectionState());
    return requireProjectionPlan(
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
    for (const auto &expected : literal_component_routes) {
        routes.push_back(
            literalPolicyRoute(context, expected, object_id, true));
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

ResolutionResult<ComponentCodecValue> decodeFoo(
    const nlohmann::json &authored) {
    const auto value = authored.at("value").get<std::int32_t>();
    if (value < 0) {
        return ResolutionResult<ComponentCodecValue>::failure(
            UserDataViolation{
                .code = "wp378.factory.foo_negative@1",
                .tokens = {"value"},
                .fields = Json{{"value", value}},
                .leaf_message = "factory foo value must be non-negative",
            });
    }
    return ResolutionResult<ComponentCodecValue>::success(
        Wp375FooComponent{.value = value});
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

    REQUIRE(context.codecCatalog().size() == literal_component_routes.size());
    for (const auto &expected_route : literal_component_routes) {
        REQUIRE(context.findCodec(expected_route.component) != nullptr);
        DYNAMIC_SECTION("set " << expected_route.component) {
            const auto plan = planOne(
                setOperation(object_id, expected_route.component),
                                      target, context);
            REQUIRE(plan.routes == std::vector{literalPolicyRoute(
                                       context, expected_route, object_id)});
        }
        DYNAMIC_SECTION("add " << expected_route.component) {
            const auto plan = planOne(
                addOperation(object_id, expected_route.component),
                                      target, context);
            auto expected = std::vector{literalPolicyRoute(
                context, expected_route, object_id)};
            expected.push_back(EditorProjectionRoute{
                .kind = EditorProjectionRouteKind::ComponentLifecycle,
                .operation_index = 0,
                .scene_id = "main",
                .object_id = object_id,
                .component = std::string{expected_route.component},
                .codec_index = contextCodecIndex(
                    context, expected_route.component),
            });
            REQUIRE(plan.routes == expected);
        }
        DYNAMIC_SECTION("remove " << expected_route.component) {
            const auto plan = planOne(
                removeOperation(object_id, expected_route.component),
                                      target, context);
            auto expected = std::vector{literalPolicyRoute(
                context, expected_route, object_id)};
            expected.push_back(EditorProjectionRoute{
                .kind = EditorProjectionRouteKind::ComponentLifecycle,
                .operation_index = 0,
                .scene_id = "main",
                .object_id = object_id,
                .component = std::string{expected_route.component},
                .codec_index = contextCodecIndex(
                    context, expected_route.component),
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
            literalPolicyRoute(context, literalComponentRoute("transform"),
                               object_id),
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

    auto wrong_codecs = std::vector<ComponentCodec>{
        context.codecCatalog().begin(), context.codecCatalog().end()};
    const auto wrong_model = std::find_if(
        wrong_codecs.begin(), wrong_codecs.end(),
        [](const ComponentCodec &codec) {
            return codec.name == "simplemodelview";
        });
    REQUIRE(wrong_model != wrong_codecs.end());
    wrong_model->editor_policy = EditorProjectionPolicy::Camera;
    const auto wrong_context = projectionContext(
        EditorProjectionMode::Committed, std::move(wrong_codecs));
    const auto literal_model = std::vector{literalPolicyRoute(
        context, literalComponentRoute("simplemodelview"), object_id)};
    const auto correct_model = planOne(
        setOperation(object_id, "simplemodelview"), target, context);
    const auto incorrectly_declared_model = planOne(
        setOperation(object_id, "simplemodelview"), target, wrong_context);
    REQUIRE(correct_model.routes == literal_model);
    REQUIRE(incorrectly_declared_model.routes != literal_model);
    REQUIRE(incorrectly_declared_model.routes != correct_model.routes);
}

TEST_CASE(
    "WP375a committed operations outside the active scene have one literal document-only route",
    "[WP375a][plan][inactive-scene][document-only][route-matrix][mutation]") {
    FastModuleContainer modules;
    test_support::SceneProjectionTarget target{twoSceneMatrixDocument()};
    const auto object_id = objectIdInScene(target, "secondary");
    const auto context = projectionContext(EditorProjectionMode::Committed);
    const auto expected = std::vector{EditorProjectionRoute{
        .kind = EditorProjectionRouteKind::DocumentOnly,
        .operation_index = 0,
        .scene_id = "secondary",
        .object_id = object_id,
    }};
    const std::array operations{
        setOperation(object_id, "camera"),
        addOperation(object_id, "behavior"),
        Json{{"op", "reparent"}, {"object_id", object_id.value}},
        Json{{"op", "destroy"},
             {"object_ids", Json::array({object_id.value})}},
        Json{{"op", "set_prefab_parameter"},
             {"object", object_id.value}},
    };
    for (const auto &operation : operations) {
        DYNAMIC_SECTION(operation.at("op")) {
            REQUIRE(planOne(operation, target, context).routes == expected);
        }
    }
}

TEST_CASE(
    "WP375b inactive preview and its restore retain one literal document-only route",
    "[WP375b][plan][preview][inactive-scene][document-only][route-matrix][mutation]") {
    FastModuleContainer modules;
    test_support::SceneProjectionTarget target{twoSceneMatrixDocument()};
    const auto inactive_id = objectIdInScene(target, "secondary");
    const auto active_id = objectIdInScene(target, "main");
    const auto context = projectionContext(EditorProjectionMode::Preview);
    const auto inactive_operation = setOperation(inactive_id, "light");
    const auto inactive_route = EditorProjectionRoute{
        .kind = EditorProjectionRouteKind::DocumentOnly,
        .operation_index = 0,
        .scene_id = "secondary",
        .object_id = inactive_id,
    };

    REQUIRE(planOne(inactive_operation, target, context).routes ==
            std::vector{inactive_route});
    const std::array inactive_wire{inactive_operation};
    const auto inactive_canonical = freezeEditorCanonicalOperations(
        inactive_wire, target.projectionState());
    const auto inactive_restore = requireProjectionPlan(
        EditorProjectionPlanningRequest{
            .base_revision = target.projectionState().revision(),
            .operations = inactive_canonical,
            .restore_committed = true,
        },
        target.projectionState(), context);
    REQUIRE(inactive_restore.routes == std::vector{inactive_route});

    const auto active_operation = setOperation(active_id, "light");
    const auto active_route = literalPolicyRoute(
        context, literalComponentRoute("light"), active_id);
    REQUIRE(planOne(active_operation, target, context).routes ==
            std::vector{active_route});
    const std::array active_wire{active_operation};
    const auto active_canonical = freezeEditorCanonicalOperations(
        active_wire, target.projectionState());
    const auto active_restore = requireProjectionPlan(
        EditorProjectionPlanningRequest{
            .base_revision = target.projectionState().revision(),
            .operations = active_canonical,
            .restore_committed = true,
        },
        target.projectionState(), context);
    REQUIRE(active_restore.routes == std::vector{active_route});
}

TEST_CASE(
    "WP375c inactive component lifecycle and reparent previews reject before document-only routing",
    "[WP375c][plan][preview][inactive-scene][lifecycle][regression][mutation]") {
    FastModuleContainer modules;
    test_support::SceneProjectionTarget target{twoSceneMatrixDocument()};
    const auto inactive_id = objectIdInScene(target, "secondary");
    const auto context = projectionContext(EditorProjectionMode::Preview);
    const std::array rejected{
        addOperation(inactive_id, "behavior"),
        removeOperation(inactive_id, "behavior"),
        Json{{"op", "reparent"}, {"object_id", inactive_id.value}},
    };

    for (const auto &operation : rejected) {
        DYNAMIC_SECTION(operation.at("op")) {
            requirePreviewRejected(operation, target, context);
        }
    }
}

TEST_CASE(
    "WP375a document-only behavior replay still validates the active registry",
    "[WP375a][materialize][inactive-scene][behavior][validation-order]") {
    FastModuleContainer modules;
    test_support::SceneProjectionTarget target{twoSceneMatrixDocument()};
    const auto object_id = objectIdInScene(target, "secondary");
    const auto context = projectionContext(EditorProjectionMode::Committed);
    auto operation = addOperation(object_id, "behavior");
    operation.at("component").at("type") = "wp375a_unknown_inactive";
    const std::array operations{operation};
    const auto canonical = freezeEditorCanonicalOperations(
        operations, target.projectionState());
    const auto plan = requireProjectionPlan(
        EditorProjectionPlanningRequest{
            .base_revision = target.projectionState().revision(),
            .operations = canonical,
        },
        target.projectionState(), context);
    REQUIRE(plan.routes == std::vector{EditorProjectionRoute{
                               .kind =
                                   EditorProjectionRouteKind::DocumentOnly,
                               .operation_index = 0,
                               .scene_id = "secondary",
                               .object_id = object_id,
                           }});
    EditorRuntimeBindingState bindings{
        .codec_generation = context.layoutGeneration(),
        .behavior_generation = context.behaviorGeneration(),
    };
    const auto result = materializeEditorProjection(
        plan, EditorProjectionModules{}, target, bindings, bindings.epoch,
        context.layoutGeneration(), context.behaviorGeneration());
    REQUIRE(result.status == EditorProjectionStatus::Rejected);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error->code ==
            EditorProjectionErrorCode::BehaviorTypeUnknown);
    REQUIRE(result.error->data.at("type") ==
            "wp375a_unknown_inactive");
    REQUIRE(target.projectionState().revision() == SceneRevision{12});
}

TEST_CASE(
    "WP375a active unbound behavior validates type before the named binding error",
    "[WP375a][materialize][behavior][validation-order][binding][mutation]") {
    FastModuleContainer modules;
    test_support::SceneProjectionTarget target{twoSceneMatrixDocument()};
    const auto object_id = objectIdInScene(target, "main");
    const auto context = projectionContext(EditorProjectionMode::Committed);
    ECSCoreTemplatePublic ecs;
    EditorRuntimeBindingState bindings{
        .codec_generation = context.layoutGeneration(),
        .behavior_generation = context.behaviorGeneration(),
    };
    const EditorProjectionModules projection_modules{
        .ecs = &ecs,
        .component_info = &GET_MODULE(ComponentInfoManager),
    };
    const auto materialize = [&](std::string type) {
        auto operation = addOperation(object_id, "behavior");
        operation.at("component").at("type") = std::move(type);
        const std::array operations{operation};
        const auto canonical = freezeEditorCanonicalOperations(
            operations, target.projectionState());
        const auto plan = requireProjectionPlan(
            EditorProjectionPlanningRequest{
                .base_revision = target.projectionState().revision(),
                .operations = canonical,
            },
            target.projectionState(), context);
        REQUIRE(plan.routes.size() == 1);
        REQUIRE(plan.routes.front().kind ==
                EditorProjectionRouteKind::BehaviorBatch);
        return materializeEditorProjection(
            plan, projection_modules, target, bindings, bindings.epoch,
            context.layoutGeneration(), context.behaviorGeneration());
    };

    const auto unknown = materialize("wp375a_unknown_active");
    REQUIRE(unknown.status == EditorProjectionStatus::Rejected);
    REQUIRE(unknown.error.has_value());
    REQUIRE(unknown.error->code ==
            EditorProjectionErrorCode::BehaviorTypeUnknown);
    REQUIRE(unknown.error->data.at("type") ==
            "wp375a_unknown_active");

    const auto known = materialize("wp375_matrix_behavior");
    REQUIRE(known.status == EditorProjectionStatus::Rejected);
    REQUIRE(known.error.has_value());
    REQUIRE(known.error->code ==
            EditorProjectionErrorCode::RuntimeBindingMissing);
    REQUIRE(known.error->data.at("object_id") == object_id.value);
    REQUIRE(known.error->data.at("scene_id") == "main");
    REQUIRE(target.projectionState().revision() == SceneRevision{12});
}

TEST_CASE(
    "WP375a renderer set route rejects its missing named participant",
    "[WP375a][materialize][participant][named-error][mutation]") {
    FastModuleContainer modules;
    test_support::SceneProjectionTarget target{modelDocument()};
    const auto object_id = matrixObjectId(target);
    const auto context = projectionContext(EditorProjectionMode::Committed);
    const Json authored{{"name", "simplemodelview"}, {"model", "next"}};
    const std::array operations{Json{
        {"op", "set_component_value"},
        {"object_id", object_id.value},
        {"component_slot", "simplemodelview"},
        {"authored_component", authored},
    }};
    const auto canonical = freezeEditorCanonicalOperations(
        operations, target.projectionState());
    const std::array commands{makeSetComponentValueCommand(
        "main", "Root", "simplemodelview", authored)};
    const auto plan = requireProjectionPlan(
        EditorProjectionPlanningRequest{
            .base_revision = target.projectionState().revision(),
            .commands = commands,
            .operations = canonical,
        },
        target.projectionState(), context);
    ECSCoreTemplatePublic ecs;
    EditorRuntimeBindingState bindings{
        .epoch = 9,
        .codec_generation = context.layoutGeneration(),
        .behavior_generation = context.behaviorGeneration(),
    };
    const auto result = materializeEditorProjection(
        plan,
        EditorProjectionModules{
            .ecs = &ecs,
            .component_info = &GET_MODULE(ComponentInfoManager),
        },
        target, bindings, bindings.epoch, context.layoutGeneration(),
        context.behaviorGeneration());
    REQUIRE(result.status == EditorProjectionStatus::Failed);
    REQUIRE(result.error.has_value());
    REQUIRE_THAT(result.error->message,
                 ContainsSubstring(
                     "editor_projection_participant_missing@1"));
    REQUIRE_THAT(result.error->message, ContainsSubstring("renderer"));
    REQUIRE(target.projectionState().revision() == SceneRevision{13});
}

TEST_CASE("WP375 preview semantic gate exists only in the planner and covers every row",
          "[WP375][plan][preview][route-matrix]") {
    FastModuleContainer modules;
    GET_MODULE(RayLayoutRuntimeState).configure(RayLayoutSemanticMode::LayoutV2);
    test_support::SceneProjectionTarget target{matrixDocument()};
    const auto object_id = matrixObjectId(target);
    const auto context = projectionContext(EditorProjectionMode::Preview);

    REQUIRE(context.codecCatalog().size() == literal_component_routes.size());
    for (const auto &expected_route : literal_component_routes) {
        REQUIRE(context.findCodec(expected_route.component) != nullptr);
        DYNAMIC_SECTION("preview set " << expected_route.component) {
            const auto operation =
                setOperation(object_id, expected_route.component);
            if (expected_route.preview_set) {
                REQUIRE(planOne(operation, target, context).routes ==
                        std::vector{literalPolicyRoute(
                            context, expected_route, object_id)});
            } else {
                requirePreviewRejected(operation, target, context);
            }
        }
        DYNAMIC_SECTION("preview add " << expected_route.component) {
            requirePreviewRejected(
                addOperation(object_id, expected_route.component),
                                   target, context);
        }
        DYNAMIC_SECTION("preview remove " << expected_route.component) {
            requirePreviewRejected(
                removeOperation(object_id, expected_route.component),
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
    for (const auto &expected_route : literal_component_routes) {
        if (!expected_route.preview_set) continue;
        restore_routes.push_back(EditorProjectionRoute{
            .kind = expected_route.kind,
            .scene_id = "main",
            .component = std::string{expected_route.component},
            .codec_index = contextCodecIndex(
                context, expected_route.component),
        });
    }
    const auto restore = requireProjectionPlan(
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
    const auto plan = requireProjectionPlan(
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

TEST_CASE(
    "WP375a pending behavior publication has a complete inverse token and retries in process",
    "[WP375a][materialize][behavior][rollback][retry][mutation]") {
    FastModuleContainer modules;
    TestSupport::ScopedTestRegistrationOwner registration_owner;
    test_support::SceneProjectionTarget target{matrixDocument()};
    const auto object_id = matrixObjectId(target);
    const auto entity_id_token =
        internal::getComponentRegisterer().registerComponent<EntityId>(
            "wp375a_rollback_entity_id");
    ECSCoreTemplatePublic ecs;
    const std::array<ComponentId, 0> no_components{};
    const auto entity = ecs.createEntity(
        no_components, [](std::span<void *>) {});
    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    EditorRuntimeBindingState binding_state{
        .bindings = {{.authoring_object_id = object_id,
                      .entity = entity}},
        .epoch = 71,
    };
    const EditorProjectionModules projection_modules{
        .ecs = &ecs,
        .component_info = &GET_MODULE(ComponentInfoManager),
        .behavior_arena = &arena,
    };

    struct OneShotAfterPublication final : EditorProjectionFaultInjector {
        bool armed = true;
        bool fired = false;

        bool shouldFail(EditorProjectionFaultPoint point,
                        std::string_view adapter_name) noexcept override {
            if (!armed || fired ||
                point != EditorProjectionFaultPoint::AfterPublication ||
                adapter_name != "scene_pair") {
                return false;
            }
            fired = true;
            armed = false;
            return true;
        }
    } fault;

    EditorEditCoordinator edits{EditorEditRuntimeDependencies{
        .document = [&]() -> const AuthoringSceneDocument & {
            return target.projectionState().authoring();
        },
        .projection_state = [&]() -> const SceneProjectionState & {
            return target.projectionState();
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .execute = [&](const EditorEditExecutionRequest &request) {
            auto codecs = std::vector<ComponentCodec>{
                componentCodecs().begin(), componentCodecs().end()};
            const auto context = projectionContext(
                EditorProjectionMode::Committed, std::move(codecs), {},
                EditorBehaviorAvailability::DllUnavailable);
            binding_state.codec_generation = context.layoutGeneration();
            binding_state.behavior_generation =
                context.behaviorGeneration();
            const auto plan = requireProjectionPlan(
                EditorProjectionPlanningRequest{
                    .base_revision = request.base_revision,
                    .commands = request.commands,
                    .operations = request.operations,
                },
                target.projectionState(), context);
            return materializeEditorProjection(
                plan, projection_modules, target, binding_state,
                binding_state.epoch, context.layoutGeneration(),
                context.behaviorGeneration(), &fault);
        },
        .gate = [] { return EditorGateObservation{}; },
        .allocate_behavior_attachment =
            [&](std::uint64_t commit_seq, std::size_t command_index,
                std::size_t attachment_index) {
                const auto identity = arena.reserveRuntimeAttachmentIdentity(
                    commit_seq, command_index, attachment_index);
                return EditorBehaviorAttachmentIdentity{
                    .handle = identity.handle.value,
                    .attachment_seq = identity.attachment_seq,
                };
            },
    }};
    const auto actor = edits.openSession({{"display_name", "rollback"}})
                           .at("actor_id")
                           .get<std::uint64_t>();
    const Json operation{
        {"op", "add_component"},
        {"object_id", object_id.value},
        {"component",
         {{"name", "behavior"},
          {"type", "wp375a_dll_absent_behavior"},
          {"params", Json::object()}}},
    };
    const auto enqueue = [&] {
        return edits.enqueue(
            {{"actor_id", actor},
             {"base_revision", target.projectionState().revision().value},
             {"operations", Json::array({operation})}});
    };

    const auto failed_ticket = enqueue();
    REQUIRE(failed_ticket.at("status") == "accepted");
    edits.commitPending();
    const auto failed =
        edits.getResult({{"ticket", failed_ticket.at("ticket")}});
    REQUIRE(failed.at("status") == "failed");
    REQUIRE(fault.fired);
    REQUIRE(arena.snapshot().empty());
    REQUIRE(target.projectionState().revision() == SceneRevision{11});
    REQUIRE(target.document.rawJson()
                .at("scenes")
                .at("main")
                .at("objects")
                .at(0)
                .at("components")
                .size() == 1);

    const auto retry_ticket = enqueue();
    REQUIRE(retry_ticket.at("status") == "accepted");
    edits.commitPending();
    const auto retry =
        edits.getResult({{"ticket", retry_ticket.at("ticket")}});
    INFO("WP375a retry result: " << retry.dump());
    REQUIRE(retry.at("status") == "committed");
    const auto attachments = arena.snapshot();
    REQUIRE(attachments.size() == 1);
    REQUIRE(attachments.front().pending);
    REQUIRE_FALSE(attachments.front().active);
    REQUIRE(target.projectionState().revision() == SceneRevision{12});

    arena.deactivateAll();
    ecs.clearEntities();
    internal::unregisterComponent(entity_id_token);
}

TEST_CASE("WP375 a registered foo codec reaches runtime set add and remove without adapter edits",
          "[WP375][WP378][component-codec][type-erased][materialize][factory][mutation]") {
    setupLogger();
    FastModuleContainer modules;
    TestSupport::ScopedTestRegistrationOwner registration_owner;
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

        const auto materialize = [&](EditorProjectionCommand command,
                                     Json operation) {
            const std::array commands{std::move(command)};
            const std::array operations{std::move(operation)};
            const auto canonical_operations = freezeEditorCanonicalOperations(
                operations, target.projectionState());
            const auto plan = requireProjectionPlan(
                EditorProjectionPlanningRequest{
                    .base_revision = target.projectionState().revision(),
                    .commands = commands,
                    .operations = canonical_operations,
                },
                target.projectionState(), context);
            return materializeEditorProjection(
                plan, projection_modules, target, bindings, bindings.epoch,
                context.layoutGeneration(), context.behaviorGeneration());
        };
        const auto execute = [&](EditorProjectionCommand command,
                                 Json operation) {
            const auto result =
                materialize(std::move(command), std::move(operation));
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

        const auto revision_before_invalid =
            target.projectionState().revision();
        const auto invalid = materialize(
            insertFooCommand(-1),
            Json{{"op", "add_component"},
                 {"object_id", object_id.value},
                 {"component_slot", "foo"},
                 {"component_index", 0},
                 {"component", Json{{"name", "foo"}, {"value", -1}}}});
        REQUIRE(invalid.status == EditorProjectionStatus::Rejected);
        REQUIRE(invalid.error.has_value());
        REQUIRE(invalid.error->code ==
                EditorProjectionErrorCode::CommandInvalid);
        REQUIRE(invalid.error->message ==
                "factory foo value must be non-negative");
        REQUIRE(invalid.error->data.at("resolution_error").at("code") ==
                "wp378.factory.foo_negative@1");
        REQUIRE(target.projectionState().revision() ==
                revision_before_invalid);
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
