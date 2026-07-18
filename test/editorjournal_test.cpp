#include "../src/core/communication/editorjournal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;

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
              {"name":"transform","pos":[5,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}
            ]}
          ]
        }
      }
    })json");
}

class DocumentTarget final : public EditorProjectionDocumentTarget {
  public:
    AuthoringSceneDocument document;

    DocumentTarget()
        : document{AuthoringSceneDocument::load(editorFixture().dump(),
                                                SceneRevision{1})} {}

    const AuthoringSceneDocument &projectionDocument() const override {
        return document;
    }
    SceneRevision nextProjectionRevision() const override {
        return SceneRevision{document.revision().value + 1};
    }
    void publishProjectionDocument(AuthoringSceneDocument &&next) noexcept override {
        document.swap(next);
    }
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
        next_ = context.next_document.encodeSemantic();
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
    DocumentTarget target;
    std::string runtime = target.document.encodeSemantic();
    std::vector<std::string> lifecycle_trace;
    EditorGateObservation gate;
    std::unique_ptr<EditorEditCoordinator> edits;
    std::uint64_t actor = 0;
    std::string reconnect_token;

    Harness() {
        edits = std::make_unique<EditorEditCoordinator>(
            EditorEditRuntimeDependencies{
                .document = [this]() -> const AuthoringSceneDocument & {
                    return target.document;
                },
                .current_scene_id = [] { return std::string{"main"}; },
                .execute = [this](const EditorEditExecutionRequest &request) {
                    RuntimeMirrorAdapter mirror{runtime};
                    std::vector<EditorProjectionAdapter *> adapters{&mirror};
                    EditorProjectionTransaction transaction{target,
                                                            request.base_revision};
                    auto result = transaction.commit(request.commands, adapters);
                    if (result.committed()) {
                        for (const auto &operation : request.operations) {
                            const auto op =
                                operation.at("op").get<std::string>();
                            if (op == "spawn" || op == "restore_objects") {
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
                .gate = [this] { return gate; },
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

TEST_CASE("WP157 edit errors use only the canonical catalog",
          "[editor][journal][wp157][catalog]") {
    for (const auto code : {
             EditorEditErrorCode::stale_revision,
             EditorEditErrorCode::gate_closed,
             EditorEditErrorCode::preview_lease_conflict,
             EditorEditErrorCode::not_editable,
             EditorEditErrorCode::schema_violation,
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
