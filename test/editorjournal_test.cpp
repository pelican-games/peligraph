#include "../src/core/communication/editorjournal.hpp"
#include "../src/core/communication/editorgate.hpp"
#include "../src/core/communication/editorruntimefactory.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/userpublic/behavior.hpp"
#include "authoringscenetestsupport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;

struct JournalBehaviorParams {
    std::string label;
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&JournalBehaviorParams::label>("label"), "journal-default"));
};

class JournalBehavior final : public Behavior {
  public:
    using Params = JournalBehaviorParams;
};

PELICAN_REGISTER_BEHAVIOR(JournalBehavior, "wp167_journal_behavior", 1);

struct JournalListRow {
    std::string label;
    static constexpr auto schema = structFields(
        componentPolicy("journal_list_row"),
        defaulted(field<&JournalListRow::label>("label"), "initial"));
};
struct JournalListParams {
    List<JournalListRow> rows;
    static constexpr auto schema = structFields(
        behaviorParamsPolicy, defaulted(field<&JournalListParams::rows>("rows")));
};
class JournalListBehavior final : public Behavior {
  public:
    using Params = JournalListParams;
};
PELICAN_REGISTER_BEHAVIOR(JournalListBehavior, "wp382b_journal_list_behavior", 1);

Json editorFixture() {
    return Json::parse(R"json({
      "schema":"pelican.scene",
      "version":1,
      "scenes":{
        "main":{
          "objects":[
            {"name":"Root","components":[
              {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
              {"name":"light","type":"directional","direction":[0,-1,0],"intensity":1,"color":[1,1,1]}
            ]},
            {"name":"Middle","parent":"Root","components":[
              {"name":"transform","pos":[1,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}
            ]},
            {"name":"Leaf","parent":"Middle","components":[
              {"name":"transform","pos":[0,2,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
              {"name":"collider","shape":"sphere","radius":0.5}
            ]},
            {"name":"Other","components":[
              {"name":"transform","pos":[5,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
              {"name":"behavior","type":"wp167_journal_behavior"}
            ]}
          ]
        }
      }
    })json");
}

class DocumentTarget final : public test_support::SceneProjectionTarget {
  public:
    DocumentTarget()
        : SceneProjectionTarget{AuthoringSceneDocument::load(
              editorFixture().dump(), SceneRevision{1})} {}
};

class PreviewDocumentTarget final : public test_support::SceneProjectionTarget {
  public:
    explicit PreviewDocumentTarget(const AuthoringSceneDocument &source)
        : SceneProjectionTarget{AuthoringSceneAuthority::stage(
              source,
              AuthoringSceneAuthority::rawView(source).documentJson(),
              SceneRevision{source.revision().value + 1})} {}
};

class RuntimeMirrorAdapter final : public EditorProjectionAdapter {
    std::string &runtime_;
    std::string old_;
    std::string next_;
    bool published_ = false;

  public:
    explicit RuntimeMirrorAdapter(std::string &runtime) : runtime_{runtime} {}

    EditorProjectionAdapterKind kind() const noexcept override {
        return EditorProjectionAdapterKind::EcsExistingValue;
    }
    std::string_view name() const noexcept override { return "fixture_runtime_mirror"; }
    EditorProjectionPublicationMode publicationMode() const noexcept override {
        return EditorProjectionPublicationMode::StagedNoexcept;
    }
    void prepare(const EditorProjectionPrepareContext &context) override {
        old_ = runtime_;
        next_ = AuthoringSceneAuthority::encodeSemantic(
            context.next_document);
        published_ = false;
    }
    void publish() noexcept override {
        runtime_.swap(next_);
        published_ = true;
    }
    void rollback() noexcept override {
        if (published_) runtime_.swap(old_);
        old_.clear();
        next_.clear();
        published_ = false;
    }
    void finish() noexcept override {
        old_.clear();
        next_.clear();
        published_ = false;
    }
};

struct Harness {
    struct RuntimeBehavior {
        AuthoringObjectId object_id{};
        std::size_t component_index = 0;
        EditorBehaviorAttachmentIdentity identity;
    };

    DocumentTarget target;
    std::string runtime = target.document.encodeSemantic();
    std::vector<std::string> lifecycle_trace;
    std::vector<std::string> phase_trace;
    std::string current_scene = "main";
    EditorGateObservation gate;
    std::unique_ptr<EditorEditCoordinator> edits;
    std::uint64_t actor = 0;
    std::string reconnect_token;
    std::size_t preview_execute_count = 0;
    std::size_t preview_boundary_count = 0;
    std::uint64_t next_behavior_handle = 100;
    std::uint64_t next_behavior_seq = 1000;
    std::size_t behavior_allocation_count = 0;
    std::vector<RuntimeBehavior> runtime_behaviors;
    std::vector<EditorBehaviorAttachmentIdentity>
        executed_behavior_identities;
    bool behavior_reload_in_progress = false;
    bool enforce_preview_planner = false;
    bool reject_preview_restore = false;
    bool reject_next_edit_execution = false;

    static EditorBehaviorAttachmentIdentity::Origin authoredOrigin(
        std::size_t component_index) {
        return EditorBehaviorAttachmentIdentity::Origin{
            .authoring_component_index = component_index,
        };
    }

    std::optional<EditorBehaviorAttachmentIdentity> resolveBehavior(
        AuthoringObjectId object_id, std::size_t component_index) const {
        const auto tracked = std::find_if(
            runtime_behaviors.begin(), runtime_behaviors.end(),
            [&](const RuntimeBehavior &behavior) {
                return behavior.object_id == object_id &&
                       behavior.component_index == component_index;
            });
        if (tracked != runtime_behaviors.end()) return tracked->identity;

        for (const auto &scene : AuthoringSceneAuthority::query(target.document)) {
            const auto object = std::find_if(
                scene.objects.begin(), scene.objects.end(),
                [&](const AuthoringObjectView &candidate) {
                    return candidate.authoring_object_id == object_id;
                });
            if (object == scene.objects.end() ||
                component_index >= object->components.size()) {
                continue;
            }
            const auto &component =
                object->components[component_index].authoredJson();
            if (component.value("name", std::string{}) != "behavior") {
                return std::nullopt;
            }
            return EditorBehaviorAttachmentIdentity{
                .handle = UINT64_C(1000000000000) +
                          object_id.value * UINT64_C(1024) + component_index,
                .attachment_seq = sceneBehaviorAttachmentSeq(
                    object->declaration_index, component_index),
                .authoring_object_id = object_id,
                .authored_component_occurrence = component_index,
                .stable_type = component.at("type").get<std::string>(),
                .origin = authoredOrigin(component_index),
            };
        }
        return std::nullopt;
    }

    void synchronizeRuntimeBehaviorPositions() {
        const auto scenes = AuthoringSceneAuthority::query(target.document);
        std::erase_if(runtime_behaviors, [&](RuntimeBehavior &behavior) {
            for (const auto &scene : scenes) {
                const auto object = std::find_if(
                    scene.objects.begin(), scene.objects.end(),
                    [&](const AuthoringObjectView &candidate) {
                        return candidate.authoring_object_id ==
                               behavior.object_id;
                    });
                if (object == scene.objects.end() ||
                    behavior.component_index >= object->components.size()) {
                    continue;
                }
                const auto &component =
                    object->components[behavior.component_index].authoredJson();
                if (component.value("name", std::string{}) != "behavior") {
                    return true;
                }
                behavior.identity.attachment_seq =
                    sceneBehaviorAttachmentSeq(
                        object->declaration_index,
                        behavior.component_index);
                behavior.identity.authoring_object_id = behavior.object_id;
                behavior.identity.authored_component_occurrence =
                    behavior.component_index;
                behavior.identity.stable_type =
                    component.at("type").get<std::string>();
                behavior.identity.origin =
                    authoredOrigin(behavior.component_index);
                return false;
            }
            return true;
        });
    }

    explicit Harness(bool install_behavior_resolver = true) {
        EditorEditRuntimeDependencies dependencies{
                .document = [this]() -> const AuthoringSceneDocument & {
                    return target.document;
                },
                .current_scene_id = [this] { return current_scene; },
                .execute = [this](const EditorEditExecutionRequest &request) {
                    phase_trace.push_back("edit:prepare_publish");
                    if (reject_next_edit_execution) {
                        reject_next_edit_execution = false;
                        return EditorProjectionResult{
                            .status = EditorProjectionStatus::Rejected,
                            .base_revision = request.base_revision,
                            .committed_revision = target.document.revision(),
                            .error = EditorProjectionError{
                                EditorProjectionErrorCode::CommandInvalid,
                                "/", "injected committed projection failure"},
                        };
                    }
                    RuntimeMirrorAdapter mirror{runtime};
                    std::vector<EditorProjectionAdapter *> adapters{&mirror};
                    EditorProjectionTransaction transaction{target,
                                                            request.base_revision};
                    auto result = transaction.commit(request.commands, adapters);
                    if (result.committed()) {
                        for (const auto &operation : request.operations) {
                            const auto &wire =
                                editorCanonicalOperationWire(operation);
                            const auto op =
                                wire.at("op").get<std::string>();
                            const auto slot = wire.value(
                                "component_slot", std::string{});
                            if (slot == "behavior") {
                                executed_behavior_identities.push_back({
                                    .handle = wire.value(
                                        "attachment_handle",
                                        std::uint64_t{}),
                                    .attachment_seq = wire.value(
                                        "attachment_seq",
                                        std::uint64_t{}),
                                });
                            }
                            if (slot == "behavior" && op == "add_component") {
                                const auto object_id = AuthoringObjectId{
                                    wire.at("object_id").get<std::uint64_t>()};
                                const auto index = wire.at("component_index")
                                                       .get<std::size_t>();
                                for (auto &behavior : runtime_behaviors) {
                                    if (behavior.object_id == object_id &&
                                        behavior.component_index >= index) {
                                        ++behavior.component_index;
                                    }
                                }
                                runtime_behaviors.push_back(RuntimeBehavior{
                                    .object_id = object_id,
                                    .component_index = index,
                                    .identity = {
                                        .handle = wire.at("attachment_handle")
                                                      .get<std::uint64_t>(),
                                        .attachment_seq = wire.at("attachment_seq")
                                                              .get<std::uint64_t>(),
                                        .authoring_object_id = object_id,
                                        .authored_component_occurrence = index,
                                        .stable_type = wire.at("component")
                                                           .at("type")
                                                           .get<std::string>(),
                                        .origin = authoredOrigin(index),
                                    },
                                });
                                lifecycle_trace.push_back("onInit");
                            } else if (slot == "behavior" &&
                                       op == "remove_component") {
                                const auto object_id = AuthoringObjectId{
                                    wire.at("object_id").get<std::uint64_t>()};
                                const auto index = wire.at("component_index")
                                                       .get<std::size_t>();
                                std::erase_if(runtime_behaviors, [&](const auto &behavior) {
                                    return behavior.object_id == object_id &&
                                           behavior.component_index == index;
                                });
                                for (auto &behavior : runtime_behaviors) {
                                    if (behavior.object_id == object_id &&
                                        behavior.component_index > index) {
                                        --behavior.component_index;
                                    }
                                }
                                lifecycle_trace.push_back("onDestroy");
                            } else if (slot == "behavior" &&
                                       op == "set_component_value") {
                                const auto label = wire.contains("value")
                                                       ? wire.at("value").get<std::string>()
                                                       : wire.at("authored_component")
                                                             .value("params", Json::object())
                                                             .value("label",
                                                                    "journal-default");
                                lifecycle_trace.push_back("event:" + label);
                                lifecycle_trace.push_back("update:" + label);
                            } else if (op == "spawn" || op == "restore_objects") {
                                lifecycle_trace.push_back("onInit");
                            } else if (op == "destroy" ||
                                       op == "remove_objects") {
                                lifecycle_trace.push_back("onDestroy");
                            } else {
                                lifecycle_trace.push_back(op);
                            }
                        }
                        synchronizeRuntimeBehaviorPositions();
                    }
                    return result;
                },
                .execute_preview =
                    [this](const EditorPreviewExecutionRequest &request) {
                        ++preview_execute_count;
                        phase_trace.push_back(request.restore_committed
                                                  ? "preview:restore"
                                                  : "preview:prepare_publish");
                        if (request.restore_committed &&
                            reject_preview_restore) {
                            return EditorProjectionResult{
                                .status = EditorProjectionStatus::Rejected,
                                .base_revision = request.base_revision,
                                .committed_revision =
                                    target.document.revision(),
                                .error = EditorProjectionError{
                                    EditorProjectionErrorCode::CommandInvalid,
                                    "/", "injected preview restore failure"},
                            };
                        }
                        if (enforce_preview_planner) {
                            try {
                                const auto context =
                                    makeEditorProjectionContext(
                                        current_scene,
                                        EditorProjectionMode::Preview);
                                auto plan_result = planEditorProjection(
                                    EditorProjectionPlanningRequest{
                                        .base_revision =
                                            request.base_revision,
                                        .commands = request.commands,
                                        .operations = request.operations,
                                        .restore_committed =
                                            request.restore_committed,
                                    },
                                    target.projectionState(), context);
                                // WP378 turned the planner's recoverable
                                // rejections into a value.  Mirror the
                                // production boundary: consume the failure
                                // here, and keep the catch for the paths that
                                // still throw.
                                if (!plan_result) {
                                    auto failure =
                                        std::move(plan_result).error();
                                    std::string message;
                                    if (const auto *recoverable = std::get_if<
                                            EditorProjectionRecoverable>(
                                            &failure)) {
                                        message = recoverable->name + ": " +
                                                  recoverable->message;
                                    } else {
                                        message =
                                            std::get<UserDataViolation>(
                                                failure)
                                                .leaf_message;
                                    }
                                    return EditorProjectionResult{
                                        .status =
                                            EditorProjectionStatus::Rejected,
                                        .base_revision = request.base_revision,
                                        .committed_revision =
                                            target.document.revision(),
                                        .error = EditorProjectionError{
                                            EditorProjectionErrorCode::
                                                CommandInvalid,
                                            "/", message},
                                    };
                                }
                            } catch (const EditorProjectionPlanError &error) {
                                return EditorProjectionResult{
                                    .status =
                                        EditorProjectionStatus::Rejected,
                                    .base_revision = request.base_revision,
                                    .committed_revision =
                                        target.document.revision(),
                                    .error = EditorProjectionError{
                                        EditorProjectionErrorCode::CommandInvalid,
                                        "/", error.what()},
                                };
                            }
                        }
                        PreviewDocumentTarget preview{target.document};
                        RuntimeMirrorAdapter mirror{runtime};
                        std::vector<EditorProjectionAdapter *> adapters{&mirror};
                        EditorProjectionTransaction transaction{
                            preview, preview.document.revision()};
                        return transaction.commit(request.commands, adapters);
                    },
                .preview_boundary = [this] {
                    ++preview_boundary_count;
                    phase_trace.push_back("preview:temporal_reset");
                },
                .gate = [this] {
                    return internal::applyBehaviorEditConcurrencyGate(
                        gate, false, behavior_reload_in_progress, false);
                },
                .allocate_behavior_attachment =
                    [this](std::uint64_t, std::size_t, std::size_t) {
                        ++behavior_allocation_count;
                        return EditorBehaviorAttachmentIdentity{
                            .handle = next_behavior_handle++,
                            .attachment_seq = next_behavior_seq++,
                        };
                    },
                .resolve_behavior_attachment =
                    [this](AuthoringObjectId object_id,
                           std::size_t attachment_index)
                    -> std::optional<EditorBehaviorAttachmentIdentity> {
                        return resolveBehavior(object_id, attachment_index);
                    },
            };
        if (!install_behavior_resolver) {
            dependencies.resolve_behavior_attachment = {};
        }
        edits = std::make_unique<EditorEditCoordinator>(
            std::move(dependencies));
        const auto session = edits->openSession({{"display_name", "fixture actor"}});
        actor = session.at("actor_id").get<std::uint64_t>();
        reconnect_token = session.at("reconnect_token").get<std::string>();
    }

    nlohmann::ordered_json enqueue(const Json &operation,
                                   std::optional<std::uint64_t> base = std::nullopt) {
        return edits->enqueue({{"actor_id", actor},
                               {"base_revision", base.value_or(
                                   target.document.revision().value)},
                               {"operations", Json::array({operation})},
                               {"coalesce_key", "fixture"}});
    }

    nlohmann::ordered_json commit(const Json &operation) {
        const auto accepted = enqueue(operation);
        edits->commitPending();
        return edits->getResult({{"ticket", accepted.at("ticket")}});
    }

    std::pair<std::uint64_t, std::string> bindNewActor(
        std::string display_name) {
        const auto session = edits->openSession(
            {{"display_name", std::move(display_name)}});
        return {session.at("actor_id").get<std::uint64_t>(),
                session.at("reconnect_token").get<std::string>()};
    }

    void bind(std::string_view token) {
        (void)edits->resumeSession({{"reconnect_token", token}});
    }
};

struct OperationCase {
    const char *name;
    Json valid;
    Json invalid;
    const char *invalid_code;
};

std::vector<OperationCase> operationCases() {
    return {
        {"set_component_value",
         {{"op", "set_component_value"}, {"object_id", 1},
          {"component_slot", "light"}, {"field_path", "/intensity"},
          {"value", 2.0}},
         {{"op", "set_component_value"}, {"object_id", 1},
          {"component_slot", "light"}, {"field_path", "/missing"},
          {"value", 2.0}},
         "schema_violation"},
        {"add_component",
         {{"op", "add_component"}, {"object_id", 1},
          {"component", {{"name", "collider"}, {"shape", "box"},
                         {"half_extents", {1, 1, 1}}}}},
         {{"op", "add_component"}, {"object_id", 1},
          {"component", {{"name", "transform"}, {"pos", {0, 0, 0}},
                         {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}}},
         "duplicate_component"},
        {"remove_component",
         {{"op", "remove_component"}, {"object_id", 1},
          {"component_slot", "light"}},
         {{"op", "remove_component"}, {"object_id", 1},
          {"component_slot", "camera"}},
         "missing_component"},
        {"spawn",
         {{"op", "spawn"}, {"scene_id", "main"},
          {"object", {{"name", "Spawned"},
                      {"components", Json::array({
                          {{"name", "transform"}, {"pos", {0, 0, 0}},
                           {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}},
                          {{"name", "behavior"}, {"type", "fixture_behavior"}}
                      })}}}},
         {{"op", "spawn"}, {"scene_id", "main"},
          {"object", {{"name", "Root"}, {"components", Json::array()}}}},
         "name_conflict"},
        {"destroy",
         {{"op", "destroy"}, {"object_id", 3}},
         {{"op", "destroy"}, {"object_id", 999}},
         "closure_unresolvable"},
        {"reparent",
         {{"op", "reparent"}, {"object_id", 3}, {"new_parent_id", 1},
          {"preserve", "local"}},
         {{"op", "reparent"}, {"object_id", 3}, {"new_parent_id", 1},
          {"preserve", "invalid"}},
         "preserve_missing"},
    };
}

} // namespace

TEST_CASE("WP378 journal maps typed decode failure and consumes canonical success",
          "[WP378][journal][component-codec][mutation]") {
    Harness harness;
    const auto before = harness.target.document.encodeSemantic();
    const auto rejected = harness.enqueue(
        {{"op", "add_component"},
         {"object_id", 1},
         {"component",
          {{"name", "camera"},
           {"type", "perspective"},
           {"yfov", 0.5F},
           {"znear", 5000.0F},
           {"zfar", 1000.0F}}}});
    REQUIRE(rejected.at("status") == "rejected");
    REQUIRE(rejected.at("error").at("code") == "schema_violation");
    REQUIRE(rejected.at("error").at("payload").at("field_path") ==
            "/znear");
    REQUIRE(rejected.at("error").at("payload").at("detail") ==
            "camera zfar (1000) must be greater than znear (5000)");
    REQUIRE(harness.target.document.encodeSemantic() == before);

    const auto committed = harness.commit(
        {{"op", "add_component"},
         {"object_id", 1},
         {"component",
          {{"name", "camera"},
           {"type", "perspective"},
           {"yfov", 0.75F},
           {"znear", 2.0F},
           {"zfar", 20.0F}}}});
    REQUIRE(committed.at("status") == "committed");
    const auto &components = harness.target.document.rawJson()
                                 .at("scenes").at("main")
                                 .at("objects").at(0).at("components");
    REQUIRE(components.back().at("name") == "camera");
    REQUIRE(components.back().at("yfov") == 0.75F);
    REQUIRE(components.back().at("znear") == 2.0F);
    REQUIRE(components.back().at("zfar") == 20.0F);
}

TEST_CASE("WP157 six edit rows enforce acceptance and execution preconditions",
          "[editor][journal][wp157][matrix]") {
    for (const auto &test : operationCases()) {
        DYNAMIC_SECTION(test.name << " positive") {
            Harness harness;
            const auto accepted = harness.enqueue(test.valid);
            REQUIRE(accepted.at("status") == "accepted");
            REQUIRE(harness.edits->pendingTicketIds().size() == 1);
            harness.edits->commitPending();
            const auto result = harness.edits->getResult(
                {{"ticket", accepted.at("ticket")}});
            REQUIRE(result.at("status") == "committed");
            REQUIRE(harness.target.document.revision().value == 2);
            REQUIRE(harness.runtime == harness.target.document.encodeSemantic());
            REQUIRE(harness.edits->journal().size() == 1);
        }

        DYNAMIC_SECTION(test.name << " acceptance rejection") {
            Harness harness;
            const auto rejected = harness.enqueue(test.invalid);
            REQUIRE(rejected.at("status") == "rejected");
            REQUIRE(rejected.at("error").at("code") == test.invalid_code);
            REQUIRE(harness.target.document.revision().value == 1);
            REQUIRE(harness.edits->journal().empty());
        }

        DYNAMIC_SECTION(test.name << " execution gate rejection") {
            Harness harness;
            const auto before = harness.target.document.encodeSemantic();
            const auto accepted = harness.enqueue(test.valid);
            REQUIRE(accepted.at("status") == "accepted");
            harness.gate.reasons = editorGateReasonBit(EditorGateReason::replay);
            harness.edits->commitPending();
            const auto result = harness.edits->getResult(
                {{"ticket", accepted.at("ticket")}});
            REQUIRE(result.at("status") == "failed");
            REQUIRE(result.at("error").at("code") == "gate_closed");
            REQUIRE(result.at("error").at("payload").at("method") == "edit");
            REQUIRE(result.at("error").at("payload").at("reason") == "replay");
            REQUIRE(harness.target.document.encodeSemantic() == before);
            REQUIRE(harness.runtime == before);
            REQUIRE(harness.edits->journal().empty());
        }
    }
}

TEST_CASE("WP157 session ActorId is issued and connection bound",
          "[editor][journal][wp157][actor]") {
    Harness harness;
    const auto first_actor = harness.actor;
    const auto first_token = harness.reconnect_token;
    const auto second = harness.edits->openSession({{"display_name", "second"}});
    REQUIRE(second.at("actor_id").get<std::uint64_t>() != first_actor);
    REQUIRE_THROWS_AS(harness.edits->enqueue(
                          {{"actor_id", first_actor},
                           {"base_revision", 1},
                           {"operations", Json::array({operationCases().front().valid})}}),
                      std::invalid_argument);
    const auto resumed = harness.edits->resumeSession(
        {{"reconnect_token", first_token}});
    REQUIRE(resumed.at("actor_id") == first_actor);
    const auto accepted = harness.enqueue(operationCases().front().valid);
    REQUIRE(accepted.at("actor_id") == first_actor);
}

TEST_CASE("WP157 global CAS rejects an unrelated-object stale edit",
          "[editor][journal][wp157][cas]") {
    Harness harness;
    const auto first = harness.enqueue(operationCases().front().valid, 1);
    const auto second = harness.enqueue(
        {{"op", "set_component_value"}, {"object_id", 4},
         {"component_slot", "transform"}, {"field_path", "/pos"},
         {"value", {7, 0, 0}}},
        1);
    REQUIRE(first.at("status") == "accepted");
    REQUIRE(second.at("status") == "accepted");
    harness.edits->commitPending();
    REQUIRE(harness.edits->getResult({{"ticket", first.at("ticket")}})
                .at("status") == "committed");
    const auto stale = harness.edits->getResult({{"ticket", second.at("ticket")}});
    REQUIRE(stale.at("status") == "rejected");
    REQUIRE(stale.at("error").at("code") == "stale_revision");
    REQUIRE(stale.at("error").at("payload").at("current_revision") == 2);
    REQUIRE(harness.edits->journal().size() == 1);
}

TEST_CASE("WP157 JOURNAL0 is complete and mechanical replay is three-way equivalent",
          "[editor][journal][wp157][roundtrip]") {
    for (const auto &test : operationCases()) {
        DYNAMIC_SECTION(test.name) {
            Harness harness;
            const auto initial_document =
                harness.target.document.encodeSemantic();
            const auto initial_runtime = harness.runtime;
            const auto accepted = harness.enqueue(test.valid);
            harness.edits->commitPending();
            const auto committed = harness.edits->getResult(
                {{"ticket", accepted.at("ticket")}});
            REQUIRE(committed.at("status") == "committed");
            const auto forward_document =
                harness.target.document.encodeSemantic();
            const auto forward_runtime = harness.runtime;
            REQUIRE(forward_document != initial_document);

            REQUIRE(harness.edits->journal().size() == 1);
            const auto &record = harness.edits->journal().front();
            const auto json = editorJournalJson(record);
            for (const auto *field : {
                     "transaction_id", "actor_id", "base_revision",
                     "committed_revision", "ordered_forward",
                     "ordered_inverse", "affected_authoring_ids",
                     "coalesce_key", "status", "stable_targets", "read_set",
                     "write_set", "structural_domain",
                     "forward_postconditions", "commands"}) {
                REQUIRE(json.contains(field));
            }
            REQUIRE(json.at("actor_display_name") == "fixture actor");
            REQUIRE_FALSE(
                json.at("commands").front().at("last_writer").empty());
            if (std::string_view{test.name} == "spawn") {
                REQUIRE(json.at("ordered_forward").front().contains("closure"));
            }

            const auto transaction_id =
                committed.at("transaction_id").get<std::string>();
            const auto inverse = harness.edits->executeJournalForVerification(
                transaction_id, true);
            REQUIRE(inverse.committed());
            REQUIRE(harness.target.document.encodeSemantic() ==
                    initial_document);
            REQUIRE(harness.runtime == initial_runtime);

            const auto replay = harness.edits->executeJournalForVerification(
                transaction_id, false);
            REQUIRE(replay.committed());
            REQUIRE(harness.target.document.encodeSemantic() ==
                    forward_document);
            REQUIRE(harness.runtime == forward_runtime);
            REQUIRE(harness.lifecycle_trace.size() == 3);
            REQUIRE(harness.lifecycle_trace[0] ==
                    harness.lifecycle_trace[2]);
            if (std::string_view{test.name} == "spawn") {
                REQUIRE(harness.lifecycle_trace ==
                        std::vector<std::string>{"onInit", "onDestroy",
                                                 "onInit"});
            } else if (std::string_view{test.name} == "destroy") {
                REQUIRE(harness.lifecycle_trace ==
                        std::vector<std::string>{"onDestroy", "onInit",
                                                 "onDestroy"});
            }
        }
    }
}

TEST_CASE("WP161 actor undo and redo are ordinary atomic transactions",
          "[editor][journal][wp161][undo][redo]") {
    Harness harness;
    const auto initial = harness.target.document.encodeSemantic();
    const auto edited = harness.commit(operationCases().front().valid);
    REQUIRE(edited.at("status") == "committed");
    const auto forward = harness.target.document.encodeSemantic();
    REQUIRE(forward != initial);

    const auto undo = harness.edits->enqueueUndo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value},
         {"transaction_id", edited.at("transaction_id")}});
    REQUIRE(undo.at("status") == "accepted");
    harness.edits->commitPending();
    const auto undone = harness.edits->getResult({{"ticket", undo.at("ticket")}});
    REQUIRE(undone.at("status") == "committed");
    REQUIRE(undone.at("operation") == "undo");
    REQUIRE(harness.target.document.encodeSemantic() == initial);
    REQUIRE(harness.runtime == initial);
    REQUIRE(harness.target.document.revision().value == 3);

    const auto redo = harness.edits->enqueueRedo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value},
         {"transaction_id", undone.at("transaction_id")}});
    REQUIRE(redo.at("status") == "accepted");
    harness.edits->commitPending();
    const auto redone = harness.edits->getResult({{"ticket", redo.at("ticket")}});
    REQUIRE(redone.at("status") == "committed");
    REQUIRE(redone.at("operation") == "redo");
    REQUIRE(harness.target.document.encodeSemantic() == forward);
    REQUIRE(harness.runtime == forward);
    REQUIRE(harness.target.document.revision().value == 4);
    REQUIRE(harness.edits->journal().size() == 3);
    REQUIRE(harness.edits->journal()[1].operation_kind == "undo");
    REQUIRE(harness.edits->journal()[2].operation_kind == "redo");
}

