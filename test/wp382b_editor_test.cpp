#include <catch2/catch_test_macros.hpp>

#include "../src/core/communication/editorjournal.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/loader/editorprojectiontransaction.hpp"
#include "../src/core/userpublic/details/schema/structfieldjson.hpp"
#include "../src/core/userpublic/behavior.hpp"
#include "authoringscenetestsupport.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::ordered_json;

struct Wp382bIndependentParams {
    std::string label;
    static constexpr auto schema = structFields(behaviorParamsPolicy,
        defaulted(field<&Wp382bIndependentParams::label>("label"), "independent"));
};
class Wp382bIndependentBehavior final : public Behavior {
  public:
    using Params = Wp382bIndependentParams;
};
PELICAN_REGISTER_BEHAVIOR(Wp382bIndependentBehavior,
                         "wp382b_independent_behavior", 1);

struct Wp382bRow {
    std::string key;
    std::int32_t amount = 7;

    static constexpr auto schema = structFields(
        componentPolicy("wp382b_row"),
        required(field<&Wp382bRow::key>("key")),
        defaulted(field<&Wp382bRow::amount>("amount"), std::int32_t{7}));
};

struct Wp382bCarrier {
    std::int32_t scalar = 0;
    List<Wp382bRow> rows;
    List<Wp382bRow> untouched;

    static constexpr auto schema = structFields(
        componentPolicy("wp382b_carrier"),
        defaulted(field<&Wp382bCarrier::scalar>("scalar"), std::int32_t{0}),
        defaulted(field<&Wp382bCarrier::rows>("rows")),
        defaulted(field<&Wp382bCarrier::untouched>("untouched")));
};

ResolutionResult<ComponentCodecValue> decodeCarrier(
    const nlohmann::json &authored) {
    auto body = authored;
    body.erase("name");
    Wp382bCarrier value;
    internal::decodeStructObject(body, value, Wp382bCarrier::schema,
                                 "component");
    return ResolutionResult<ComponentCodecValue>::success(std::move(value));
}

nlohmann::ordered_json encodeCarrier(const ComponentCodecValue &erased) {
    auto result = Json{{"name", "wp382b_carrier"}};
    const auto fields = internal::encodeStructObject(
        std::any_cast<const Wp382bCarrier &>(erased), Wp382bCarrier::schema,
        "component");
    for (auto field = fields.begin(); field != fields.end(); ++field) {
        result[field.key()] = field.value();
    }
    return result;
}

const SchemaNode &carrierSchema() {
    static const auto schema =
        internal::materializeSchemaNode(Wp382bCarrier::schema);
    return schema;
}

void applyCarrier(const ComponentCodecValue &, void *) {}
ComponentCodecValue projectCarrier(const void *) { return Wp382bCarrier{}; }

ComponentCodec carrierCodec() {
    return ComponentCodec{
        "wp382b_carrier", ComponentCodecRuntimeKind::Ecs,
        EditorProjectionPolicy::EcsCodec, decodeCarrier, encodeCarrier,
        carrierSchema, applyCarrier, projectCarrier, nullptr, nullptr};
}

Json row(std::uint32_t id, std::string key,
         std::optional<std::int32_t> amount = std::nullopt) {
    Json result{{"$row", {{"id", id}, {"v", 1}}},
                {"key", std::move(key)}};
    if (amount) result["amount"] = *amount;
    return result;
}

Json list(std::uint32_t next_id, Json rows) {
    return Json{{"$list", {{"v", 1}, {"next_id", next_id}}},
                {"rows", std::move(rows)}};
}

AuthoringSceneDocument carrierDocument(bool two_attachments = false) {
    Json components = Json::array(
        {{{"name", "wp382b_carrier"},
          {"scalar", 1},
          {"rows", list(3, Json::array({row(1, "one"),
                                         row(2, "two", 2)}))},
          {"untouched", list(2, Json::array({row(1, "fixed")}))}}});
    if (two_attachments) {
        components.push_back(
            {{"name", "wp382b_carrier"},
             {"scalar", 10},
             {"rows", list(3, Json::array({row(1, "other-one"),
                                            row(2, "other-two", 20)}))},
             {"untouched", list(1, Json::array())}});
    }
    return AuthoringSceneDocument::load(
        Json{{"schema", "pelican.scene"},
             {"version", 2},
             {"scenes",
              {{"main",
                {{"objects",
                  Json::array({{{"name", "Root"},
                                {"components", std::move(components)}}})}}}}}}
            .dump(),
        SceneRevision{41});
}

const nlohmann::json &component(
    const test_support::SceneProjectionTarget &target,
    std::size_t index = 0) {
    return target.document.rawJson()
        .at("scenes")
        .at("main")
        .at("objects")
        .at(0)
        .at("components")
        .at(index);
}

Json address(std::size_t occurrence, std::string list_path,
             std::optional<std::uint32_t> row_id = std::nullopt,
             std::string leaf_path = {}) {
    Json result{
        {"authoring_object_id", 1},
        {"component",
         {{"type", "wp382b_carrier"},
          {"occurrence", occurrence},
          {"origin",
           {{"kind", "authored"},
            {"authoring_component_index", occurrence}}}}},
        {"list_path", std::move(list_path)},
    };
    if (row_id) result["row_id"] = *row_id;
    if (!leaf_path.empty()) result["leaf_path"] = std::move(leaf_path);
    return result;
}

