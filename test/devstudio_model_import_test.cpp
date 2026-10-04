// MI-1s (engine MI-1): an outside model brought into a project from the GUI.
// Where the chosen file goes (as it is in the project, else a copy under
// assets/models), what register_model is sent with and how each answer is
// read -- a stem that is taken numbered, a file registered already, a refusal
// that takes back only the copy this made -- and the sentences the refusals
// read as. The test answers for the engine through the session's transport.

#include "../src/devstudio/model/inspectormodel.hpp"
#include "../src/devstudio/model/modelimport.hpp"

#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace PelicanStudio;
using Json = nlohmann::json;

namespace {

void write(const std::filesystem::path &path, const std::string &bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << bytes;
}

std::string read(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

struct FakeEngine {
    struct Request {
        std::int64_t id = 0;
        std::string method;
        Json params;
    };
    std::vector<Request> sent;
    std::int64_t next = 10;

    ModelRegistrationSession::Transport transport() {
        return {.request = [this](const std::string &method, const Json &params, std::string *) {
            sent.push_back({++next, method, params});
            return next;
        }};
    }
    Request last() const {
        REQUIRE_FALSE(sent.empty());
        return sent.back();
    }
};

struct Outcome {
    std::optional<ModelRegistrationSession::Registered> registered;
    std::optional<std::string> refused;
};

Json registeredResult(const std::string &name, const std::string &path, bool live = true) {
    return Json{{"committed", true},
                {"name", name},
                {"path", path},
                {"asset_data_digest", "b"},
                {"previous_asset_data_digest", "a"},
                {"applied", live ? "live" : "restart_required"},
                {"restart_required", live ? Json(nullptr)
                                          : Json{{"reason", "the model could not be registered in place: busy"}}}};
}

} // namespace

TEST_CASE("MI-1s decision 1: a model file outside the project is copied into assets/models and registered by "
          "its path; a taken stem is numbered _001, _002; a refusal takes back only the copy this made",
          "[devstudio][model_import][mi1s]") {
    QTemporaryDir project_dir;
    QTemporaryDir outside_dir;
    REQUIRE(project_dir.isValid());
    REQUIRE(outside_dir.isValid());
    const std::filesystem::path root{project_dir.path().toStdWString()};
    const std::filesystem::path outside{outside_dir.path().toStdWString()};
    write(root / "project.json", "{}");
    write(outside / "box.glb", "glb bytes");
    // A file of that name is in assets/models already: the copy takes the next.
    write(root / "assets" / "models" / "box.glb", "another box");

    FakeEngine engine;
    ModelRegistrationSession session{engine.transport()};
    Outcome outcome;
    const auto done = [&](const ModelRegistrationSession::Registered &model) { outcome.registered = model; };
    const auto refused = [&](const std::string &sentence) { outcome.refused = sentence; };

    REQUIRE(session.registerFile(root, outside / "box.glb", done, refused));
    const auto copy = root / "assets" / "models" / "box_2.glb";
    CHECK(read(copy) == "glb bytes");
    CHECK(read(root / "assets" / "models" / "box.glb") == "another box");
    // The path alone: the engine names the model by the file's stem.
    CHECK(engine.last().method == "register_model");
    CHECK(engine.last().params == Json{{"path", "assets/models/box_2.glb"}});

    // The stem is a model's name already: sent again, numbered.
    REQUIRE(session.receiveRefusal(engine.last().id, "model.name_conflict@1", "exists", Json{{"name", "box_2"}}));
    CHECK(engine.last().params == Json{{"path", "assets/models/box_2.glb"}, {"name", "box_2_001"}});
    REQUIRE(session.receiveRefusal(engine.last().id, "model.name_conflict@1", "exists", Json{{"name", "box_2_001"}}));
    CHECK(engine.last().params == Json{{"path", "assets/models/box_2.glb"}, {"name", "box_2_002"}});
    CHECK_FALSE(outcome.registered);
    CHECK(session.busy());
    REQUIRE(session.receiveResult(engine.last().id, registeredResult("box_2_002", "assets/models/box_2.glb")));
    REQUIRE(outcome.registered);
    CHECK(outcome.registered->name == "box_2_002");
    CHECK(outcome.registered->live);
    CHECK_FALSE(outcome.registered->already);
    CHECK_FALSE(session.busy());
    CHECK(std::filesystem::exists(copy));

    // The object Add Object places draws it, named after it.
    const auto object = modelObjectDeclaration(uniqueObjectName(modelObjectBaseName("box_2_002"), {}), "box_2_002");
    CHECK(object.at("name") == "box_2_002");
    CHECK(object.at("components").at(1) == Json{{"name", "simplemodelview"}, {"model", "box_2_002"}});
    CHECK(modelObjectBaseName("a.b-c") == "a_b_c");
    CHECK(modelObjectBaseName("..") == "Model");

    // restart_required: written, drawn once the engine starts again.
    outcome = {};
    write(outside / "rig.vrm", "vrm bytes");
    REQUIRE(session.registerFile(root, outside / "rig.vrm", done, refused));
    REQUIRE(session.receiveResult(engine.last().id, registeredResult("rig", "assets/models/rig.vrm", false)));
    REQUIRE(outcome.registered);
    CHECK_FALSE(outcome.registered->live);
    CHECK(outcome.registered->restart_reason == "the model could not be registered in place: busy");

    // A refusal: the copy this made is removed, and the engine's message is
    // in the sentence.
    outcome = {};
    write(outside / "broken.glb", "not a glb");
    REQUIRE(session.registerFile(root, outside / "broken.glb", done, refused));
    const auto broken = root / "assets" / "models" / "broken.glb";
    CHECK(std::filesystem::exists(broken));
    REQUIRE(session.receiveRefusal(engine.last().id, "model.load_failed@1", "register_model: does not load",
                                   Json{{"path", "assets/models/broken.glb"},
                                        {"message", "failed to load gltf file (Invalid glTF binary.)"}}));
    REQUIRE(outcome.refused);
    CHECK(*outcome.refused ==
          "assets/models/broken.glb does not load: failed to load gltf file (Invalid glTF binary.) "
          "(model.load_failed@1).");
    CHECK_FALSE(std::filesystem::exists(broken));

    // A file in the project is registered where it is, and a refusal leaves it.
    outcome = {};
    write(root / "props" / "chair.glb", "chair");
    REQUIRE(session.registerFile(root, root / "props" / "chair.glb", done, refused));
    CHECK(engine.last().params == Json{{"path", "props/chair.glb"}});
    REQUIRE(session.receiveRefusal(engine.last().id, "model.load_failed@1", "no", Json{{"path", "props/chair.glb"}}));
    CHECK(std::filesystem::exists(root / "props" / "chair.glb"));

    // Chosen again: the registration that is there is the answer.
    outcome = {};
    REQUIRE(session.registerFile(root, root / "props" / "chair.glb", done, refused));
    REQUIRE(session.receiveRefusal(engine.last().id, "model.path_conflict@1", "registered already",
                                   Json{{"path", "props/chair.glb"}, {"name", "chair"}}));
    REQUIRE(outcome.registered);
    CHECK(outcome.registered->name == "chair");
    CHECK(outcome.registered->already);

    // An outside .gltf reads files beside it: not copied alone, nothing sent.
    outcome = {};
    write(outside / "scene.gltf", "{}");
    const auto sent = engine.sent.size();
    CHECK_FALSE(session.registerFile(root, outside / "scene.gltf", done, refused));
    REQUIRE(outcome.refused);
    CHECK(outcome.refused->find(".gltf reads the .bin and images beside it") != std::string::npos);
    CHECK(engine.sent.size() == sent);
    CHECK_FALSE(std::filesystem::exists(root / "assets" / "models" / "scene.gltf"));
    // ... while one in the project is registered where it is.
    write(root / "assets" / "models" / "kit" / "scene.gltf", "{}");
    CHECK(session.registerFile(root, root / "assets" / "models" / "kit" / "scene.gltf", done, refused));
    CHECK(engine.last().params == Json{{"path", "assets/models/kit/scene.gltf"}});
}

TEST_CASE("MI-1s decision 5: register_model's and unregister_model's refusals are one sentence each, and an edit "
          "naming a model the project has not registered says so and where it is registered from",
          "[devstudio][model_import][mi1s]") {
    const auto sentence = [](const std::string &code, Json fields, const std::string &message = "refused") {
        return modelRefusalDescription(Json{{"code", code}, {"message", message}, {"error_fields", fields}});
    };
    CHECK(sentence("model.path_invalid@1", {{"path", "../outside.glb"}, {"reason", "dot_segment"}}) ==
          "../outside.glb cannot name a model file: it holds a . or .. folder (model.path_invalid@1).");
    CHECK(sentence("model.format_unsupported@1", {{"path", "assets/models/thing.fbx"}, {"extension", ".fbx"}}) ==
          "assets/models/thing.fbx is not a .glb, .gltf or .vrm file (model.format_unsupported@1).");
    CHECK(sentence("model.file_missing@1", {{"path", "assets/models/none.glb"}}) ==
          "assets/models/none.glb is not a file in the project (model.file_missing@1).");
    CHECK(sentence("model.name_conflict@1", {{"name", "box"}}) ==
          "A model named box exists already (model.name_conflict@1).");
    CHECK(sentence("model.name_invalid@1", {{"name", "engine://primitives/cube"}, {"reason", "engine_primitive"}}) ==
          "engine://primitives/cube is one of the engine's own shapes, which stay (model.name_invalid@1).");
    CHECK(sentence("model.unknown@1", {{"name", "nothing"}}) ==
          "No model is named nothing in this project (model.unknown@1).");
    // in_use: the engine names the first 8; the rest is an ellipsis.
    CHECK(sentence("model.in_use@1", {{"name", "box"}, {"count", 2}, {"objects", {"Crate", "#7"}}}) ==
          "box is used by 2 objects: Crate, #7 (model.in_use@1).");
    CHECK(sentence("model.in_use@1", {{"name", "box"}, {"count", 1}, {"objects", {"Crate"}}}) ==
          "box is used by 1 object: Crate (model.in_use@1).");
    Json nine = Json::array();
    for (int index = 0; index < 8; ++index) nine.push_back("o" + std::to_string(index));
    CHECK(sentence("model.in_use@1", {{"name", "box"}, {"count", 9}, {"objects", nine}}) ==
          "box is used by 9 objects: o0, o1, o2, o3, o4, o5, o6, o7, \xE2\x80\xA6 (model.in_use@1).");
    // A refusal Studio has a sentence for reads the same wherever it is shown.
    CHECK(knownRefusalDescription("model.name_conflict@1", R"(model.name_conflict@1: {"name":"box"})") ==
          "A model named box exists already (model.name_conflict@1).");
    CHECK(modelRefusalDescription(Json{{"code", "material.name_conflict@1"}, {"message", "x"}}).empty());

    // engine MI-1: a spawn of a model the project does not list.
    const Json ticket{{"status", "failed"},
                      {"error",
                       {{"code", "method_unavailable"},
                        {"message", "unknown model asset: checker_box"},
                        {"payload",
                         {{"adapter", "/adapters/object_lifecycle.default_scene.3"},
                          {"method", "spawn"},
                          {"model", "checker_box"},
                          {"reason", "model_unknown"}}}}}};
    const auto unknown = modelUnknownEditDescription(ticket.at("error"));
    CHECK(unknown.starts_with("Model checker_box is not registered in this project. "));
    CHECK(unknown.find("Import\xE2\x80\xA6") != std::string::npos);
    CHECK(modelUnknownEditDescription(Json{{"code", "method_unavailable"}, {"message", "x"}}).empty());
}