TEST_CASE("WP161 writer history supports multi-level undo and redo on one path",
          "[editor][journal][wp161][undo][redo][history]") {
    Harness harness;
    const auto initial = harness.target.document.encodeSemantic();
    const auto first = harness.commit(
        {{"op", "set_component_value"},
         {"object_id", 1},
         {"component_slot", "transform"},
         {"field_path", "/pos"},
         {"value", {1, 0, 0}}});
    REQUIRE(first.at("status") == "committed");
    const auto after_first = harness.target.document.encodeSemantic();
    const auto second = harness.commit(
        {{"op", "set_component_value"},
         {"object_id", 1},
         {"component_slot", "transform"},
         {"field_path", "/pos"},
         {"value", {2, 0, 0}}});
    REQUIRE(second.at("status") == "committed");
    const auto after_second = harness.target.document.encodeSemantic();

    const auto revert = [&] {
        const auto accepted = harness.edits->enqueueUndo(
            {{"actor_id", harness.actor},
             {"base_revision",
              harness.target.document.revision().value}});
        REQUIRE(accepted.at("status") == "accepted");
        harness.edits->commitPending();
        const auto result = harness.edits->getResult(
            {{"ticket", accepted.at("ticket")}});
        REQUIRE(result.at("status") == "committed");
    };
    revert();
    REQUIRE(harness.target.document.encodeSemantic() == after_first);
    revert();
    REQUIRE(harness.target.document.encodeSemantic() == initial);

    const auto replay = [&] {
        const auto accepted = harness.edits->enqueueRedo(
            {{"actor_id", harness.actor},
             {"base_revision",
              harness.target.document.revision().value}});
        REQUIRE(accepted.at("status") == "accepted");
        harness.edits->commitPending();
        const auto result = harness.edits->getResult(
            {{"ticket", accepted.at("ticket")}});
        REQUIRE(result.at("status") == "committed");
    };
    replay();
    REQUIRE(harness.target.document.encodeSemantic() == after_first);
    replay();
    REQUIRE(harness.target.document.encodeSemantic() == after_second);
}