std::string semanticWithoutHighWater(const AuthoringSceneDocument &document) {
    // Preserve the actual semantic bytes: reparsing and dumping would hide
    // changes in their representation. Only the allocator's numeric token is
    // allowed to differ across undo/redo. The semantic encoder sorts keys, so
    // next_id is the first member of these fixtures' $list envelopes.
    static const std::regex high_water{R"(("\$list":\{"next_id":)[0-9]+)"};
    return std::regex_replace(AuthoringSceneAuthority::encodeSemantic(document),
                              high_water, "$1<high-water>");
}

struct CoordinatorFixture {
    test_support::SceneProjectionTarget target;
    EditorEditCoordinator edits;
    std::uint64_t actor = 0;
    std::string reconnect_token;

    explicit CoordinatorFixture(AuthoringSceneDocument document,
        decltype(EditorEditRuntimeDependencies::allocate_behavior_attachment)
            allocate_behavior = {})
        : target{std::move(document)},
          edits{EditorEditRuntimeDependencies{
              .document = [&]() -> const AuthoringSceneDocument & {
                  return target.projectionState().authoring();
              },
              .projection_state = [&]() -> const SceneProjectionState & {
                  return target.projectionState();
              },
              .current_scene_id = [] { return std::string{"main"}; },
              .execute = [&](const EditorEditExecutionRequest &request) {
                  EditorProjectionTransaction transaction{target,
                                                          request.base_revision};
                  std::vector<EditorProjectionAdapter *> adapters;
                  return transaction.commit(request.commands, adapters);
              },
              .gate = [] { return EditorGateObservation{}; },
              .allocate_behavior_attachment = std::move(allocate_behavior),
          }} {
        const auto session = edits.openSession({{"display_name", "wp382b"}});
        actor = session.at("actor_id").get<std::uint64_t>();
        reconnect_token = session.at("reconnect_token").get<std::string>();
    }

    Json commit(Json operation, Json *accepted_out = nullptr) {
        const auto accepted = edits.enqueue(
            {{"actor_id", actor},
             {"base_revision", target.projectionState().revision().value},
             {"operations", Json::array({std::move(operation)})}});
        if (accepted_out != nullptr) *accepted_out = accepted;
        REQUIRE(accepted.at("status") == "accepted");
        edits.commitPending();
        const auto result =
            edits.getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("status") == "committed");
        return result;
    }

    Json undo(const Json &transaction) {
        const auto accepted = edits.enqueueUndo(
            {{"actor_id", actor},
             {"base_revision", target.projectionState().revision().value},
             {"transaction_id", transaction.at("transaction_id")}});
        edits.commitPending();
        const auto result =
            edits.getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("status") == "committed");
        return result;
    }

    Json redo(const Json &undo_transaction) {
        const auto accepted = edits.enqueueRedo(
            {{"actor_id", actor},
             {"base_revision", target.projectionState().revision().value},
             {"transaction_id", undo_transaction.at("transaction_id")}});
        edits.commitPending();
        const auto result =
            edits.getResult({{"ticket", accepted.at("ticket")}});
        INFO(result.dump());
        REQUIRE(result.at("status") == "committed");
        return result;
    }
};

