#include "../src/core/communication/editorjournal.hpp"
#include "../src/core/communication/editorruntimefactory.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/userpublic/behavior.hpp"
#include "authoringscenetestsupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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
    bool behavior_reload_in_progress = false;

    Harness() {
        edits = std::make_unique<EditorEditCoordinator>(
            EditorEditRuntimeDependencies{
                .document = [this]() -> const AuthoringSceneDocument & {
                    return target.document;
                },
                .current_scene_id = [this] { return current_scene; },
                .execute = [this](const EditorEditExecutionRequest &request) {
                    phase_trace.push_back("edit:prepare_publish");
                    RuntimeMirrorAdapter mirror{runtime};
                    std::vector<EditorProjectionAdapter *> adapters{&mirror};
                    EditorProjectionTransaction transaction{target,
                                                            request.base_revision};
                    auto result = transaction.commit(request.commands, adapters);
                    if (result.committed()) {
                        for (const auto &operation : request.operations) {
                            const auto op =
                                operation.at("op").get<std::string>();
                            const auto slot = operation.value(
                                "component_slot", std::string{});
                            if (slot == "behavior" && op == "add_component") {
                                const auto index = operation.at("component_index")
                                                       .get<std::size_t>();
                                for (auto &behavior : runtime_behaviors) {
                                    if (behavior.component_index >= index) {
                                        ++behavior.component_index;
                                    }
                                }
                                runtime_behaviors.push_back(RuntimeBehavior{
                                    .component_index = index,
                                    .identity = {
                                        .handle = operation.at("attachment_handle")
                                                      .get<std::uint64_t>(),
                                        .attachment_seq = operation.at("attachment_seq")
                                                              .get<std::uint64_t>(),
                                    },
                                });
                                lifecycle_trace.push_back("onInit");
                            } else if (slot == "behavior" &&
                                       op == "remove_component") {
                                const auto index = operation.at("component_index")
                                                       .get<std::size_t>();
                                std::erase_if(runtime_behaviors, [&](const auto &behavior) {
                                    return behavior.component_index == index;
                                });
                                for (auto &behavior : runtime_behaviors) {
                                    if (behavior.component_index > index) {
                                        --behavior.component_index;
                                    }
                                }
                                lifecycle_trace.push_back("onDestroy");
                            } else if (slot == "behavior" &&
                                       op == "set_component_value") {
                                const auto label = operation.contains("value")
                                                       ? operation.at("value").get<std::string>()
                                                       : operation.at("authored_component")
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
                    }
                    return result;
                },
                .execute_preview =
                    [this](const EditorPreviewExecutionRequest &request) {
                        ++preview_execute_count;
                        phase_trace.push_back(request.restore_committed
                                                  ? "preview:restore"
                                                  : "preview:prepare_publish");
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
                    [this](AuthoringObjectId, std::size_t attachment_index)
                    -> std::optional<EditorBehaviorAttachmentIdentity> {
                        const auto found = std::find_if(
                            runtime_behaviors.begin(), runtime_behaviors.end(),
                            [&](const auto &behavior) {
                                return behavior.component_index == attachment_index;
                            });
                        if (found == runtime_behaviors.end()) return std::nullopt;
                        return found->identity;
                    },
            });
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

    SECTION("irreversible adapter fields are unavailable") {
        Harness harness;
        const auto rejected = harness.edits->openPreview(
            {{"actor_id", harness.actor},
             {"operations", Json::array({
                 {{"op", "set_component_value"}, {"object_id", 3},
                  {"component_slot", "collider"},
                  {"field_path", "/radius"}, {"value", 2.0}}
             })}});
        REQUIRE(rejected.at("error").at("code") == "method_unavailable");
        REQUIRE(harness.preview_execute_count == 0);
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

TEST_CASE("WP167 params edit expands defaults temporarily and undo restores raw omission",
          "[editor][journal][wp167][behavior][params][defaults][undo]") {
    Harness harness;
    const auto edited = harness.commit(
        {{"op", "set_component_value"},
         {"object_id", 4},
         {"component_slot", "behavior"},
         {"attachment_index", 1},
         {"field_path", "/params/label"},
         {"value", "expanded"}});
    REQUIRE(edited.at("status") == "committed");
    auto components = harness.target.document.rawJson()
                          .at("scenes")
                          .at("main")
                          .at("objects")
                          .at(3)
                          .at("components");
    REQUIRE(components.at(1).at("params").at("label") == "expanded");
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
    REQUIRE(harness.lifecycle_trace == std::vector<std::string>{
                                           "event:expanded", "update:expanded",
                                           "event:journal-default",
                                           "update:journal-default"});
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
                "handles/" + std::to_string(identity.handle)) != std::string::npos);
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
             EditorEditErrorCode::unknown_component_type,
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

} // namespace Pelican