TEST_CASE("WP161 two-actor structural overlap makes the whole undo a no-op",
          "[editor][journal][wp161][undo][conflict][two-actor]") {
    struct ConflictCase {
        const char *name;
        Json actor_a;
        std::function<Json(const Harness &)> actor_b;
    };
    const std::vector<ConflictCase> cases{
        {"spawn-edit-undo",
         {{"op", "spawn"}, {"scene_id", "main"},
          {"object", {{"name", "Spawned"},
                      {"components", Json::array({
                          {{"name", "transform"}, {"pos", {0, 0, 0}},
                           {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}
                      })}}}},
         [](const Harness &harness) {
             const auto object = harness.edits->journal().front()
                                     .affected_authoring_ids.front().value;
             return Json{{"op", "set_component_value"}, {"object_id", object},
                         {"component_slot", "transform"}, {"field_path", "/pos"},
                         {"value", {9, 0, 0}}};
         }},
        {"reparent-local-edit-undo",
         {{"op", "reparent"}, {"object_id", 3}, {"new_parent_id", 1},
          {"preserve", "local"}},
         [](const Harness &) {
             return Json{{"op", "set_component_value"}, {"object_id", 3},
                         {"component_slot", "transform"}, {"field_path", "/pos"},
                         {"value", {0, 8, 0}}};
         }},
        {"destroy-index-insert-undo",
         {{"op", "destroy"}, {"object_id", 3}},
         [](const Harness &) {
             return Json{{"op", "spawn"}, {"scene_id", "main"},
                         {"declaration_index", 2},
                         {"object", {{"name", "Leaf"},
                                     {"components", Json::array({
                                         {{"name", "transform"},
                                          {"pos", {0, 0, 0}},
                                          {"rotation", {0, 0, 0, 1}},
                                          {"scale", {1, 1, 1}}}
                                     })}}}};
         }},
        {"add-edit-remove-undo",
         {{"op", "add_component"}, {"object_id", 4},
          {"component", {{"name", "collider"}, {"shape", "box"},
                         {"half_extents", {1, 1, 1}}}}},
         [](const Harness &) {
             return Json{{"op", "set_component_value"}, {"object_id", 4},
                         {"component_slot", "collider"},
                         {"field_path", "/half_extents"},
                         {"value", {2, 2, 2}}};
         }},
        {"remove-add-undo",
         {{"op", "remove_component"}, {"object_id", 1},
          {"component_slot", "light"}},
         [](const Harness &) {
             return Json{{"op", "add_component"}, {"object_id", 1},
                         {"component_index", 1},
                         {"component", {{"name", "light"},
                                        {"type", "directional"},
                                        {"direction", {0, -1, 0}},
                                        {"intensity", 2},
                                        {"color", {1, 1, 1}}}}};
         }},
    };

    for (const auto &test : cases) {
        DYNAMIC_SECTION(test.name) {
            Harness harness;
            const auto actor_a = harness.actor;
            const auto actor_a_token = harness.reconnect_token;
            const auto committed_a = harness.commit(test.actor_a);
            REQUIRE(committed_a.at("status") == "committed");
            const auto [actor_b, actor_b_token] = harness.bindNewActor("actor-b");
            (void)actor_b_token;
            const auto accepted_b = harness.edits->enqueue(
                {{"actor_id", actor_b},
                 {"base_revision", harness.target.document.revision().value},
                 {"operations", Json::array({test.actor_b(harness)})}});
            REQUIRE(accepted_b.at("status") == "accepted");
            harness.edits->commitPending();
            REQUIRE(harness.edits->getResult({{"ticket", accepted_b.at("ticket")}})
                        .at("status") == "committed");
            harness.bind(actor_a_token);

            const auto before = harness.target.document.encodeSemantic();
            const auto revision = harness.target.document.revision().value;
            const auto journal_size = harness.edits->journal().size();
            const auto rejected = harness.edits->enqueueUndo(
                {{"actor_id", actor_a}, {"base_revision", revision},
                 {"transaction_id", committed_a.at("transaction_id")}});
            REQUIRE(rejected.at("status") == "rejected");
            REQUIRE(rejected.at("error").at("code") == "undo_conflict");
            REQUIRE(rejected.at("error").at("payload").contains("domain"));
            REQUIRE(rejected.at("error").at("payload").contains("owner_txn"));
            REQUIRE(rejected.at("error").at("payload").contains("revision"));
            REQUIRE(harness.target.document.encodeSemantic() == before);
            REQUIRE(harness.target.document.revision().value == revision);
            REQUIRE(harness.edits->journal().size() == journal_size);
        }
    }
}

TEST_CASE("WP161 reparent descendant cross-edit is an undo conflict",
          "[editor][journal][wp161][undo][descendant]") {
    Harness harness;
    const auto actor_a = harness.actor;
    const auto actor_a_token = harness.reconnect_token;
    const auto reparent = harness.commit(
        {{"op", "reparent"}, {"object_id", 2}, {"new_parent_id", 4},
         {"preserve", "local"}});
    const auto [actor_b, token_b] = harness.bindNewActor("actor-b");
    (void)token_b;
    const auto descendant = harness.edits->enqueue(
        {{"actor_id", actor_b},
         {"base_revision", harness.target.document.revision().value},
         {"operations", Json::array({
             {{"op", "set_component_value"}, {"object_id", 3},
              {"component_slot", "transform"}, {"field_path", "/pos"},
              {"value", {0, 12, 0}}}
         })}});
    harness.edits->commitPending();
    REQUIRE(harness.edits->getResult({{"ticket", descendant.at("ticket")}})
                .at("status") == "committed");
    harness.bind(actor_a_token);
    const auto rejected = harness.edits->enqueueUndo(
        {{"actor_id", actor_a},
         {"base_revision", harness.target.document.revision().value},
         {"transaction_id", reparent.at("transaction_id")}});
    REQUIRE(rejected.at("error").at("code") == "undo_conflict");
}

TEST_CASE("WP161 preview open update forced-abort and idempotent abort obey the lease matrix",
          "[editor][journal][wp161][preview][lease]") {
    Harness harness;
    const auto document_bytes = harness.target.document.encodeSemantic();
    const Json light_preview{{"op", "set_component_value"}, {"object_id", 1},
                             {"component_slot", "light"},
                             {"field_path", "/intensity"}, {"value", 4.0}};
    const auto open = harness.edits->openPreview(
        {{"actor_id", harness.actor},
         {"operations", Json::array({light_preview})}});
    REQUIRE(open.at("status") == "accepted");
    REQUIRE(harness.target.document.encodeSemantic() == document_bytes);
    REQUIRE(harness.runtime == document_bytes);
    harness.edits->commitPending();
    const auto opened = harness.edits->getPreviewResult(
        {{"request_id", open.at("request_id")}});
    REQUIRE(opened.at("status") == "open");
    REQUIRE(opened.at("preview_epoch") == 1);
    REQUIRE(harness.target.document.encodeSemantic() == document_bytes);
    REQUIRE(harness.target.document.revision().value == 1);
    REQUIRE(harness.edits->journal().empty());
    REQUIRE(harness.runtime != document_bytes);
    REQUIRE(harness.preview_boundary_count == 1);
    REQUIRE_FALSE(harness.edits->canPreview(Json::object()).at("can_preview"));
    REQUIRE(harness.edits->canPreview(Json::object()).at("reasons").back() ==
            "preview_lease_conflict");

    auto updated_operation = light_preview;
    updated_operation["value"] = 6.0;
    const auto update = harness.edits->updatePreview(
        {{"actor_id", harness.actor}, {"ticket", open.at("ticket")},
         {"operations", Json::array({updated_operation})}});
    REQUIRE(update.at("status") == "accepted");
    harness.edits->commitPending();
    const auto updated = harness.edits->getPreviewResult(
        {{"request_id", update.at("request_id")}});
    REQUIRE(updated.at("status") == "updated");
    REQUIRE(updated.at("preview_epoch") == 2);
    REQUIRE(harness.target.document.encodeSemantic() == document_bytes);

    const auto overlap = harness.enqueue(light_preview);
    REQUIRE(overlap.at("status") == "rejected");
    REQUIRE(overlap.at("error").at("code") == "preview_lease_conflict");

    const auto non_overlap = harness.enqueue(
        {{"op", "set_component_value"}, {"object_id", 4},
         {"component_slot", "transform"}, {"field_path", "/pos"},
         {"value", {11, 0, 0}}});
    REQUIRE(non_overlap.at("status") == "accepted");
    harness.edits->commitPending();
    REQUIRE(harness.edits->getResult({{"ticket", non_overlap.at("ticket")}})
                .at("status") == "committed");
    REQUIRE(harness.edits->previewEpoch() == 3);
    REQUIRE_FALSE(harness.edits->hasOpenPreviewLease());
    REQUIRE(harness.runtime == harness.target.document.encodeSemantic());
    REQUIRE(harness.preview_boundary_count == 2);
    const auto notifications = harness.edits->takeCompletedResults();
    REQUIRE(std::any_of(notifications.begin(), notifications.end(),
                        [&](const auto &value) {
                            return value.value("event", std::string{}) ==
                                       "ticket_forced_aborted" &&
                                   value.at("ticket") == open.at("ticket") &&
                                   value.at("reason") == "base_revision_stale";
                        }));

    const auto abort = harness.edits->abortPreview(
        {{"actor_id", harness.actor}, {"ticket", open.at("ticket")}});
    harness.edits->commitPending();
    const auto aborted = harness.edits->getPreviewResult(
        {{"request_id", abort.at("request_id")}});
    REQUIRE(aborted.at("status") == "succeeded");
    REQUIRE(aborted.at("final_status") == "forced_aborted");
    REQUIRE(aborted.at("preview_epoch") == 3);
}

TEST_CASE("WP161 preview commit abort ownership capability and execution gate are stable",
          "[editor][journal][wp161][preview][matrix]") {
    const Json preview_operation{
        {"op", "set_component_value"}, {"object_id", 1},
        {"component_slot", "light"}, {"field_path", "/intensity"},
        {"value", 3.0}};

    SECTION("commit is one normal transaction") {
        Harness harness;
        const auto open = harness.edits->openPreview(
            {{"actor_id", harness.actor},
             {"operations", Json::array({preview_operation})}});
        harness.edits->commitPending();
        const auto commit = harness.edits->commitPreview(
            {{"actor_id", harness.actor}, {"ticket", open.at("ticket")}});
        REQUIRE(commit.at("status") == "accepted");
        harness.edits->commitPending();
        const auto committed = harness.edits->getPreviewResult(
            {{"request_id", commit.at("request_id")}});
        REQUIRE(committed.at("status") == "committed");
        REQUIRE(committed.at("committed_revision") == 2);
        REQUIRE(committed.at("preview_epoch") == 2);
        REQUIRE(harness.edits->journal().size() == 1);
        REQUIRE(harness.runtime == harness.target.document.encodeSemantic());
        REQUIRE_FALSE(harness.edits->hasOpenPreviewLease());
    }

    SECTION("abort restores committed runtime without a durable trace") {
        Harness harness;
        const auto bytes = harness.target.document.encodeSemantic();
        const auto open = harness.edits->openPreview(
            {{"actor_id", harness.actor},
             {"operations", Json::array({preview_operation})}});
        harness.edits->commitPending();
        const auto abort = harness.edits->abortPreview(
            {{"actor_id", harness.actor}, {"ticket", open.at("ticket")}});
        harness.edits->commitPending();
        const auto result = harness.edits->getPreviewResult(
            {{"request_id", abort.at("request_id")}});
        REQUIRE(result.at("final_status") == "aborted");
        REQUIRE(harness.runtime == bytes);
        REQUIRE(harness.target.document.encodeSemantic() == bytes);
        REQUIRE(harness.target.document.revision().value == 1);
        REQUIRE(harness.edits->journal().empty());
        REQUIRE(harness.preview_boundary_count == 2);
    }

    SECTION("stale ticket commit rejects and immediately forced-aborts") {
        Harness harness;
        const auto initial_edit = harness.commit(operationCases().front().valid);
        const Json transform_preview{
            {"op", "set_component_value"}, {"object_id", 4},
            {"component_slot", "transform"}, {"field_path", "/pos"},
            {"value", {13, 0, 0}}};
        const auto open = harness.edits->openPreview(
            {{"actor_id", harness.actor},
             {"operations", Json::array({transform_preview})}});
        harness.edits->commitPending();
        REQUIRE(harness.edits->hasOpenPreviewLease());
        REQUIRE(harness.edits->executeJournalForVerification(
                    initial_edit.at("transaction_id").get<std::string>(), true)
                    .committed());
        const auto stale = harness.edits->commitPreview(
            {{"actor_id", harness.actor}, {"ticket", open.at("ticket")}});
        REQUIRE(stale.at("status") == "rejected");
        REQUIRE(stale.at("error").at("code") == "stale_revision");
        REQUIRE_FALSE(harness.edits->hasOpenPreviewLease());
        REQUIRE(harness.edits->previewEpoch() == 2);
        REQUIRE(harness.runtime == harness.target.document.encodeSemantic());
    }

    SECTION("only the lease owner may update or abort") {
        Harness harness;
        const auto owner_token = harness.reconnect_token;
        const auto open = harness.edits->openPreview(
            {{"actor_id", harness.actor},
             {"operations", Json::array({preview_operation})}});
        harness.edits->commitPending();
        const auto [other, other_token] = harness.bindNewActor("other");
        (void)other_token;
        const auto update = harness.edits->updatePreview(
            {{"actor_id", other}, {"ticket", open.at("ticket")},
             {"operations", Json::array({preview_operation})}});
        REQUIRE(update.at("error").at("code") == "not_lease_owner");
        const auto abort = harness.edits->abortPreview(
            {{"actor_id", other}, {"ticket", open.at("ticket")}});
        REQUIRE(abort.at("error").at("code") == "not_lease_owner");
        harness.bind(owner_token);
        REQUIRE(harness.edits->forceAbortPreview("fixture"));
    }

    SECTION("preview component semantics are delegated to the executor") {
        Harness harness;
        const auto opened = harness.edits->openPreview(
            {{"actor_id", harness.actor},
             {"operations", Json::array({
                 {{"op", "set_component_value"}, {"object_id", 3},
                  {"component_slot", "collider"},
                  {"field_path", "/radius"}, {"value", 2.0}}
             })}});
        REQUIRE(opened.at("status") == "accepted");
        harness.edits->commitPending();
        const auto result = harness.edits->getPreviewResult(
            {{"request_id", opened.at("request_id")}});
        REQUIRE(result.at("status") == "open");
        REQUIRE(harness.preview_execute_count == 1);
    }

    SECTION("gate closes between acceptance and execution") {
        Harness harness;
        const auto bytes = harness.target.document.encodeSemantic();
        const auto open = harness.edits->openPreview(
            {{"actor_id", harness.actor},
             {"operations", Json::array({preview_operation})}});
        harness.gate.reasons =
            editorGateReasonBit(EditorGateReason::reload_scene_transition);
        ++harness.gate.transition_epoch;
        harness.edits->commitPending();
        const auto failed = harness.edits->getPreviewResult(
            {{"request_id", open.at("request_id")}});
        REQUIRE(failed.at("status") == "failed");
        REQUIRE(failed.at("error").at("code") == "gate_closed");
        REQUIRE(failed.at("error").at("payload").at("method") ==
                "open_preview");
        REQUIRE(harness.preview_execute_count == 0);
        REQUIRE(harness.runtime == bytes);
        REQUIRE(harness.target.document.encodeSemantic() == bytes);
    }

    SECTION("scene transition wins between acceptance and execution") {
        Harness harness;
        const auto bytes = harness.target.document.encodeSemantic();
        const auto open = harness.edits->openPreview(
            {{"actor_id", harness.actor},
             {"operations", Json::array({preview_operation})}});
        harness.current_scene = "other";
        ++harness.gate.transition_epoch;
        harness.edits->commitPending();
        const auto failed = harness.edits->getPreviewResult(
            {{"request_id", open.at("request_id")}});
        REQUIRE(failed.at("status") == "failed");
        REQUIRE(failed.at("error").at("code") == "gate_closed");
        REQUIRE(failed.at("error").at("payload").at("reason") ==
                "reload_scene_transition");
        REQUIRE(harness.preview_execute_count == 0);
        REQUIRE(harness.runtime == bytes);
    }

    SECTION("callback or reload gate forced-aborts before observers resume") {
        Harness harness;
        const auto open = harness.edits->openPreview(
            {{"actor_id", harness.actor},
             {"operations", Json::array({preview_operation})}});
        harness.edits->commitPending();
        REQUIRE(harness.edits->hasOpenPreviewLease());
        harness.phase_trace.push_back("reload:published");
        harness.gate.reasons =
            editorGateReasonBit(EditorGateReason::reload_scene_transition);
        ++harness.gate.transition_epoch;
        harness.edits->commitPending();
        harness.phase_trace.push_back("observer:resume");
        REQUIRE_FALSE(harness.edits->hasOpenPreviewLease());
        const auto restore = std::find(harness.phase_trace.begin(),
                                       harness.phase_trace.end(),
                                       "preview:restore");
        const auto observer = std::find(harness.phase_trace.begin(),
                                        harness.phase_trace.end(),
                                        "observer:resume");
        REQUIRE(restore != harness.phase_trace.end());
        REQUIRE(restore < observer);
        const auto notifications = harness.edits->takeCompletedResults();
        REQUIRE(std::any_of(notifications.begin(), notifications.end(),
                            [&](const auto &value) {
                                return value.value("event", std::string{}) ==
                                           "ticket_forced_aborted" &&
                                       value.at("ticket") == open.at("ticket");
                            }));
    }
}

TEST_CASE(
    "WP375c inactive lifecycle preview is rejected before document-only routing while active set still aborts",
    "[WP375c][editor][journal][preview][inactive-scene][abort][regression][mutation]") {
    Harness harness;
    harness.enforce_preview_planner = true;
    harness.current_scene = "secondary";

    const auto inactive = harness.edits->openPreview(
        {{"actor_id", harness.actor},
         {"operations",
          Json::array({{{"op", "reparent"},
                        {"object_id", 3},
                        {"new_parent_id", 1},
                        {"preserve", "local"}}})}});
    REQUIRE(inactive.at("status") == "accepted");
    harness.edits->commitPending();
    const auto inactive_result = harness.edits->getPreviewResult(
        {{"request_id", inactive.at("request_id")}});
    REQUIRE(inactive_result.at("status") == "failed");
    REQUIRE(inactive_result.at("error").at("code") ==
            "editor_preview_unsupported@1");
    REQUIRE_FALSE(harness.edits->hasOpenPreviewLease());

    harness.current_scene = "main";
    const Json active_set{{"op", "set_component_value"},
                          {"object_id", 1},
                          {"component_slot", "light"},
                          {"field_path", "/intensity"},
                          {"value", 3.75}};
    const auto active = harness.edits->openPreview(
        {{"actor_id", harness.actor},
         {"operations", Json::array({active_set})}});
    harness.edits->commitPending();
    REQUIRE(harness.edits
                ->getPreviewResult(
                    {{"request_id", active.at("request_id")}})
                .at("status") == "open");
    const auto aborted = harness.edits->abortPreview(
        {{"actor_id", harness.actor}, {"ticket", active.at("ticket")}});
    harness.edits->commitPending();
    const auto abort_result = harness.edits->getPreviewResult(
        {{"request_id", aborted.at("request_id")}});
    REQUIRE(abort_result.at("status") == "succeeded");
    REQUIRE(abort_result.at("final_status") == "aborted");
    REQUIRE_FALSE(harness.edits->hasOpenPreviewLease());
}

TEST_CASE(
    "WP375c forced abort propagates a named restore failure and permits an explicit retry",
    "[WP375c][editor][journal][preview][forced-abort][error][mutation]") {
    Harness harness;
    const Json preview_operation{
        {"op", "set_component_value"}, {"object_id", 1},
        {"component_slot", "light"}, {"field_path", "/intensity"},
        {"value", 4.0}};
    const auto opened = harness.edits->openPreview(
        {{"actor_id", harness.actor},
         {"operations", Json::array({preview_operation})}});
    harness.edits->commitPending();
    REQUIRE(harness.edits->hasOpenPreviewLease());

    harness.reject_preview_restore = true;
    try {
        (void)harness.edits->forceAbortPreview("reload");
        FAIL("forced abort unexpectedly hid the restore failure");
    } catch (const EditorPreviewAbortError &error) {
        REQUIRE_THAT(error.what(), Catch::Matchers::ContainsSubstring(
                                      "editor_preview_forced_abort_failed@1"));
        REQUIRE_THAT(error.what(), Catch::Matchers::ContainsSubstring(
                                      "injected preview restore failure"));
    }
    REQUIRE(harness.edits->hasOpenPreviewLease());

    harness.reject_preview_restore = false;
    REQUIRE(harness.edits->forceAbortPreview("retry"));
    REQUIRE_FALSE(harness.edits->hasOpenPreviewLease());
}

TEST_CASE(
    "WP375c a failed forced abort never rewrites an already committed edit",
    "[WP375c][editor][journal][preview][forced-abort][commit][mutation]") {
    Harness harness;
    const auto opened = harness.edits->openPreview(
        {{"actor_id", harness.actor},
         {"operations",
          Json::array({{{"op", "set_component_value"},
                        {"object_id", 1},
                        {"component_slot", "light"},
                        {"field_path", "/intensity"},
                        {"value", 4.0}}})}});
    REQUIRE(opened.at("status") == "accepted");
    harness.edits->commitPending();
    REQUIRE(harness.edits->hasOpenPreviewLease());
    (void)harness.edits->takeCompletedResults();

    harness.reject_preview_restore = true;
    const auto edit = harness.enqueue(
        {{"op", "set_component_value"}, {"object_id", 4},
         {"component_slot", "transform"}, {"field_path", "/pos"},
         {"value", {11, 0, 0}}});
    REQUIRE(edit.at("status") == "accepted");
    harness.edits->commitPending();

    const auto result =
        harness.edits->getResult({{"ticket", edit.at("ticket")}});
    REQUIRE(result.at("status") == "committed");
    REQUIRE_THAT(result.at("preview_abort_error").get<std::string>(),
                 Catch::Matchers::ContainsSubstring(
                     "editor_preview_forced_abort_failed@1"));
    REQUIRE(harness.edits->hasOpenPreviewLease());
    const auto notifications = harness.edits->takeCompletedResults();
    REQUIRE(std::count_if(notifications.begin(), notifications.end(),
                          [&](const auto &value) {
                              return value.contains("ticket") &&
                                     value.at("ticket") == edit.at("ticket");
                          }) == 1);
}

TEST_CASE(
    "WP375c failed preview commit restores runtime and releases its lease",
    "[WP375c][editor][journal][preview][commit-failure][lease][mutation]") {
    Harness harness;
    const Json preview_operation{
        {"op", "set_component_value"}, {"object_id", 1},
        {"component_slot", "light"}, {"field_path", "/intensity"},
        {"value", 4.5}};
    const auto opened = harness.edits->openPreview(
        {{"actor_id", harness.actor},
         {"operations", Json::array({preview_operation})}});
    harness.edits->commitPending();
    REQUIRE(harness.edits->hasOpenPreviewLease());

    harness.reject_next_edit_execution = true;
    const auto commit = harness.edits->commitPreview(
        {{"actor_id", harness.actor}, {"ticket", opened.at("ticket")}});
    REQUIRE(commit.at("status") == "accepted");
    harness.edits->commitPending();
    const auto failed = harness.edits->getPreviewResult(
        {{"request_id", commit.at("request_id")}});
    REQUIRE(failed.at("status") == "failed");
    REQUIRE_FALSE(harness.edits->hasOpenPreviewLease());
    REQUIRE(std::ranges::find(harness.phase_trace, "preview:restore") !=
            harness.phase_trace.end());
    REQUIRE(harness.runtime == harness.target.document.encodeSemantic());

    const auto reopened = harness.edits->openPreview(
        {{"actor_id", harness.actor},
         {"operations", Json::array({preview_operation})}});
    harness.edits->commitPending();
    REQUIRE(harness.edits
                ->getPreviewResult(
                    {{"request_id", reopened.at("request_id")}})
                .at("status") == "open");
    REQUIRE(harness.edits->forceAbortPreview("cleanup"));
}

TEST_CASE("WP161 RPC and windowed paths share one frame-boundary phase trace",
          "[editor][journal][wp161][ec3-2][phase]") {
    const Json operation{{"op", "set_component_value"}, {"object_id", 1},
                         {"component_slot", "light"},
                         {"field_path", "/intensity"}, {"value", 2.0}};
    Harness rpc;
    const auto rpc_result = rpc.enqueue(operation);
    rpc.edits->commitPending();
    REQUIRE(rpc.edits->getResult({{"ticket", rpc_result.at("ticket")}})
                .at("status") == "committed");

    Harness windowed;
    const auto windowed_result = windowed.enqueue(operation);
    // WindowedRpcHost invokes the same coordinator hook at this boundary.
    windowed.edits->commitPending();
    REQUIRE(windowed.edits->getResult(
                {{"ticket", windowed_result.at("ticket")}})
                .at("status") == "committed");
    REQUIRE(rpc.phase_trace == windowed.phase_trace);
    REQUIRE(rpc.target.document.encodeSemantic() ==
            windowed.target.document.encodeSemantic());
}

TEST_CASE("WP161 behavior attachment sequence survives spawn undo redo",
          "[editor][journal][wp161][behavior][attachment-seq]") {
    Harness harness;
    const Json spawn{{"op", "spawn"}, {"scene_id", "main"},
                     {"declaration_index", 2},
                     {"object", {{"name", "BehaviorObject"},
                                 {"components", Json::array({
                                     {{"name", "transform"}, {"pos", {0, 0, 0}},
                                      {"rotation", {0, 0, 0, 1}},
                                      {"scale", {1, 1, 1}}},
                                     {{"name", "behavior"},
                                      {"type", "fixture_behavior"}}
                                 })}}}};
    const auto forward = harness.commit(spawn);
    REQUIRE(forward.at("status") == "committed");
    const auto original_seq = sceneBehaviorAttachmentSeq(2, 1);
    const auto undo = harness.edits->enqueueUndo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value}});
    harness.edits->commitPending();
    REQUIRE(harness.edits->getResult({{"ticket", undo.at("ticket")}})
                .at("status") == "committed");
    const auto redo = harness.edits->enqueueRedo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value}});
    harness.edits->commitPending();
    REQUIRE(harness.edits->getResult({{"ticket", redo.at("ticket")}})
                .at("status") == "committed");
    const auto object = harness.target.document.query().front().objects.at(2);
    REQUIRE(object.name == "BehaviorObject");
    REQUIRE(sceneBehaviorAttachmentSeq(object.declaration_index, 1) ==
            original_seq);
    REQUIRE(sceneBehaviorAttachmentSeq(4, 1) != original_seq);
    REQUIRE(harness.lifecycle_trace ==
            std::vector<std::string>{"onInit", "onDestroy", "onInit"});
}