#if PELICAN_WITH_RPC
TEST_CASE("WP382b row RPC operations retain raw bytes and journal high water",
          "[WP382b][editor][journal][wire]") {
#else
TEST_CASE("WP382b row coordinator operations retain raw bytes and journal high water",
          "[WP382b][editor][journal][rpc-off]") {
#endif
    auto codecs = std::vector<ComponentCodec>{componentCodecs().begin(),
                                               componentCodecs().end()};
    codecs.push_back(carrierCodec());
    testing::ScopedComponentCodecCatalog catalog{std::move(codecs)};
    CoordinatorFixture fixture{carrierDocument()};

    const auto before = semanticWithoutHighWater(fixture.target.document);
#if PELICAN_WITH_RPC
    std::istringstream input;
    std::ostringstream output;
    RpcServer rpc{input, output};
    rpc.setOrderedHandler("edit", [&](const nlohmann::json &params) {
        return fixture.edits.enqueue(params);
    });
    rpc.setOrderedHandler("undo", [&](const nlohmann::json &params) {
        return fixture.edits.enqueueUndo(params);
    });
    rpc.setOrderedHandler("redo", [&](const nlohmann::json &params) {
        return fixture.edits.enqueueRedo(params);
    });
    rpc.setOrderedHandler("get_edit_result", [&](const nlohmann::json &params) {
        return fixture.edits.getResult(params);
    });
    std::uint64_t request_id = 0;
    const auto call = [&](std::string_view method, Json params) {
        const auto response = Json::parse(rpc.processLine(
            Json{{"jsonrpc", "2.0"},
                 {"id", ++request_id},
                 {"method", method},
                 {"params", std::move(params)}}
                .dump()));
        INFO(response.dump());
        REQUIRE_FALSE(response.contains("error"));
        return response.at("result");
    };
#else
    // RPC is absent from this product configuration. Exercise the same edit
    // and journal contract through its available coordinator entry points.
    const auto call = [&](std::string_view method, Json params) -> Json {
        if (method == "edit") return fixture.edits.enqueue(params);
        if (method == "undo") return fixture.edits.enqueueUndo(params);
        if (method == "redo") return fixture.edits.enqueueRedo(params);
        REQUIRE(std::string{method} == "get_edit_result");
        return fixture.edits.getResult(params);
    };
#endif
    const auto rpc_edit = [&](Json operation) {
        const auto accepted = call(
            "edit", {{"actor_id", fixture.actor},
                     {"base_revision",
                      fixture.target.projectionState().revision().value},
                     {"operations", Json::array({std::move(operation)})}});
        REQUIRE(accepted.at("status") == "accepted");
        fixture.edits.commitPending();
        const auto committed =
            call("get_edit_result", {{"ticket", accepted.at("ticket")}});
        REQUIRE(committed.at("status") == "committed");
        return std::pair{accepted, committed};
    };
    const auto rpc_revert = [&](std::string_view method,
                                const Json &transaction) {
        const auto accepted = call(
            method, {{"actor_id", fixture.actor},
                     {"base_revision",
                      fixture.target.projectionState().revision().value},
                     {"transaction_id", transaction.at("transaction_id")}});
        REQUIRE(accepted.at("status") == "accepted");
        fixture.edits.commitPending();
        const auto committed =
            call("get_edit_result", {{"ticket", accepted.at("ticket")}});
        REQUIRE(committed.at("status") == "committed");
        return committed;
    };
    const Json insert_operation{
        {"op", "insert_component_field"},
        {"address", address(0, "/rows")},
        {"position", {{"before", 1}}},
        {"value", {{"key", "inserted"}, {"amount", 3}}},
    };
    const auto [accepted, inserted] = rpc_edit(insert_operation);
    REQUIRE(accepted.at("row_id") == 3);
    REQUIRE(component(fixture.target).at("rows").at("rows").at(0)
                .at("$row").at("id") == 3);
    const auto after_insert =
        semanticWithoutHighWater(fixture.target.document);

    const auto [set_accepted, set] = rpc_edit(
        {{"op", "set_component_value"},
         {"address", address(0, "/rows", 3, "/amount")},
         {"value", 9}});
    (void)set_accepted;
    REQUIRE(component(fixture.target).at("rows").at("rows").at(0)
                .at("amount") == 9);
    const auto after_set = semanticWithoutHighWater(fixture.target.document);

    const auto [remove_accepted, removed] = rpc_edit(
        {{"op", "remove_component_field"},
         {"address", address(0, "/rows", 3)}});
    (void)remove_accepted;
    REQUIRE(component(fixture.target).at("rows").at("rows").size() == 2);
    REQUIRE(component(fixture.target).at("rows").at("$list").at("next_id") ==
            4);
    const auto after_remove =
        semanticWithoutHighWater(fixture.target.document);

    const auto undo_remove = rpc_revert("undo", removed);
    REQUIRE(semanticWithoutHighWater(fixture.target.document) == after_set);
    const auto undo_set = rpc_revert("undo", set);
    REQUIRE(semanticWithoutHighWater(fixture.target.document) == after_insert);
    const auto undo_insert = rpc_revert("undo", inserted);
    REQUIRE(component(fixture.target).at("rows").at("$list").at("next_id") ==
            4);
    REQUIRE(semanticWithoutHighWater(fixture.target.document) == before);

    const auto redo_insert = rpc_revert("redo", undo_insert);
    REQUIRE(semanticWithoutHighWater(fixture.target.document) == after_insert);
    const auto redo_set = rpc_revert("redo", undo_set);
    REQUIRE(semanticWithoutHighWater(fixture.target.document) == after_set);
    (void)rpc_revert("redo", undo_remove);
    REQUIRE(semanticWithoutHighWater(fixture.target.document) == after_remove);
    REQUIRE(component(fixture.target).at("rows").at("rows").size() == 2);
    REQUIRE(component(fixture.target).at("rows").at("$list").at("next_id") ==
            4);
    REQUIRE(redo_insert.at("status") == "committed");
    REQUIRE(redo_set.at("status") == "committed");

    // Reproduce the production RPC-bytes escape: legacy set_component_value
    // must not be able to rewind the allocator after row 3 was removed.
    const auto rewind = call(
        "edit", {{"actor_id", fixture.actor},
                 {"base_revision",
                  fixture.target.projectionState().revision().value},
                 {"operations",
                  Json::array({{{"op", "set_component_value"},
                                {"object_id", 1},
                                {"component_slot", "wp382b_carrier"},
                                {"field_path", "/rows/$list/next_id"},
                                {"value", 3}}})}});
    REQUIRE(rewind.at("status") == "rejected");
    REQUIRE(rewind.at("error").at("code") ==
            "ListIndexAddressingRejected");
    REQUIRE(rewind.at("error").at("payload").at("pointer_classification") ==
            "list_envelope");
    const auto [next_accepted, next_insert] = rpc_edit(
        {{"op", "insert_component_field"},
         {"address", address(0, "/rows")},
         {"position", {{"end", true}}},
         {"value", {{"key", "after-rejected-rewind"}, {"amount", 4}}}});
    (void)next_insert;
    REQUIRE(next_accepted.at("row_id") == 4);
}

TEST_CASE("WP382b field insertion uses schema defaults and batch allocation is monotonic",
          "[WP382b][editor][insert][allocator]") {
    auto codecs = std::vector<ComponentCodec>{componentCodecs().begin(),
                                               componentCodecs().end()};
    codecs.push_back(carrierCodec());
    testing::ScopedComponentCodecCatalog catalog{std::move(codecs)};
    CoordinatorFixture fixture{carrierDocument()};

    const auto inserted_default = fixture.commit(
        {{"op", "insert_component_field"},
         {"address", address(0, "/rows", 1, "/amount")}});
    REQUIRE(component(fixture.target).at("rows").at("rows").at(0)
                .at("amount") == 7);
    fixture.commit(
        {{"op", "remove_component_field"},
         {"address", address(0, "/rows", 1, "/amount")}});
    REQUIRE_FALSE(component(fixture.target).at("rows").at("rows").at(0)
                      .contains("amount"));
    (void)inserted_default;

    const Json operations = Json::array(
        {{{"op", "insert_component_field"},
          {"address", address(0, "/rows")},
          {"position", {{"end", true}}},
          {"value", {{"key", "copy-a"}, {"amount", 1}}}},
         {{"op", "insert_component_field"},
          {"address", address(0, "/rows")},
          {"position", {{"end", true}}},
          {"value",
           {{"$row", {{"id", 99}, {"v", 1}, {"hint", {{"copy", true}}}}},
            {"key", "copy-b"},
            {"amount", 2}}}}});
    const auto accepted = fixture.edits.enqueue(
        {{"actor_id", fixture.actor},
         {"base_revision", fixture.target.projectionState().revision().value},
         {"operations", operations}});
    REQUIRE(accepted.at("status") == "accepted");
    REQUIRE(accepted.at("inserted_rows").at(0).at("row_id") == 3);
    REQUIRE(accepted.at("inserted_rows").at(1).at("row_id") == 4);
    fixture.edits.commitPending();
    REQUIRE(fixture.edits.getResult({{"ticket", accepted.at("ticket")}})
                .at("status") == "committed");
    const auto &rows = component(fixture.target).at("rows").at("rows");
    REQUIRE(rows.at(rows.size() - 2).at("$row").at("id") == 3);
    REQUIRE(rows.back().at("$row").at("id") == 4);
    REQUIRE(rows.back().begin().key() == "$row");
    REQUIRE(rows.back().at("$row").at("hint").at("copy") == true);
    REQUIRE(component(fixture.target).at("rows").at("$list").at("next_id") ==
            5);

    const auto inserted =
        fixture.edits.getResult({{"ticket", accepted.at("ticket")}});
    (void)fixture.undo(inserted);
    Json next_accepted;
    const auto first_after_undo = fixture.commit(
        {{"op", "insert_component_field"},
         {"address", address(0, "/rows")},
         {"position", {{"end", true}}},
         {"value", {{"key", "after-batch-undo"}}}}, &next_accepted);
    REQUIRE(next_accepted.at("row_id") == 5);
    const auto second_after_undo = fixture.commit(
        {{"op", "insert_component_field"},
         {"address", address(0, "/rows")},
         {"position", {{"end", true}}},
         {"value", {{"key", "after-first-undo"}}}});
    (void)fixture.undo(second_after_undo);
    (void)fixture.undo(first_after_undo);
    REQUIRE(component(fixture.target).at("rows").at("$list").at("next_id") == 7);
}

TEST_CASE("WP382b list edit failures retain their protocol names",
          "[WP382b][editor][negative][envelope]") {
    auto codecs = std::vector<ComponentCodec>{componentCodecs().begin(),
                                               componentCodecs().end()};
    codecs.push_back(carrierCodec());
    testing::ScopedComponentCodecCatalog catalog{std::move(codecs)};

    const auto reject = [](CoordinatorFixture &fixture, Json operation,
                           std::string_view code) {
        const auto result = fixture.edits.enqueue(
            {{"actor_id", fixture.actor},
             {"base_revision", fixture.target.projectionState().revision().value},
             {"operations", Json::array({std::move(operation)})}});
        REQUIRE(result.at("status") == "rejected");
        REQUIRE(result.at("error").at("code").get<std::string>() ==
                std::string{code});
    };

    auto malformed = nlohmann::json::parse(
        test_support::authoring(carrierDocument()).encodeSemantic());
    malformed["scenes"]["main"]["objects"][0]["components"][0]["rows"]
        .erase("$list");
    CoordinatorFixture missing{AuthoringSceneDocument::load(
        malformed.dump(), SceneRevision{90})};
    reject(missing,
           {{"op", "insert_component_field"},
            {"address", address(0, "/rows")},
            {"position", {{"end", true}}},
            {"value", {{"key", "x"}, {"amount", 1}}}},
           "RowEnvelopeMissing");

    auto bare_array_json = nlohmann::json::parse(
        test_support::authoring(carrierDocument()).encodeSemantic());
    bare_array_json["scenes"]["main"]["objects"][0]["components"][0]
                   ["rows"] = Json::array();
    CoordinatorFixture bare_array{AuthoringSceneDocument::load(
        bare_array_json.dump(), SceneRevision{92})};
    reject(bare_array,
           {{"op", "insert_component_field"},
            {"address", address(0, "/rows")},
            {"position", {{"end", true}}},
            {"value", {{"key", "x"}, {"amount", 1}}}},
           "ListContainerMissing");

    auto unsupported_json = nlohmann::json::parse(
        test_support::authoring(carrierDocument()).encodeSemantic());
    unsupported_json["scenes"]["main"]["objects"][0]["components"][0]
                    ["rows"]["$list"]["v"] = 2;
    CoordinatorFixture unsupported{AuthoringSceneDocument::load(
        unsupported_json.dump(), SceneRevision{93})};
    reject(unsupported,
           {{"op", "insert_component_field"},
            {"address", address(0, "/rows")},
            {"position", {{"end", true}}},
            {"value", {{"key", "x"}, {"amount", 1}}}},
           "RowEnvelopeUnsupported");

    auto unsupported_row_json = nlohmann::json::parse(
        test_support::authoring(carrierDocument()).encodeSemantic());
    unsupported_row_json["scenes"]["main"]["objects"][0]["components"][0]
                        ["rows"]["rows"][0]["$row"]["v"] = 2;
    CoordinatorFixture unsupported_row{AuthoringSceneDocument::load(
        unsupported_row_json.dump(), SceneRevision{95})};
    reject(unsupported_row,
           {{"op", "insert_component_field"},
            {"address", address(0, "/rows")},
            {"position", {{"end", true}}},
            {"value", {{"key", "x"}, {"amount", 1}}}},
           "RowEnvelopeUnsupported");

    auto missing_row_json = nlohmann::json::parse(
        test_support::authoring(carrierDocument()).encodeSemantic());
    missing_row_json["scenes"]["main"]["objects"][0]["components"][0]
                    ["rows"]["rows"][0].erase("$row");
    CoordinatorFixture missing_row{AuthoringSceneDocument::load(
        missing_row_json.dump(), SceneRevision{94})};
    reject(missing_row,
           {{"op", "set_component_value"},
            {"address", address(0, "/rows", 1, "/amount")},
            {"value", 1}},
           "RowEnvelopeMissing");

    auto exhausted_json = nlohmann::json::parse(
        test_support::authoring(carrierDocument()).encodeSemantic());
    exhausted_json["scenes"]["main"]["objects"][0]["components"][0]
                  ["rows"]["$list"]["next_id"] =
        std::numeric_limits<std::uint32_t>::max();
    CoordinatorFixture exhausted{AuthoringSceneDocument::load(
        exhausted_json.dump(), SceneRevision{91})};
    reject(exhausted,
           {{"op", "insert_component_field"},
            {"address", address(0, "/rows")},
            {"position", {{"end", true}}},
            {"value", {{"key", "x"}, {"amount", 1}}}},
           "RowIdExhausted");

    CoordinatorFixture witness{carrierDocument()};
    auto bad_address = address(0, "/rows", 1, "/amount");
    bad_address["component"]["origin"]["authoring_component_index"] = 9;
    reject(witness,
           {{"op", "set_component_value"},
            {"address", bad_address},
            {"value", 1}},
           "journal_integrity_error@1");
}

TEST_CASE("WP382b row identity survives insertion and attachment witnesses disambiguate",
          "[WP382b][editor][row-id][attachment]") {
    auto codecs = std::vector<ComponentCodec>{componentCodecs().begin(),
                                               componentCodecs().end()};
    codecs.push_back(carrierCodec());
    testing::ScopedComponentCodecCatalog catalog{std::move(codecs)};
    CoordinatorFixture fixture{carrierDocument(true)};

    const auto client_b =
        fixture.edits.openSession({{"display_name", "client-b"}});
    const auto inserted_by_b = fixture.edits.enqueue(
        {{"actor_id", client_b.at("actor_id")},
         {"base_revision", fixture.target.projectionState().revision().value},
         {"operations",
          Json::array({{{"op", "insert_component_field"},
                        {"address", address(0, "/rows")},
                        {"position", {{"before", 1}}},
                        {"value", {{"key", "front"}, {"amount", 30}}}}})}});
    REQUIRE(inserted_by_b.at("status") == "accepted");
    fixture.edits.commitPending();
    REQUIRE(fixture.edits.getResult({{"ticket", inserted_by_b.at("ticket")}})
                .at("status") == "committed");
    (void)fixture.edits.resumeSession(
        {{"reconnect_token", fixture.reconnect_token}});
    fixture.commit(
        {{"op", "set_component_value"},
         {"address", address(0, "/rows", 2, "/amount")},
         {"value", 22}});
    const auto second_attachment_set = fixture.commit(
        {{"op", "set_component_value"},
         {"address", address(1, "/rows", 2, "/amount")},
         {"value", 222}});

    REQUIRE(component(fixture.target, 0).at("rows").at("rows").at(2)
                .at("$row").at("id") == 2);
    REQUIRE(component(fixture.target, 0).at("rows").at("rows").at(2)
                .at("amount") == 22);
    REQUIRE(component(fixture.target, 1).at("rows").at("rows").at(1)
                .at("amount") == 222);
    const auto undone = fixture.undo(second_attachment_set);
    REQUIRE(component(fixture.target, 0).at("rows").at("rows").at(2)
                .at("amount") == 22);
    REQUIRE(component(fixture.target, 1).at("rows").at("rows").at(1)
                .at("amount") == 20);
    (void)fixture.redo(undone);
    REQUIRE(component(fixture.target, 1).at("rows").at("rows").at(1)
                .at("amount") == 222);
}

TEST_CASE("WP382b separate attachments do not create a false undo conflict",
          "[WP382b][editor][journal][attachment][two-actor][undo]") {
    auto codecs = std::vector<ComponentCodec>{componentCodecs().begin(),
                                               componentCodecs().end()};
    codecs.push_back(carrierCodec());
    testing::ScopedComponentCodecCatalog catalog{std::move(codecs)};
    CoordinatorFixture fixture{carrierDocument(true)};

    const auto actor_a_insert = fixture.commit(
        {{"op", "insert_component_field"},
         {"address", address(0, "/rows")},
         {"position", {{"end", true}}},
         {"value", {{"key", "actor-a"}, {"amount", 3}}}});
    const auto actor_b_session =
        fixture.edits.openSession({{"display_name", "actor-b"}});
    const auto actor_b_set = fixture.edits.enqueue(
        {{"actor_id", actor_b_session.at("actor_id")},
         {"base_revision", fixture.target.projectionState().revision().value},
         {"operations",
          Json::array({{{"op", "set_component_value"},
                        {"address", address(1, "/rows", 2, "/amount")},
                        {"value", 222}}})}});
    REQUIRE(actor_b_set.at("status") == "accepted");
    fixture.edits.commitPending();
    REQUIRE(fixture.edits.getResult({{"ticket", actor_b_set.at("ticket")}})
                .at("status") == "committed");
    (void)fixture.edits.resumeSession(
        {{"reconnect_token", fixture.reconnect_token}});

    const auto undone = fixture.undo(actor_a_insert);
    REQUIRE(undone.at("status") == "committed");
    REQUIRE(component(fixture.target, 0).at("rows").at("rows").size() == 2);
    REQUIRE(component(fixture.target, 1).at("rows").at("rows").at(1)
                .at("amount") == 222);
}

TEST_CASE("WP382b behavior insertion leaves an independent component list undo available",
          "[WP382b][editor][journal][attachment][two-actor][undo]") {
    auto codecs = std::vector<ComponentCodec>{componentCodecs().begin(),
                                               componentCodecs().end()};
    codecs.push_back(carrierCodec());
    testing::ScopedComponentCodecCatalog catalog{std::move(codecs)};
    CoordinatorFixture fixture{carrierDocument(),
        [](std::uint64_t revision, std::size_t, std::size_t occurrence) {
            return EditorBehaviorAttachmentIdentity{
                .handle = revision, .attachment_seq = occurrence};
        }};
    const auto edited = fixture.commit({{"op", "set_component_value"},
        {"address", address(0, "/rows", 2, "/amount")}, {"value", 99}});
    const auto actor_a = fixture.actor;
    fixture.actor = fixture.edits.openSession({{"display_name", "actor-b"}})
                        .at("actor_id").get<std::uint64_t>();
    fixture.commit({{"op", "add_component"}, {"object_id", 1},
        {"component", {{"name", "behavior"},
                       {"type", "wp382b_independent_behavior"}}}});
    (void)fixture.edits.resumeSession({{"reconnect_token", fixture.reconnect_token}});
    fixture.actor = actor_a;
    (void)fixture.undo(edited);
    REQUIRE(component(fixture.target).at("rows").at("rows").at(1)
                .at("amount") == 2);
    REQUIRE(component(fixture.target, 1).at("type") == "wp382b_independent_behavior");
}

TEST_CASE("WP382b later client list structure prevents a stale row undo",
          "[WP382b][editor][cas][journal]") {
    auto codecs = std::vector<ComponentCodec>{componentCodecs().begin(),
                                               componentCodecs().end()};
    codecs.push_back(carrierCodec());
    testing::ScopedComponentCodecCatalog catalog{std::move(codecs)};
    CoordinatorFixture fixture{carrierDocument()};

    const auto first = fixture.commit(
        {{"op", "insert_component_field"},
         {"address", address(0, "/rows")},
         {"position", {{"end", true}}},
         {"value", {{"key", "client-a"}, {"amount", 3}}}});
    const auto actor_b_session =
        fixture.edits.openSession({{"display_name", "client-b"}});
    const auto actor_b = actor_b_session.at("actor_id");
    const auto accepted_b = fixture.edits.enqueue(
        {{"actor_id", actor_b},
         {"base_revision", fixture.target.projectionState().revision().value},
         {"operations",
          Json::array({{{"op", "insert_component_field"},
                        {"address", address(0, "/rows")},
                        {"position", {{"end", true}}},
                        {"value", {{"key", "client-b"}, {"amount", 4}}}}})}});
    REQUIRE(accepted_b.at("status") == "accepted");
    fixture.edits.commitPending();
    REQUIRE(fixture.edits.getResult({{"ticket", accepted_b.at("ticket")}})
                .at("status") == "committed");
    (void)fixture.edits.resumeSession(
        {{"reconnect_token", fixture.reconnect_token}});

    const auto rejected = fixture.edits.enqueueUndo(
        {{"actor_id", fixture.actor},
         {"base_revision", fixture.target.projectionState().revision().value},
         {"transaction_id", first.at("transaction_id")}});
    REQUIRE(rejected.at("status") == "rejected");
    REQUIRE(rejected.at("error").at("code") == "undo_conflict");
    REQUIRE(component(fixture.target).at("rows").at("rows").size() == 4);
    REQUIRE(component(fixture.target).at("rows").at("$list").at("next_id") ==
            5);
}

TEST_CASE("WP382b scalar raw patch preserves list subtrees and named negatives",
          "[WP382b][editor][raw][negative]") {
    auto codecs = std::vector<ComponentCodec>{componentCodecs().begin(),
                                               componentCodecs().end()};
    codecs.push_back(carrierCodec());
    testing::ScopedComponentCodecCatalog catalog{std::move(codecs)};
    CoordinatorFixture fixture{carrierDocument()};

    const auto old_component = component(fixture.target);
    const auto old_rows = old_component.at("rows").dump();
    const auto old_untouched = old_component.at("untouched").dump();
    REQUIRE_FALSE(old_component.at("rows").at("rows").at(0)
                      .contains("amount"));
    fixture.commit({{"op", "set_component_value"},
                    {"object_id", 1},
                    {"component_slot", "wp382b_carrier"},
                    {"field_path", "/scalar"},
                    {"value", 8}});
    const auto &after = component(fixture.target);
    REQUIRE(after.at("rows").dump() == old_rows);
    REQUIRE(after.at("untouched").dump() == old_untouched);
    REQUIRE_FALSE(after.at("rows").at("rows").at(0).contains("amount"));
    auto expected = old_component;
    expected["scalar"] = 8;
    REQUIRE(after == expected);

    const auto indexed = fixture.edits.enqueue(
        {{"actor_id", fixture.actor},
         {"base_revision", fixture.target.projectionState().revision().value},
         {"operations",
          Json::array({{{"op", "set_component_value"},
                        {"object_id", 1},
                        {"component_slot", "wp382b_carrier"},
                        {"field_path", "/rows/rows/0/amount"},
                        {"value", 1}}})}});
    REQUIRE(indexed.at("status") == "rejected");
    REQUIRE(indexed.at("error").at("code") ==
            "ListIndexAddressingRejected");

    // An index that points at a leaf that really exists must still be
    // refused by name, and the value must not change.
    REQUIRE(after.at("rows").at("rows").at(1).contains("amount"));
    const auto existing_amount = after.at("rows").at("rows").at(1).at("amount");
    const auto indexed_existing = fixture.edits.enqueue(
        {{"actor_id", fixture.actor},
         {"base_revision", fixture.target.projectionState().revision().value},
         {"operations",
          Json::array({{{"op", "set_component_value"},
                        {"object_id", 1},
                        {"component_slot", "wp382b_carrier"},
                        {"field_path", "/rows/rows/1/amount"},
                        {"value", existing_amount.get<int>() + 1000}}})}});
    REQUIRE(indexed_existing.at("status") == "rejected");
    REQUIRE(indexed_existing.at("error").at("code") ==
            "ListIndexAddressingRejected");
    REQUIRE(component(fixture.target).at("rows").at("rows").at(1).at("amount") ==
            existing_amount);

    const auto missing = fixture.edits.enqueue(
        {{"actor_id", fixture.actor},
         {"base_revision", fixture.target.projectionState().revision().value},
         {"operations",
          Json::array({{{"op", "set_component_value"},
                        {"object_id", 1},
                        {"component_slot", "wp382b_carrier"},
                        {"field_path", "/absent"},
                        {"value", 1}}})}});
    REQUIRE(missing.at("status") == "rejected");
    REQUIRE(missing.at("error").at("code") == "schema_violation");

    const auto missing_row_leaf = fixture.edits.enqueue(
        {{"actor_id", fixture.actor},
         {"base_revision", fixture.target.projectionState().revision().value},
         {"operations",
          Json::array({{{"op", "set_component_value"},
                        {"address", address(0, "/rows", 1, "/amount")},
                        {"value", 12}}})}});
    REQUIRE(missing_row_leaf.at("status") == "rejected");
    REQUIRE(missing_row_leaf.at("error").at("code") == "schema_violation");
    REQUIRE_FALSE(component(fixture.target).at("rows").at("rows").at(0)
                      .contains("amount"));
}

TEST_CASE("WP382b legacy field operations reject every list pointer class",
          "[WP382b][editor][negative][pointer][matrix]") {
    auto codecs = std::vector<ComponentCodec>{componentCodecs().begin(),
                                               componentCodecs().end()};
    codecs.push_back(carrierCodec());
    testing::ScopedComponentCodecCatalog catalog{std::move(codecs)};
    CoordinatorFixture fixture{carrierDocument()};

    struct PointerCase {
        const char *path;
        const char *classification;
    };
    const std::vector<PointerCase> pointers{
        {"/rows", "list_root"},
        {"/rows/rows", "rows_container"},
        {"/rows/$list", "list_envelope"},
        {"/rows/rows/0/$row", "row_envelope"},
        {"/rows/rows/0/amount", "numeric_index"},
        {"/rows/rows/-", "append_index"},
    };
    for (const auto op : {"set_component_value", "insert_component_field",
                          "remove_component_field"}) {
        for (const auto &pointer : pointers) {
            DYNAMIC_SECTION(op << " " << pointer.path) {
                Json operation{{"op", op},
                               {"object_id", 1},
                               {"component_slot", "wp382b_carrier"},
                               {"field_path", pointer.path}};
                if (std::string_view{op} == "set_component_value") {
                    operation["value"] = 0;
                }
                const auto before =
                    fixture.target.document.encodeSemantic();
                const auto rejected = fixture.edits.enqueue(
                    {{"actor_id", fixture.actor},
                     {"base_revision",
                      fixture.target.projectionState().revision().value},
                     {"operations", Json::array({std::move(operation)})}});
                REQUIRE(rejected.at("status") == "rejected");
                REQUIRE(rejected.at("error").at("code") ==
                        "ListIndexAddressingRejected");
                REQUIRE(rejected.at("error")
                            .at("payload")
                            .at("pointer_classification") ==
                        pointer.classification);
                REQUIRE(fixture.target.document.encodeSemantic() ==
                        before);
            }
        }
    }
}

TEST_CASE("WP382b spawn preserves a non-behavior component authored form",
          "[WP382b][editor][journal][spawn][raw][undo][redo]") {
    auto codecs = std::vector<ComponentCodec>{componentCodecs().begin(),
                                               componentCodecs().end()};
    codecs.push_back(carrierCodec());
    testing::ScopedComponentCodecCatalog catalog{std::move(codecs)};
    CoordinatorFixture fixture{carrierDocument()};

    const Json authored_component{
        {"name", "wp382b_carrier"},
        {"rows", list(2, Json::array({row(1, "spawned")}))},
    };
    const auto before = fixture.target.document.encodeSemantic();
    const auto spawned = fixture.commit(
        {{"op", "spawn"},
         {"scene_id", "main"},
         {"object",
          {{"name", "SpawnedRaw"},
           {"components", Json::array({authored_component})}}}});
    const auto &spawned_component = fixture.target.document.rawJson()
                                        .at("scenes")
                                        .at("main")
                                        .at("objects")
                                        .back()
                                        .at("components")
                                        .at(0);
    REQUIRE(spawned_component == authored_component);
    REQUIRE_FALSE(spawned_component.contains("scalar"));
    REQUIRE_FALSE(spawned_component.contains("untouched"));
    const auto after = fixture.target.document.encodeSemantic();

    const auto undone = fixture.undo(spawned);
    REQUIRE(fixture.target.document.encodeSemantic() == before);
    (void)fixture.redo(undone);
    REQUIRE(fixture.target.document.encodeSemantic() == after);
    REQUIRE(fixture.target.document.rawJson()
                .at("scenes")
                .at("main")
                .at("objects")
                .back()
                .at("components")
                .at(0) == authored_component);
}

TEST_CASE("WP382b persisted list high water is not recomputed from rows",
          "[WP382b][editor][high-water][reload]") {
    auto codecs = std::vector<ComponentCodec>{componentCodecs().begin(),
                                               componentCodecs().end()};
    codecs.push_back(carrierCodec());
    testing::ScopedComponentCodecCatalog catalog{std::move(codecs)};
    CoordinatorFixture first{carrierDocument()};
    first.commit({{"op", "remove_component_field"},
                  {"address", address(0, "/rows", 2)}});
    const auto saved = first.target.document.encodeSemantic();

    CoordinatorFixture reloaded{AuthoringSceneDocument::load(
        saved, SceneRevision{80})};
    Json accepted;
    reloaded.commit(
        {{"op", "insert_component_field"},
         {"address", address(0, "/rows")},
         {"position", {{"end", true}}},
         {"value", {{"key", "after-reload"}, {"amount", 5}}}},
        &accepted);
    REQUIRE(accepted.at("row_id") == 3);
    REQUIRE(component(reloaded.target).at("rows").at("$list").at("next_id") ==
            4);
}

} // namespace
} // namespace Pelican