TEST_CASE("WP382b params insert materializes only the selected default and undo restores omission",
          "[editor][journal][wp382b][behavior][params][defaults][undo]") {
    Harness harness;
    const auto edited = harness.commit(
        {{"op", "insert_component_field"},
         {"object_id", 4},
         {"component_slot", "behavior"},
         {"attachment_index", 1},
         {"field_path", "/params/label"}});
    INFO(edited.dump());
    REQUIRE(edited.at("status") == "committed");
    auto components = harness.target.document.rawJson()
                          .at("scenes")
                          .at("main")
                          .at("objects")
                          .at(3)
                          .at("components");
    REQUIRE(components.at(1).at("params").at("label") == "journal-default");
    REQUIRE(components.at(1).at("params").size() == 1);
    const auto &record = harness.edits->journal().back();
    REQUIRE_FALSE(record.ordered_inverse.front()
                      .at("authored_component")
                      .contains("params"));

    const auto undo = harness.edits->enqueueUndo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value}});
    harness.edits->commitPending();
    REQUIRE(harness.edits->getResult({{"ticket", undo.at("ticket")}})
                .at("status") == "committed");
    components = harness.target.document.rawJson()
                     .at("scenes")
                     .at("main")
                     .at("objects")
                     .at(3)
                     .at("components");
    REQUIRE_FALSE(components.at(1).contains("params"));
    REQUIRE(harness.lifecycle_trace ==
            std::vector<std::string>{"insert_component_field",
                                     "insert_component_field"});
}

TEST_CASE("Behavior field undo distinguishes separate attachments",
          "[editor][journal][behavior][attachment-field-undo]") {
    // WP382b-fix acceptance 3: separate attachments edited by separate actors
    // stay independently undoable, while the same attachment still conflicts.
    // Both variants assert in the shared body below, so the choice is a
    // generator run: an assertionless section is rejected by
    // `-w NoAssertions`.
    const bool same_attachment = GENERATE(false, true);
    CAPTURE(same_attachment);
    Harness harness;
    REQUIRE(harness.commit({{"op", "add_component"}, {"object_id", 4},
                            {"component", {{"name", "behavior"},
                                           {"type", "wp167_journal_behavior"}}}})
                .at("status") == "committed");
    const auto actor_a = harness.actor;
    const auto token_a = harness.reconnect_token;
    const auto first = harness.commit(
        {{"op", "insert_component_field"}, {"object_id", 4},
         {"component_slot", "behavior"}, {"attachment_index", 1},
         {"field_path", "/params/label"}});
    REQUIRE(first.at("status") == "committed");
    const auto [actor_b, token_b] = harness.bindNewActor("other-field-editor");
    const auto second = harness.edits->enqueue(
        {{"actor_id", actor_b},
         {"base_revision", harness.target.document.revision().value},
         {"operations", Json::array({
             {{"op", same_attachment ? "remove_component_field" : "insert_component_field"},
              {"object_id", 4}, {"component_slot", "behavior"},
              {"attachment_index", same_attachment ? 1 : 2},
              {"field_path", "/params/label"}}})}});
    REQUIRE(second.at("status") == "accepted");
    harness.edits->commitPending();
    REQUIRE(harness.edits->getResult({{"ticket", second.at("ticket")}})
                .at("status") == "committed");
    harness.bind(token_a);
    const auto undo = harness.edits->enqueueUndo(
        {{"actor_id", actor_a},
         {"base_revision", harness.target.document.revision().value},
         {"transaction_id", first.at("transaction_id")}});
    INFO(undo.dump());
    if (same_attachment) {
        REQUIRE(undo.at("status") == "rejected");
        REQUIRE(undo.at("error").at("code") == "undo_conflict");
    } else {
        REQUIRE(undo.at("status") == "accepted");
        harness.edits->commitPending();
        REQUIRE(harness.edits->getResult({{"ticket", undo.at("ticket")}})
                    .at("status") == "committed");
        const auto components = harness.target.document.rawJson()
            .at("scenes").at("main").at("objects").at(3).at("components");
        REQUIRE_FALSE(components.at(1).contains("params"));
        REQUIRE(components.at(2).at("params").at("label") == "journal-default");
    }
}

TEST_CASE("WP382b behavior remove undo preserves the exact authored component",
          "[WP382b][editor][journal][behavior][raw][remove][undo]") {
    Harness harness;
    const auto before = harness.target.document.encodeSemantic();
    const auto removed = harness.commit(
        {{"op", "remove_component"},
         {"object_id", 4},
         {"component_slot", "behavior"},
         {"attachment_index", 1}});
    REQUIRE(removed.at("status") == "committed");

    const auto accepted = harness.edits->enqueueUndo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value},
         {"transaction_id", removed.at("transaction_id")}});
    REQUIRE(accepted.at("status") == "accepted");
    harness.edits->commitPending();
    REQUIRE(harness.edits->getResult({{"ticket", accepted.at("ticket")}})
                .at("status") == "committed");
    REQUIRE(harness.target.document.encodeSemantic() == before);
    const auto &component = harness.target.document.rawJson()
                                .at("scenes")
                                .at("main")
                                .at("objects")
                                .at(3)
                                .at("components")
                                .at(1);
    REQUIRE_FALSE(component.contains("params"));
    const auto authored_component = component;
    REQUIRE(harness.commit({{"op", "add_component"},
                            {"object_id", 4},
                            {"component", authored_component}})
                .at("status") == "committed");
    REQUIRE(harness.target.document.rawJson().at("scenes").at("main")
                .at("objects").at(3).at("components").back().dump() ==
            authored_component.dump());
}

TEST_CASE("List editing can undo and redo through attachment creation",
          "[editor][journal][behavior][list-creation-undo]") {
    // The three routes into the attachment differ only in how the component is
    // created; every assertion lives in the shared body below, so they are
    // generator runs rather than assertionless sections.
    const std::string variant =
        GENERATE(std::string{"add component"}, std::string{"spawn object"},
                 std::string{"destroy and restore object with edited List"});
    CAPTURE(variant);
    const bool spawn = variant != "add component";
    const bool destroy = variant == "destroy and restore object with edited List";
    Harness harness;
    const Json initial{{"name", "behavior"},
        {"type", "wp382b_journal_list_behavior"},
        {"params", {{"rows", {{"$list", {{"v", 1}, {"next_id", 1}}},
                             {"rows", Json::array()}}}}}};
    const auto created = harness.commit(spawn
        ? Json{{"op", "spawn"}, {"scene_id", "main"},
               {"object", {{"name", "ListObject"}, {"components", Json::array({initial})}}}}
        : Json{{"op", "add_component"}, {"object_id", 4}, {"component", initial}});
    REQUIRE(created.at("status") == "committed");
    const auto object_id = spawn ? 5 : 4;
    const auto component_index = spawn ? 0 : 2;
    const Json address{{"authoring_object_id", object_id},
        {"component", {{"type", "wp382b_journal_list_behavior"},
            {"occurrence", component_index}, {"origin", {{"kind", "authored"},
                {"authoring_component_index", component_index}}}}}, {"list_path", "/rows"}};
    const auto inserted = harness.commit({{"op", "insert_component_field"},
        {"address", address}, {"position", {{"end", true}}},
        {"value", {{"label", "retained-row"}}}});
    REQUIRE(inserted.at("status") == "committed");
    const auto replay = [&](bool redo) {
        const Json request{{"actor_id", harness.actor},
            {"base_revision", harness.target.document.revision().value}};
        const auto accepted = redo ? harness.edits->enqueueRedo(request)
                                   : harness.edits->enqueueUndo(request);
        INFO(accepted.dump());
        REQUIRE(accepted.at("status") == "accepted");
        harness.edits->commitPending();
        const auto result = harness.edits->getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("status") == "committed");
    };
    if (destroy) {
        REQUIRE(harness.commit({{"op", "destroy"}, {"object_id", object_id}})
                    .at("status") == "committed");
        replay(false);
    }
    replay(false);
    replay(false);
    replay(true);
    replay(true);
    const auto rows = harness.target.document.rawJson().at("scenes").at("main")
        .at("objects").at(spawn ? 4 : 3).at("components").at(component_index)
        .at("params").at("rows");
    REQUIRE(rows.at("rows").size() == 1);
    REQUIRE(rows.at("rows").at(0).at("$row").at("id") == 1);
    REQUIRE(rows.at("rows").at(0).at("label") == "retained-row");
    REQUIRE(rows.at("$list").at("next_id") == 2);
}

TEST_CASE("WP382b behavior attachment insertion rejects an older list undo",
          "[WP382b][editor][journal][behavior][list][two-actor]") {
    Harness harness;
    const Json rows{{"$list", {{"v", 1}, {"next_id", 2}}},
        {"rows", Json::array({{{"$row", {{"v", 1}, {"id", 1}}},
            {"label", "original"}}})}};
    const Json initial{{"name", "behavior"},
        {"type", "wp382b_journal_list_behavior"}, {"params", {{"rows", rows}}}};
    harness.commit({{"op", "add_component"}, {"object_id", 4},
        {"component", initial}});
    const auto edited = harness.commit({{"op", "set_component_value"},
        {"address", {{"authoring_object_id", 4},
            {"component", {{"type", "wp382b_journal_list_behavior"},
                {"occurrence", 2}, {"origin", {{"kind", "authored"},
                    {"authoring_component_index", 2}}}}},
            {"list_path", "/rows"}, {"row_id", 1}, {"leaf_path", "/label"}}},
        {"value", "edited"}});
    INFO(edited.dump());
    REQUIRE(edited.at("status") == "committed");
    SECTION("the unchanged attachment can undo its own row edit") {
        const auto undo = harness.edits->enqueueUndo({{"actor_id", harness.actor},
            {"base_revision", harness.target.document.revision().value},
            {"transaction_id", edited.at("transaction_id")}});
        REQUIRE(undo.at("status") == "accepted");
        harness.edits->commitPending();
        REQUIRE(harness.edits->getResult({{"ticket", undo.at("ticket")}})
                    .at("status") == "committed");
        REQUIRE(harness.target.document.rawJson().at("scenes").at("main")
            .at("objects").at(3).at("components").at(2).at("params")
            .at("rows").at("rows").at(0).at("label") == "original");
        return;
    }
    SECTION("an inserted copy cannot receive the older attachment's undo") {
    const auto actor_a_token = harness.reconnect_token;
    const auto actor_a = harness.actor;
    const Json copy = harness.target.document.rawJson().at("scenes").at("main")
        .at("objects").at(3).at("components").at(2);
    const auto [actor_b, actor_b_token] = harness.bindNewActor("actor-b");
    (void)actor_b_token;
    harness.actor = actor_b;
    harness.commit({{"op", "add_component"}, {"object_id", 4},
        {"component_index", 2}, {"component", copy}});
    harness.bind(actor_a_token);
    harness.actor = actor_a;
    const auto before = harness.target.document.encodeSemantic();
    const auto result = harness.edits->enqueueUndo({{"actor_id", harness.actor},
        {"base_revision", harness.target.document.revision().value},
        {"transaction_id", edited.at("transaction_id")}});
    INFO(result.dump());
    if (result.at("status") == "accepted") {
        harness.edits->commitPending();
        INFO(harness.edits->getResult({{"ticket", result.at("ticket")}}).dump());
        INFO(harness.target.document.encodeSemantic());
        REQUIRE(result.at("status") == "rejected");
    }
    REQUIRE(result.at("status") == "rejected");
    REQUIRE(result.at("error").at("code") == "undo_conflict");
    REQUIRE(harness.target.document.encodeSemantic() == before);
    }
}

TEST_CASE("WP167 direct behavior attachment edits keep exact handles and sequences",
          "[editor][journal][wp167][behavior][identity][undo-redo]") {
    Harness harness;
    const auto behavior = [](std::string_view label) {
        return Json{{"name", "behavior"},
                    {"type", "wp167_journal_behavior"},
                    {"params", {{"label", label}}}};
    };

    const auto first = harness.commit({{"op", "add_component"},
                                       {"object_id", 1},
                                       {"component", behavior("one")}});
    REQUIRE(first.at("status") == "committed");
    REQUIRE(harness.runtime_behaviors.size() == 1);
    const auto identity = harness.runtime_behaviors.front().identity;
    const auto &forward_record = harness.edits->journal().back();
    REQUIRE(forward_record.ordered_forward.front().at("attachment_handle") ==
            identity.handle);
    REQUIRE(forward_record.ordered_forward.front().at("attachment_seq") ==
            identity.attachment_seq);
    REQUIRE(forward_record.ordered_inverse.front().at("attachment_handle") ==
            identity.handle);
    REQUIRE(forward_record.ordered_inverse.front().at("attachment_seq") ==
            identity.attachment_seq);
    REQUIRE(forward_record.stable_targets.front().find(
                "occurrences/2") != std::string::npos);
    const auto first_stable_target = forward_record.stable_targets.front();

    const auto undo = harness.edits->enqueueUndo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value}});
    harness.edits->commitPending();
    const auto undo_result =
        harness.edits->getResult({{"ticket", undo.at("ticket")}});
    REQUIRE(undo_result.at("status") == "committed");
    REQUIRE(harness.runtime_behaviors.empty());

    const auto redo = harness.edits->enqueueRedo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value}});
    harness.edits->commitPending();
    REQUIRE(harness.edits->getResult({{"ticket", redo.at("ticket")}})
                .at("status") == "committed");
    REQUIRE(harness.runtime_behaviors.size() == 1);
    REQUIRE(harness.runtime_behaviors.front().identity.handle == identity.handle);
    REQUIRE(harness.runtime_behaviors.front().identity.attachment_seq ==
            identity.attachment_seq);
    REQUIRE(harness.lifecycle_trace ==
            std::vector<std::string>{"onInit", "onDestroy", "onInit"});

    REQUIRE(harness.commit({{"op", "add_component"},
                            {"object_id", 1},
                            {"component", behavior("two")}})
                .at("status") == "committed");
    REQUIRE(harness.runtime_behaviors.size() == 2);
    const auto second_identity = harness.runtime_behaviors.back().identity;
    REQUIRE(second_identity.handle != identity.handle);
    REQUIRE(second_identity.attachment_seq != identity.attachment_seq);
    REQUIRE(harness.edits->journal().back().stable_targets.front() !=
            first_stable_target);

    REQUIRE(harness.commit({{"op", "set_component_value"},
                            {"object_id", 1},
                            {"component_slot", "behavior"},
                            {"attachment_index", 2},
                            {"field_path", "/params/label"},
                            {"value", "one-edited"}})
                .at("status") == "committed");
    const auto &components = harness.target.document.rawJson()
                                 .at("scenes")
                                 .at("main")
                                 .at("objects")
                                 .at(0)
                                 .at("components");
    REQUIRE(components.at(2).at("params").at("label") == "one-edited");
    REQUIRE(components.at(3).at("params").at("label") == "two");
    std::cout << "WP360_BEHAVIOR_RAW_SLOT edited_component_index=2"
                 " untouched_component_index=3 attachment_seq="
              << identity.attachment_seq << '\n';
    REQUIRE(harness.lifecycle_trace[harness.lifecycle_trace.size() - 2] ==
            "event:one-edited");
    REQUIRE(harness.lifecycle_trace.back() == "update:one-edited");

    const auto semantic_before_invalid =
        harness.target.document.encodeSemantic();
    const auto lifecycle_before_invalid = harness.lifecycle_trace;
    const auto invalid = harness.enqueue(
        {{"op", "set_component_value"},
         {"object_id", 1},
         {"component_slot", "behavior"},
         {"attachment_index", 2},
         {"field_path", "/params/label"},
         {"value", 7}});
    REQUIRE(invalid.at("status") == "rejected");
    REQUIRE(invalid.at("error").at("code") == "schema_violation");
    REQUIRE(harness.target.document.encodeSemantic() == semantic_before_invalid);
    REQUIRE(harness.lifecycle_trace == lifecycle_before_invalid);

    const auto remove_second = harness.commit(
        {{"op", "remove_component"},
         {"object_id", 1},
         {"component_slot", "behavior"},
         {"attachment_index", 3}});
    REQUIRE(remove_second.at("status") == "committed");
    const auto &remove_record = harness.edits->journal().back();
    REQUIRE(remove_record.ordered_forward.front().at("attachment_handle") ==
            second_identity.handle);
    REQUIRE(remove_record.ordered_inverse.front().at("attachment_seq") ==
            second_identity.attachment_seq);
    REQUIRE(harness.runtime_behaviors.size() == 1);
    REQUIRE(harness.runtime_behaviors.front().identity.handle == identity.handle);

    const auto undo_remove = harness.edits->enqueueUndo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value}});
    harness.edits->commitPending();
    REQUIRE(harness.edits->getResult({{"ticket", undo_remove.at("ticket")}})
                .at("status") == "committed");
    REQUIRE(harness.runtime_behaviors.size() == 2);
    REQUIRE(harness.runtime_behaviors.back().identity.handle ==
            second_identity.handle);
    REQUIRE(harness.runtime_behaviors.back().identity.attachment_seq ==
            second_identity.attachment_seq);

    const auto redo_remove = harness.edits->enqueueRedo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value}});
    harness.edits->commitPending();
    REQUIRE(harness.edits->getResult({{"ticket", redo_remove.at("ticket")}})
                .at("status") == "committed");
    REQUIRE(harness.runtime_behaviors.size() == 1);
    REQUIRE(harness.runtime_behaviors.front().identity.handle == identity.handle);
    REQUIRE(std::vector<std::string>(harness.lifecycle_trace.end() - 3,
                                     harness.lifecycle_trace.end()) ==
            std::vector<std::string>{"onDestroy", "onInit", "onDestroy"});
}

TEST_CASE(
    "WP375b behavior undo re-resolves a journal handle after the runtime arena changes",
    "[WP375b][editor][journal][behavior][reload][undo][identity][mutation]") {
    Harness harness;
    const auto added = harness.commit(
        {{"op", "add_component"},
         {"object_id", 1},
         {"component",
          {{"name", "behavior"},
           {"type", "wp167_journal_behavior"},
           {"params", {{"label", "pending"}}}}}});
    REQUIRE(added.at("status") == "committed");
    REQUIRE(harness.runtime_behaviors.size() == 1);
    const auto journal_handle =
        harness.runtime_behaviors.front().identity.handle;
    const auto journal_sequence =
        harness.runtime_behaviors.front().identity.attachment_seq;
    REQUIRE(harness.edits->journal().back()
                .ordered_inverse.front()
                .at("attachment_handle") == journal_handle);

    // A production game-logic reload destroys the arena and reconstructs the
    // same authored attachment with a new runtime-only handle. Preserve the
    // stable object/index/sequence identity while changing only that handle.
    const std::uint64_t reloaded_handle = journal_handle + 10000;
    harness.runtime_behaviors.front().identity.handle = reloaded_handle;
    REQUIRE(harness.runtime_behaviors.front().identity.attachment_seq ==
            journal_sequence);

    const auto undo = harness.edits->enqueueUndo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value},
         {"transaction_id", added.at("transaction_id")}});
    REQUIRE(undo.at("status") == "accepted");
    harness.edits->commitPending();
    const auto undone =
        harness.edits->getResult({{"ticket", undo.at("ticket")}});
    REQUIRE(undone.at("status") == "committed");
    REQUIRE(harness.runtime_behaviors.empty());
    REQUIRE(harness.executed_behavior_identities.back().handle ==
            reloaded_handle);
    REQUIRE(harness.executed_behavior_identities.back().handle !=
            journal_handle);
    REQUIRE(harness.executed_behavior_identities.back().attachment_seq ==
            journal_sequence);
}

TEST_CASE(
    "WP375c behavior undo survives a preceding object destroy that changes position sequence",
    "[WP375c][editor][journal][behavior][undo][identity][mutation]") {
    Harness harness;
    const auto added = harness.commit(
        {{"op", "add_component"},
         {"object_id", 4},
         {"component",
          {{"name", "behavior"},
           {"type", "wp167_journal_behavior"},
           {"params", {{"label", "shifted"}}}}}});
    REQUIRE(added.at("status") == "committed");
    REQUIRE(harness.runtime_behaviors.size() == 1);
    const auto original_sequence =
        harness.runtime_behaviors.front().identity.attachment_seq;
    REQUIRE(original_sequence == sceneBehaviorAttachmentSeq(3, 2));

    const auto [destroy_actor, destroy_token] =
        harness.bindNewActor("destroy actor");
    (void)destroy_token;
    const auto destroyed = harness.edits->enqueue(
        {{"actor_id", destroy_actor},
         {"base_revision", harness.target.document.revision().value},
         {"operations", Json::array({
                            {{"op", "destroy"}, {"object_id", 1}}})},
         {"coalesce_key", "destroy-a"}});
    REQUIRE(destroyed.at("status") == "accepted");
    harness.edits->commitPending();
    REQUIRE(harness.edits
                ->getResult({{"ticket", destroyed.at("ticket")}})
                .at("status") == "committed");
    REQUIRE(harness.runtime_behaviors.size() == 1);
    const auto shifted_sequence =
        harness.runtime_behaviors.front().identity.attachment_seq;
    REQUIRE(shifted_sequence == sceneBehaviorAttachmentSeq(0, 2));
    REQUIRE(shifted_sequence != original_sequence);

    harness.bind(harness.reconnect_token);
    const auto undo = harness.edits->enqueueUndo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value},
         {"transaction_id", added.at("transaction_id")}});
    INFO(undo.dump());
    REQUIRE(undo.at("status") == "accepted");
    harness.edits->commitPending();
    REQUIRE(harness.edits->getResult({{"ticket", undo.at("ticket")}})
                .at("status") == "committed");
    REQUIRE(harness.runtime_behaviors.empty());
    REQUIRE(harness.executed_behavior_identities.back().attachment_seq ==
            shifted_sequence);
}

TEST_CASE(
    "WP375c zero position sequence still validates authored behavior identity",
    "[WP375c][editor][journal][behavior][identity][zero][mutation]") {
    for (const auto mismatch : {std::string{"object"},
                                std::string{"occurrence"},
                                std::string{"type"},
                                std::string{"origin"}}) {
        DYNAMIC_SECTION(mismatch) {
            Harness harness;
            const auto added = harness.commit(
                {{"op", "add_component"},
                 {"object_id", 1},
                 {"component_index", 0},
                 {"component",
                  {{"name", "behavior"},
                   {"type", "wp167_journal_behavior"},
                   {"params", {{"label", "zero"}}}}}});
            REQUIRE(added.at("status") == "committed");
            REQUIRE(harness.runtime_behaviors.size() == 1);
            REQUIRE(harness.runtime_behaviors.front().identity.attachment_seq ==
                    0);
            REQUIRE(harness.edits->journal().back()
                        .ordered_inverse.front()
                        .at("attachment_seq") == 0);

            auto &runtime_identity =
                harness.runtime_behaviors.front().identity;
            if (mismatch == "object") {
                runtime_identity.authoring_object_id = AuthoringObjectId{2};
            } else if (mismatch == "occurrence") {
                runtime_identity.authored_component_occurrence = 1;
            } else if (mismatch == "type") {
                runtime_identity.stable_type = "neighbor_behavior";
            } else {
                runtime_identity.origin.authoring_component_index = 1;
            }

            const auto undo = harness.edits->enqueueUndo(
                {{"actor_id", harness.actor},
                 {"base_revision", harness.target.document.revision().value},
                 {"transaction_id", added.at("transaction_id")}});
            REQUIRE(undo.at("status") == "accepted");
            harness.edits->commitPending();
            const auto rejected = harness.edits->getResult(
                {{"ticket", undo.at("ticket")}});
            REQUIRE(rejected.at("status") == "failed");
            REQUIRE(rejected.at("error").at("code") ==
                    "journal_integrity_error@1");
        }
    }
}

TEST_CASE(
    "WP375c behavior journal replay rejects an unconfigured identity resolver",
    "[WP375c][editor][journal][behavior][identity][resolver][mutation]") {
    Harness harness{false};
    const auto added = harness.commit(
        {{"op", "add_component"},
         {"object_id", 1},
         {"component",
          {{"name", "behavior"},
           {"type", "wp167_journal_behavior"},
           {"params", {{"label", "resolver"}}}}}});
    REQUIRE(added.at("status") == "committed");

    const auto undo = harness.edits->enqueueUndo(
        {{"actor_id", harness.actor},
         {"base_revision", harness.target.document.revision().value},
         {"transaction_id", added.at("transaction_id")}});
    REQUIRE(undo.at("status") == "accepted");
    harness.edits->commitPending();
    const auto rejected =
        harness.edits->getResult({{"ticket", undo.at("ticket")}});
    REQUIRE(rejected.at("status") == "failed");
    REQUIRE(rejected.at("error").at("code") ==
            "journal_integrity_error@1");
    REQUIRE(rejected.at("error").at("payload").at("reason") ==
            "behavior_attachment_resolver_unavailable");
}

TEST_CASE("WP167 replay and golden gates reject behavior edits before identity allocation",
          "[editor][journal][wp167][behavior][gate]") {
    for (const auto &[reason, name] :
         std::vector<std::pair<EditorGateReason, std::string>>{
             {EditorGateReason::replay, "replay"},
             {EditorGateReason::golden, "golden"}}) {
        DYNAMIC_SECTION(name) {
            Harness harness;
            harness.gate.reasons = editorGateReasonBit(reason);
            const auto rejected = harness.enqueue(
                {{"op", "add_component"},
                 {"object_id", 1},
                 {"component", {{"name", "behavior"},
                                {"type", "wp167_journal_behavior"},
                                {"params", {{"label", "blocked"}}}}}});
            REQUIRE(rejected.at("status") == "rejected");
            REQUIRE(rejected.at("error").at("code") == "gate_closed");
            REQUIRE(rejected.at("error").at("payload").at("reason") == name);
            REQUIRE(harness.behavior_allocation_count == 0);
            REQUIRE(harness.target.document.revision() == SceneRevision{1});
        }
    }
}

TEST_CASE("WP167 G2 reload overlap is a stable reject at acceptance and execution",
          "[editor][journal][wp167][behavior][g2][reload]") {
    const auto add_behavior = Json{
        {"op", "add_component"},
        {"object_id", 1},
        {"component", {{"name", "behavior"},
                       {"type", "wp167_journal_behavior"},
                       {"params", {{"label", "g2"}}}}},
    };

    SECTION("reload already active") {
        Harness harness;
        harness.behavior_reload_in_progress = true;
        const auto rejected = harness.enqueue(add_behavior);
        REQUIRE(rejected.at("status") == "rejected");
        REQUIRE(rejected.at("error").at("code") == "gate_closed");
        REQUIRE(rejected.at("error").at("payload").at("reason") ==
                "reload_scene_transition");
        REQUIRE(harness.behavior_allocation_count == 0);
        REQUIRE(harness.edits->journal().empty());
    }

    SECTION("reload starts after acceptance") {
        Harness harness;
        const auto accepted = harness.enqueue(add_behavior);
        REQUIRE(accepted.at("status") == "accepted");
        REQUIRE(harness.behavior_allocation_count == 1);
        harness.behavior_reload_in_progress = true;
        harness.edits->commitPending();
        const auto failed = harness.edits->getResult(
            {{"ticket", accepted.at("ticket")}});
        REQUIRE(failed.at("status") == "failed");
        REQUIRE(failed.at("error").at("code") == "gate_closed");
        REQUIRE(failed.at("error").at("payload").at("reason") ==
                "reload_scene_transition");
        REQUIRE(harness.target.document.revision() == SceneRevision{1});
        REQUIRE(harness.runtime_behaviors.empty());
        REQUIRE(harness.edits->journal().empty());
    }
}

TEST_CASE("WP157 edit errors use only the canonical catalog",
          "[editor][journal][wp157][catalog]") {
    for (const auto code : {
             EditorEditErrorCode::stale_revision,
             EditorEditErrorCode::gate_closed,
             EditorEditErrorCode::preview_lease_conflict,
             EditorEditErrorCode::preview_lease_busy,
             EditorEditErrorCode::not_lease_owner,
             EditorEditErrorCode::ticket_not_found,
             EditorEditErrorCode::undo_conflict,
             EditorEditErrorCode::not_editable,
             EditorEditErrorCode::schema_violation,
              EditorEditErrorCode::prefab_instance_id_collision,
              EditorEditErrorCode::behavior_type_unknown,
              EditorEditErrorCode::component_unknown,
             EditorEditErrorCode::duplicate_component,
             EditorEditErrorCode::missing_component,
             EditorEditErrorCode::name_conflict,
             EditorEditErrorCode::parent_not_found,
             EditorEditErrorCode::closure_unresolvable,
             EditorEditErrorCode::cycle_detected,
             EditorEditErrorCode::zero_scale,
             EditorEditErrorCode::non_finite_transform,
             EditorEditErrorCode::trs_unrepresentable,
             EditorEditErrorCode::preserve_missing,
             EditorEditErrorCode::method_unavailable,
         }) {
        REQUIRE(std::string{editorEditErrorCodeName(code)} != "unknown");
    }
}

TEST_CASE("Editor transition counter moves on reload outcomes and scene switches only",
          "[editor][gate][transition]") {
    using Facts = watch::ReloadTransitionFacts;
    const auto facts = [](std::uint64_t applied, std::uint64_t watcher_epoch) {
        return Facts{
            .state = "watching",
            .epoch = watcher_epoch,
            .participants = {
                {.name = "pelican.render_pipeline", .attempted = applied,
                 .applied = applied},
                {.name = "pelican.shaders"},
            },
        };
    };
    internal::EditorTransitionCounter counter;
    const auto base = facts(1, 7);
    const auto first = counter.observe("main", &base);

    // Observing the same facts again is not a transition. A window resize
    // re-lowers the render pipeline, but that only moves the participant's
    // self-description, which ReloadTransitionFacts cannot carry.
    const auto same = facts(1, 7);
    CHECK(counter.observe("main", &same) == first);

    // An applied reload, a watcher epoch change, and a scene switch are.
    const auto reloaded = facts(2, 7);
    const auto after_reload = counter.observe("main", &reloaded);
    CHECK(after_reload != first);
    const auto watcher = facts(2, 8);
    const auto after_watcher = counter.observe("main", &watcher);
    CHECK(after_watcher != after_reload);
    const auto after_scene = counter.observe("other", &watcher);
    CHECK(after_scene != after_watcher);

    // Returning to earlier facts is still a transition: the counter never
    // reuses a value, unlike the hash it replaced.
    CHECK(counter.observe("main", &base) != after_scene);
    CHECK(counter.observe("main", &base) == counter.observe("main", &base));

    internal::EditorTransitionCounter without_reload;
    const auto none = without_reload.observe("main", nullptr);
    CHECK(without_reload.observe("main", nullptr) == none);
    CHECK(without_reload.observe("main", &base) != none);
}

TEST_CASE("Editor gate epoch treats a gate that closed and reopened as a change",
          "[editor][gate]") {
    EditorGateEpoch<EditorGateObservation> gate;
    const EditorGateObservation open{.reasons = 0, .transition_epoch = 3};
    const auto accepted = gate.observe(open);
    CHECK(gate.observe(open) == accepted);

    const EditorGateObservation closed{
        .reasons = editorGateReasonBit(EditorGateReason::replay),
        .transition_epoch = 3};
    const auto while_closed = gate.observe(closed);
    CHECK(while_closed != accepted);
    // Open again with the same transition epoch: an edit accepted before the
    // gate closed must still see a different epoch.
    CHECK(gate.observe(open) != accepted);
    const auto advanced = gate.advance();
    CHECK(advanced == gate.current());
    CHECK(advanced != accepted);
}

} // namespace Pelican
