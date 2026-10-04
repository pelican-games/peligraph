// HW-2s / AV-1s: the Assets dock -- its tree, Reload when saved outside on
// every row, the line that says what is watched, Track asset contents, the
// contents check and its two actions -- on the real widgets, with the test
// answering for the engine (PJ-1's get_project_settings /
// edit_project_settings, AV-1's verify_assets / update_assets_manifest /
// get_assets_verification, get_status). And decision 7: the Frame Plan's
// shader follower when the engine's own watch is open.

#include "../src/devstudio/model/assetsdockmodel.hpp"
#include "../src/devstudio/model/inspectormodel.hpp"
#include "../src/devstudio/model/modelimport.hpp"
#include "assetsdock.hpp"
#include "shaderreloadfollower.hpp"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>

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

QApplication &application() {
    static int argc = 1;
    static char name[] = "devstudio_assets_dock_test";
    static char *argv[] = {name, nullptr};
    static QApplication app{argc, argv};
    return app;
}

void write(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << text;
}

// A Blank project's assets/ (projecttemplate.cpp), with one texture.
std::filesystem::path blankProject(const QTemporaryDir &temporary) {
    const std::filesystem::path root{temporary.path().toStdWString()};
    write(root / "project.json", R"({"schema":"pelican.project","version":1,"name":"assets"})");
    write(root / "assets" / ".gitkeep", "");
    write(root / "assets" / "asset_data.json", "{}");
    write(root / "assets" / "models" / ".gitkeep", "");
    write(root / "assets" / "textures" / ".gitkeep", "");
    write(root / "assets" / "textures" / "rock.ktx2", "rock");
    write(root / "assets" / "audio" / ".gitkeep", "");
    return root;
}

// Takes every request the dock sends; the test answers them as the engine.
struct FakeEngine {
    struct Request {
        qint64 id = 0;
        std::string method;
        Json params;
    };
    std::vector<Request> pending;
    qint64 next = 100;

    AssetsDock::Hooks hooks(std::vector<std::filesystem::path> documents = {}) {
        return AssetsDock::Hooks{
            .send = [this](const QString &method, const QJsonObject &params, QString *) {
                pending.push_back({++next, method.toStdString(),
                                   Json::parse(QJsonDocument(params)
                                                   .toJson(QJsonDocument::Compact)
                                                   .toStdString())});
                return next;
            },
            .restart = {},
            .render_documents = [documents] { return documents; },
        };
    }

    std::optional<Request> take(const std::string &method) {
        for (auto it = pending.begin(); it != pending.end(); ++it)
            if (it->method == method) {
                auto request = *it;
                pending.erase(it);
                return request;
            }
        return std::nullopt;
    }

    [[nodiscard]] bool asked(const std::string &method) const {
        for (const auto &request : pending)
            if (request.method == method) return true;
        return false;
    }
};

// As the viewport hands a reply on: a get_status to every reader first
// (engineStatusParsed), then to whoever asked (inspectorRpcSucceeded).
void answer(AssetsDock &dock, const FakeEngine::Request &request, const Json &result) {
    const auto bytes = QByteArray::fromStdString(result.dump());
    if (request.method == "get_status") {
        dock.observeStatus(request.id, result);
        CHECK_FALSE(dock.receiveResult(request.id, bytes));
        return;
    }
    REQUIRE(dock.receiveResult(request.id, bytes));
}

void answer(AssetsDock &dock, FakeEngine &engine, const std::string &method, const Json &result) {
    const auto request = engine.take(method);
    REQUIRE(request.has_value());
    answer(dock, *request, result);
}

Json settings(const std::string &digest, Json hot_reload = nullptr, Json asset_stores = nullptr) {
    return Json{{"project_digest", digest}, {"hot_reload", hot_reload}, {"asset_stores", asset_stores}};
}

Json status(Json watch = nullptr, std::uint64_t hashed = 0,
            Json verification = Json{{"state", "not_run"},
                                     {"reason", "no asset store declares a manifest"}},
            Json stores = Json::object()) {
    return Json{{"reload", {{"watch", watch}, {"hashed_files", hashed}, {"scans", 0}}},
                {"assets_verification", verification},
                {"stores", stores}};
}

Json committed(const std::string &digest, Json applied, Json previous, Json verification = nullptr) {
    Json result{{"committed", true},  {"changed", true},          {"project_digest", digest},
                {"applied", applied}, {"previous", previous},     {"restart_required", nullptr},
                {"asset_stores_change",
                 {{"added", Json::array()}, {"removed", Json::array()},
                  {"manifests_declared", Json::array()}, {"manifests_removed", Json::array()}}}};
    if (!verification.is_null()) result["assets_verification"] = verification;
    return result;
}

QTreeWidgetItem *row(QTreeWidget &tree, const QString &logical) {
    for (QTreeWidgetItemIterator it{&tree}; *it; ++it)
        if ((*it)->data(0, AssetLogicalPathRole).toString() == logical) return *it;
    return nullptr;
}

// The engine is up and has said what project.json holds and what it watches.
void ready(AssetsDock &dock, FakeEngine &engine, Json hot_reload = nullptr,
           Json asset_stores = nullptr, Json reading = status()) {
    dock.engineAvailable();
    answer(dock, engine, "get_project_settings", settings("d0", hot_reload, asset_stores));
    answer(dock, engine, "get_status", reading);
}

} // namespace

TEST_CASE("HW-2s decision 1: the Assets dock is the project's asset folders, without .gitkeep, "
          "and follows the disk",
          "[devstudio][assets-dock][hw2s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = blankProject(temporary);
    write(root / "art" / "logo.png", "png");
    FakeEngine engine;
    AssetsDock dock{engine.hooks()};
    dock.show();
    dock.open(root);
    auto &tree = *dock.tree();
    REQUIRE(tree.topLevelItemCount() == 1);
    QTreeWidgetItem *assets = tree.topLevelItem(0);
    CHECK(assets->text(0) == QStringLiteral("assets"));
    CHECK(assets->isExpanded());
    // Folders first, by name; the template's .gitkeep files are not rows.
    REQUIRE(assets->childCount() == 4);
    CHECK(assets->child(0)->text(0) == QStringLiteral("audio"));
    CHECK(assets->child(1)->text(0) == QStringLiteral("models"));
    CHECK(assets->child(2)->text(0) == QStringLiteral("textures"));
    CHECK(assets->child(3)->text(0) == QStringLiteral("asset_data.json"));
    CHECK(row(tree, QStringLiteral("assets/.gitkeep")) == nullptr);
    REQUIRE(row(tree, QStringLiteral("assets/textures/rock.ktx2")) != nullptr);
    CHECK(row(tree, QStringLiteral("assets/textures/.gitkeep")) == nullptr);

    // A file saved into a folder becomes a row, without asking for it.
    write(root / "assets" / "textures" / "moss.ktx2", "moss");
    for (int tick = 0; tick < 60 && row(tree, QStringLiteral("assets/textures/moss.ktx2")) == nullptr;
         ++tick)
        QTest::qWait(50);
    CHECK(row(tree, QStringLiteral("assets/textures/moss.ktx2")) != nullptr);

    // A store mounted outside assets/ is a top folder of its own, read from
    // where the engine says its root is.
    ready(dock, engine, nullptr, Json{{"art", {{"mount", "art/"}}}},
          status(nullptr, 0,
                 Json{{"state", "not_run"}, {"reason", "no asset store declares a manifest"}},
                 Json{{"art", (temporary.path() + QStringLiteral("/art")).toStdString()}}));
    REQUIRE(tree.topLevelItemCount() == 2);
    CHECK(tree.topLevelItem(1)->text(0) == QStringLiteral("art"));
    CHECK(row(tree, QStringLiteral("art/logo.png")) != nullptr);

    // Another project: nothing of this one.
    dock.open({});
    CHECK(tree.topLevelItemCount() == 0);
}

TEST_CASE("HW-2s decision 2: a row's switch is one edit of hot_reload, a folder's covers what is "
          "inside it, and Undo sends what was there before",
          "[devstudio][assets-dock][hw2s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = blankProject(temporary);
    FakeEngine engine;
    AssetsDock dock{engine.hooks()};
    dock.show();
    dock.open(root);
    ready(dock, engine);
    auto &tree = *dock.tree();
    QTreeWidgetItem *textures = row(tree, QStringLiteral("assets/textures"));
    REQUIRE(textures != nullptr);
    // The default is no watch: nothing is ticked and nothing was written.
    CHECK(textures->checkState(1) == Qt::Unchecked);
    CHECK(dock.watchLine()->isHidden());
    CHECK(engine.pending.empty());

    tree.setCurrentItem(textures);
    REQUIRE(dock.reloadAction()->isEnabled());
    CHECK_FALSE(dock.reloadAction()->isChecked());
    dock.reloadAction()->trigger();
    // Against the digest of project.json as it is now.
    answer(dock, engine, "get_project_settings", settings("d1"));
    const auto edit = engine.take("edit_project_settings");
    REQUIRE(edit.has_value());
    CHECK(edit->params ==
          Json{{"base_project_digest", "d1"}, {"hot_reload", Json::array({"assets/textures"})}});
    // The box shows what was asked for while the engine answers.
    CHECK(textures->checkState(1) == Qt::Checked);
    answer(dock, *edit,
           committed("d2", {{"hot_reload", {"assets/textures"}}, {"asset_stores", nullptr}},
                     {{"hot_reload", nullptr}, {"asset_stores", nullptr}}));
    answer(dock, engine, "get_status", status(Json::array({"assets/textures"})));
    CHECK(textures->checkState(1) == Qt::Checked);
    // A file inside is watched through its folder: a grey mark, not a tick.
    QTreeWidgetItem *rock = row(tree, QStringLiteral("assets/textures/rock.ktx2"));
    REQUIRE(rock != nullptr);
    CHECK(rock->checkState(1) == Qt::PartiallyChecked);
    CHECK(rock->text(1) == QStringLiteral("via textures"));
    CHECK(row(tree, QStringLiteral("assets/models"))->checkState(1) == Qt::Unchecked);
    // ... and cannot be switched off on its own.
    rock->setCheckState(1, Qt::Unchecked);
    CHECK(engine.pending.empty());
    CHECK(rock->checkState(1) == Qt::PartiallyChecked);
    CHECK(dock.noticeLine()->text().contains(QStringLiteral("untick assets/textures")));

    // One edit, one Undo: the value before it, not the digest.
    REQUIRE(dock.undoAction()->isEnabled());
    CHECK(dock.undoAction()->text() ==
          QStringLiteral("Undo Reload when saved outside (assets/textures)"));
    dock.undoAction()->trigger();
    answer(dock, engine, "get_project_settings", settings("d2", Json::array({"assets/textures"})));
    const auto undo = engine.take("edit_project_settings");
    REQUIRE(undo.has_value());
    CHECK(undo->params == Json{{"base_project_digest", "d2"}, {"hot_reload", nullptr}});
    answer(dock, *undo,
           committed("d3", {{"hot_reload", nullptr}, {"asset_stores", nullptr}},
                     {{"hot_reload", {"assets/textures"}}, {"asset_stores", nullptr}}));
    answer(dock, engine, "get_status", status());
    CHECK(textures->checkState(1) == Qt::Unchecked);
    CHECK_FALSE(dock.undoAction()->isEnabled());

    // A refusal is one sentence, and project.json is the engine's to keep.
    tree.setCurrentItem(textures);
    dock.reloadAction()->trigger();
    answer(dock, engine, "get_project_settings", settings("d3"));
    const auto refused = engine.take("edit_project_settings");
    REQUIRE(refused.has_value());
    CHECK(dock.receiveRefusal(
        refused->id, -32000, QStringLiteral("edit_project_settings: project.json changed"),
        QJsonDocument::fromJson(
            R"({"error_code":"external_modification","error_fields":{"expected":"d3","actual":"d4"}})")
            .object()));
    CHECK_FALSE(dock.receiveFailure(refused->id, QStringLiteral("RPC -32000: x")));
    CHECK(dock.noticeLine()->text().startsWith(QStringLiteral("project.json changed outside Studio")));
    // ... and the file is read again for the next edit.
    CHECK(engine.asked("get_project_settings"));
}

TEST_CASE("HW-2s decision 3: the line above the tree says what is watched and what was read "
          "again last; a render document's row says the watch does not apply it",
          "[devstudio][assets-dock][hw2s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = blankProject(temporary);
    write(root / "assets" / "passes" / "extra.json", "{}");
    FakeEngine engine;
    AssetsDock dock{engine.hooks({root / "assets" / "passes" / "extra.json"})};
    dock.show();
    dock.open(root);
    ready(dock, engine, Json::array({"assets/textures", "assets/asset_data.json"}), nullptr,
          status(Json::array({"assets/textures", "assets/asset_data.json"}), 4));
    CHECK(dock.watchLine()->text() == QStringLiteral("Watching 1 folder and 1 file"));

    // A save outside under the watch: get_status is read until the engine's
    // count of hashed files moves, and the file is named.
    write(root / "assets" / "textures" / "rock.ktx2", "rock, painted again");
    dock.rescan();
    for (int tick = 0; tick < 40 && !engine.asked("get_status"); ++tick) QTest::qWait(50);
    REQUIRE(engine.asked("get_status"));
    answer(dock, engine, "get_status",
           status(Json::array({"assets/textures", "assets/asset_data.json"}), 4));
    // Not yet: asked again a little later.
    for (int tick = 0; tick < 40 && !engine.asked("get_status"); ++tick) QTest::qWait(50);
    REQUIRE(engine.asked("get_status"));
    answer(dock, engine, "get_status",
           status(Json::array({"assets/textures", "assets/asset_data.json"}), 5));
    CHECK(dock.watchLine()->text() ==
          QStringLiteral("Watching 1 folder and 1 file · last reload: textures/rock.ktx2"));
    // Then nothing more is read while nothing is written.
    QTest::qWait(AssetsDockSession::ReloadFollowMs * 2);
    CHECK_FALSE(engine.asked("get_status"));

    // A file outside the watch is not followed.
    write(root / "assets" / "models" / "a.glb", "glb");
    dock.rescan();
    QTest::qWait(AssetsDockSession::ReloadFollowMs * 2);
    CHECK_FALSE(engine.asked("get_status"));

    // RL-1: the render document's row says what takes it in instead.
    QTreeWidgetItem *document = row(*dock.tree(), QStringLiteral("assets/passes/extra.json"));
    REQUIRE(document != nullptr);
    CHECK(document->toolTip(0).contains(QStringLiteral("Reload from disk")));
}

TEST_CASE("HW-2s decision 4: Track asset contents declares main at assets/ with a manifest in one "
          "edit, is offered only while no store declares one, and its Undo can be refused",
          "[devstudio][assets-dock][hw2s][av1s]") {
    application();
    // What it declares.
    CHECK(assetStoresWithManifest(nullptr) ==
          Json{{"main", {{"mount", "assets/"}, {"manifest", "assets.manifest.json"}}}});
    CHECK(assetStoresWithManifest(Json{{"main", {{"mount", "assets/"}}}, {"art", {{"mount", "art/"}}}}) ==
          Json{{"art", {{"mount", "art/"}, {"manifest", "assets.manifest.json"}}},
               {"main", {{"mount", "assets/"}, {"manifest", "assets.main.manifest.json"}}}});
    CHECK(assetStoresDeclareManifest(Json{{"main", {{"mount", "assets/"}, {"manifest", "m.json"}}}}));

    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = blankProject(temporary);
    FakeEngine engine;
    AssetsDock dock{engine.hooks()};
    dock.show();
    dock.open(root);
    // Before the engine has said what project.json holds, nothing is offered.
    CHECK(dock.trackButton()->isHidden());
    ready(dock, engine);
    REQUIRE_FALSE(dock.trackButton()->isHidden());
    QTest::mouseClick(dock.trackButton(), Qt::LeftButton);
    answer(dock, engine, "get_project_settings", settings("d1"));
    const auto edit = engine.take("edit_project_settings");
    REQUIRE(edit.has_value());
    CHECK(edit->params ==
          Json{{"base_project_digest", "d1"},
               {"asset_stores", {{"main", {{"mount", "assets/"}, {"manifest", "assets.manifest.json"}}}}}});
    const Json declared{{"main", {{"mount", "assets/"}, {"manifest", "assets.manifest.json"}}}};
    answer(dock, *edit,
           committed("d2", {{"hot_reload", nullptr}, {"asset_stores", declared}},
                     {{"hot_reload", nullptr}, {"asset_stores", nullptr}},
                     {{"state", "running"}, {"failed", Json::array()},
                      {"generated", Json::array({{{"store", "main"}, {"files", 6}}})}}));
    // The check runs on the engine's worker: get_status is read until it ends.
    CHECK(dock.trackButton()->isHidden());
    answer(dock, engine, "get_status", status(nullptr, 0, {{"state", "running"}}));
    CHECK(dock.verificationLine()->text() == QStringLiteral("Checking asset contents…"));
    for (int tick = 0; tick < 40 && !engine.asked("get_status"); ++tick) QTest::qWait(50);
    answer(dock, engine, "get_status",
           status(nullptr, 0,
                  {{"state", "verified"}, {"stores", 1}, {"matched", 6}, {"info", 0},
                   {"warnings", 0}, {"errors", 0}}));
    answer(dock, engine, "get_assets_verification",
           {{"state", "verified"},
            {"stores", Json::array({{{"name", "main"},
                                     {"root", root.generic_string() + "/assets"},
                                     {"manifest", root.generic_string() + "/assets.manifest.json"},
                                     {"matched", 6},
                                     {"issues", Json::array()}}})}});
    CHECK(dock.verificationLine()->text() ==
          QStringLiteral("Asset contents match the manifest (6 files)."));
    CHECK(dock.trackButton()->isHidden());

    // Its Undo takes the store away; an engine that had the store when it
    // started cannot, and says so in one sentence.
    CHECK(dock.undoAction()->text() == QStringLiteral("Undo Track asset contents"));
    dock.undoAction()->trigger();
    answer(dock, engine, "get_project_settings", settings("d2", nullptr, declared));
    const auto undo = engine.take("edit_project_settings");
    REQUIRE(undo.has_value());
    CHECK(undo->params == Json{{"base_project_digest", "d2"}, {"asset_stores", nullptr}});
    CHECK(dock.receiveRefusal(
        undo->id, -32000, QStringLiteral("edit_project_settings: project.settings_change_unsupported@1"),
        QJsonDocument::fromJson(
            R"({"error_code":"project.settings_change_unsupported@1","error_fields":{"key":"asset_stores","store":"main","reason":"the store was declared when the engine started"}})")
            .object()));
    CHECK(dock.noticeLine()->text().contains(QStringLiteral("so it stays declared")));
    CHECK(projectSettingsRefusalDescription(
              Json{{"code", "assets_store_unknown@1"}, {"error_fields", {{"store", "nope"}}}}) ==
          "No asset store is named nope (assets_store_unknown@1).");
}

TEST_CASE("AV-1s decision 5: what the check found is a line under the tree and a mark on each "
          "file's row; not_run shows nothing",
          "[devstudio][assets-dock][av1s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = blankProject(temporary);
    write(root / "assets" / "textures" / "stray.ktx2", "stray");
    FakeEngine engine;
    AssetsDock dock{engine.hooks()};
    dock.show();
    dock.open(root);
    ready(dock, engine);
    // A project whose stores declare no manifest: no line, no band.
    CHECK(dock.verificationLine()->text().isEmpty());
    CHECK_FALSE(dock.verifyButton()->isVisible());

    const Json declared{{"main", {{"mount", "assets/"}, {"manifest", "assets.manifest.json"}}}};
    dock.engineAvailable();
    answer(dock, engine, "get_project_settings", settings("d0", nullptr, declared));
    answer(dock, engine, "get_status",
           status(nullptr, 0,
                  {{"state", "verified"}, {"stores", 1}, {"matched", 4}, {"info", 1},
                   {"warnings", 2}, {"errors", 0}}));
    const auto assets = root.generic_string() + "/assets";
    answer(dock, engine, "get_assets_verification",
           {{"state", "verified"},
            {"stores",
             Json::array({{{"name", "main"},
                           {"root", assets},
                           {"manifest", root.generic_string() + "/assets.manifest.json"},
                           {"matched", 4},
                           {"issues",
                            Json::array({{{"kind", "content_mismatch"}, {"severity", "info"},
                                          {"file", "textures/rock.ktx2"},
                                          {"message", "asset content differs: textures/rock.ktx2"}},
                                         {{"kind", "missing"}, {"severity", "warning"},
                                          {"file", "textures/gone.ktx2"},
                                          {"message", "asset is missing: textures/gone.ktx2"}},
                                         {{"kind", "extra"}, {"severity", "warning"},
                                          {"file", "textures/stray.ktx2"},
                                          {"message", "asset is not listed in the manifest: "
                                                      "textures/stray.ktx2"}}})}}})}});
    CHECK(dock.verificationLine()->text() ==
          QStringLiteral("3 files differ from the manifest: 1 content differs, 1 missing, 1 extra."));
    auto &tree = *dock.tree();
    QTreeWidgetItem *rock = row(tree, QStringLiteral("assets/textures/rock.ktx2"));
    QTreeWidgetItem *stray = row(tree, QStringLiteral("assets/textures/stray.ktx2"));
    REQUIRE(rock != nullptr);
    REQUIRE(stray != nullptr);
    CHECK(rock->text(2) == QStringLiteral("content differs"));
    CHECK(rock->toolTip(2) == QStringLiteral("asset content differs: textures/rock.ktx2"));
    CHECK(stray->text(2) == QStringLiteral("extra"));
    // ... and the folder that holds them is opened to show it.
    CHECK(row(tree, QStringLiteral("assets/textures"))->isExpanded());
    // A file the manifest lists and the disk does not has a grey row where it
    // would be, with no switch.
    QTreeWidgetItem *gone = row(tree, QStringLiteral("assets/textures/gone.ktx2"));
    REQUIRE(gone != nullptr);
    CHECK(gone->parent() == row(tree, QStringLiteral("assets/textures")));
    CHECK(gone->text(2) == QStringLiteral("missing"));
    CHECK_FALSE(gone->flags().testFlag(Qt::ItemIsUserCheckable));
    CHECK(row(tree, QStringLiteral("assets/asset_data.json"))->text(2).isEmpty());
}

TEST_CASE("AV-1s decision 6: Verify again and Update manifest wait while a check runs, and "
          "Update manifest says how many files change before it writes",
          "[devstudio][assets-dock][av1s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = blankProject(temporary);
    FakeEngine engine;
    AssetsDock dock{engine.hooks()};
    dock.show();
    dock.open(root);
    const Json declared{{"main", {{"mount", "assets/"}, {"manifest", "assets.manifest.json"}}}};
    const auto one_extra = Json{{"state", "verified"}, {"stores", 1}, {"matched", 4},
                                {"info", 0},          {"warnings", 1}, {"errors", 0}};
    const Json detail{
        {"state", "verified"},
        {"stores",
         Json::array({{{"name", "main"},
                       {"root", root.generic_string() + "/assets"},
                       {"manifest", root.generic_string() + "/assets.manifest.json"},
                       {"matched", 4},
                       {"issues", Json::array({{{"kind", "extra"}, {"severity", "warning"},
                                                {"file", "textures/rock.ktx2"},
                                                {"message", "not listed"}}})}}})}};
    ready(dock, engine, nullptr, declared, status(nullptr, 0, one_extra));
    answer(dock, engine, "get_assets_verification", detail);
    REQUIRE(dock.verifyButton()->isEnabled());
    auto *update = dock.findChild<QPushButton *>(QStringLiteral("pelican.assets.updateManifest.main"));
    REQUIRE(update != nullptr);
    CHECK(update->isEnabled());

    // Verify again: one request, and both wait until the check has ended.
    QTest::mouseClick(dock.verifyButton(), Qt::LeftButton);
    answer(dock, engine, "verify_assets", {{"state", "running"}});
    CHECK_FALSE(dock.verifyButton()->isEnabled());
    CHECK_FALSE(update->isEnabled());
    answer(dock, engine, "get_status", status(nullptr, 0, {{"state", "running"}}));
    CHECK_FALSE(dock.verifyButton()->isEnabled());
    for (int tick = 0; tick < 40 && !engine.asked("get_status"); ++tick) QTest::qWait(50);
    answer(dock, engine, "get_status", status(nullptr, 0, one_extra));
    // The same counts as before, read again all the same: the check ran.
    answer(dock, engine, "get_assets_verification", detail);
    CHECK(dock.verifyButton()->isEnabled());
    CHECK(update->isEnabled());

    // Update manifest asks first, with the count, and writes on Update.
    QTest::mouseClick(update, Qt::LeftButton);
    CHECK(engine.pending.empty());
    auto *confirm = dock.findChild<QLabel *>(QStringLiteral("pelican.assets.updateConfirm"));
    REQUIRE(confirm != nullptr);
    CHECK(confirm->isVisible());
    CHECK(confirm->text() == QStringLiteral("1 file will change in assets.manifest.json."));
    QTest::mouseClick(dock.findChild<QPushButton *>(QStringLiteral("pelican.assets.updateConfirmed")),
                      Qt::LeftButton);
    const auto request = engine.take("update_assets_manifest");
    REQUIRE(request.has_value());
    CHECK(request->params == Json{{"store", "main"}});
    CHECK_FALSE(confirm->isVisible());
    CHECK_FALSE(dock.verifyButton()->isEnabled());
    answer(dock, *request,
           {{"store", "main"}, {"manifest", "m"}, {"files", 5}, {"state", "running"}});
    answer(dock, engine, "get_status",
           status(nullptr, 0,
                  {{"state", "verified"}, {"stores", 1}, {"matched", 5}, {"info", 0},
                   {"warnings", 0}, {"errors", 0}}));
    answer(dock, engine, "get_assets_verification",
           {{"state", "verified"},
            {"stores", Json::array({{{"name", "main"},
                                     {"root", root.generic_string() + "/assets"},
                                     {"manifest", root.generic_string() + "/assets.manifest.json"},
                                     {"matched", 5},
                                     {"issues", Json::array()}}})}});
    CHECK(dock.verificationLine()->text() ==
          QStringLiteral("Asset contents match the manifest (5 files)."));
    CHECK(dock.verifyButton()->isEnabled());
}

TEST_CASE("HW-2s decision 7: with the engine's own watch open, a reload_shaders that found "
          "nothing new is followed as the watcher's commit",
          "[devstudio][assets-dock][hw2s][shader-reload]") {
    application();
    std::vector<qint64> reads;
    std::vector<qint64> reloads;
    std::vector<ShaderReloadOutcome> outcomes;
    ShaderReloadFollower follower{ShaderReloadFollower::Hooks{
        .request_status =
            [&](QString *) {
                reads.push_back(static_cast<qint64>(reads.size()) + 9000);
                return reads.back();
            },
        .apply = [](const ShaderReloadMarks &, const ShaderReloadReading &, bool) {},
        .request_reload =
            [&](QString *) {
                reloads.push_back(static_cast<qint64>(reloads.size()) + 7000);
                return reloads.back();
            },
        .reloaded = [&](const ShaderReloadOutcome &outcome) { outcomes.push_back(outcome); },
    }};
    const auto reading = [](bool enabled, const char *reason) {
        return readShaderReloadStatus(
            Json{{"reload",
                  {{"runtime",
                    {{"pelican.shaders",
                      {{"attempted", 0}, {"applied", 0}, {"failed", 0}, {"last_error", nullptr},
                       {"details", {{"enabled", enabled}, {"gate_reason", reason}}}}}}}}}});
    };
    const auto write_and_answer_nothing_new = [&] {
        follower.sourceChanged();
        QTest::qWait(ShaderReloadFollower::ReloadDebounceMs + 150);
        REQUIRE_FALSE(reloads.empty());
        REQUIRE(follower.receive(reloads.back(), QByteArray{R"({"committed":false,"attempted":false})"}));
        REQUIRE_FALSE(outcomes.empty());
        return outcomes.back().kind;
    };

    // Studio's engine without hot_reload: its gate is shut ("rpc driver"), the
    // request did the compile, and nothing new is nothing new.
    follower.observeStatus(reading(false, "rpc driver"));
    CHECK(write_and_answer_nothing_new() == ShaderReloadOutcome::Kind::unchanged);
    QTest::qWait(50);
    CHECK(reads.empty());

    // The Assets dock switched the watch on: a get_status any panel read says
    // the gate is open, and the watcher's compile is followed as a commit --
    // the plan and the sockets are read again, and the status once for marks.
    follower.observeStatus(reading(true, ""));
    CHECK(write_and_answer_nothing_new() == ShaderReloadOutcome::Kind::committed);
    QTest::qWait(50);
    CHECK(reads.size() == 1);
}

TEST_CASE("MI-1s decision 4: a registered model's row offers Remove from project..., which asks first -- "
          "saying its file stays, and that its material assignments go with it when it has some -- and "
          "says how unregister_model went in one sentence",
          "[devstudio][assets-dock][mi1s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = blankProject(temporary);
    write(root / "assets" / "models" / "box.glb", "glb");
    write(root / "assets" / "models" / "loose.glb", "glb");
    FakeEngine engine;
    auto hooks = engine.hooks();
    std::vector<std::string> asked_bindings;
    std::function<void(bool)> bindings_answer;
    std::vector<std::string> removed;
    std::function<void(const std::string &)> removal_said;
    hooks.model_at = [](const std::string &logical) -> std::optional<std::string> {
        if (logical == "assets/models/box.glb") return std::string{"box"};
        return std::nullopt;
    };
    hooks.model_bindings = [&](const std::string &model, std::function<void(bool)> answered) {
        asked_bindings.push_back(model);
        bindings_answer = std::move(answered);
    };
    hooks.remove_model = [&](const std::string &model, std::function<void(const std::string &)> said) {
        removed.push_back(model);
        removal_said = std::move(said);
    };
    AssetsDock dock{hooks};
    dock.show();
    dock.open(root);
    ready(dock, engine);
    auto &tree = *dock.tree();
    QAction *remove = dock.removeModelAction();
    REQUIRE(remove != nullptr);
    CHECK(remove->objectName() == QStringLiteral("pelican.assets.removeModel"));
    auto *confirm = dock.findChild<QLabel *>(QStringLiteral("pelican.assets.removeModelConfirm"));
    auto *remove_button = dock.findChild<QPushButton *>(QStringLiteral("pelican.assets.removeModelConfirmed"));
    auto *cancel_button = dock.findChild<QPushButton *>(QStringLiteral("pelican.assets.removeModelCancel"));
    REQUIRE(confirm != nullptr);
    REQUIRE(remove_button != nullptr);
    REQUIRE(cancel_button != nullptr);

    // A file no model is registered from: nothing to remove.
    tree.setCurrentItem(row(tree, QStringLiteral("assets/models/loose.glb")));
    CHECK_FALSE(remove->isEnabled());
    tree.setCurrentItem(row(tree, QStringLiteral("assets/models/box.glb")));
    REQUIRE(remove->isEnabled());

    // Asked first; the bindings are read before the question is put.
    remove->trigger();
    REQUIRE(asked_bindings == std::vector<std::string>{"box"});
    CHECK_FALSE(confirm->isVisibleTo(&dock));
    bindings_answer(false);
    REQUIRE(confirm->isVisibleTo(&dock));
    CHECK(confirm->text() == QStringLiteral("Remove model box from the project? assets/models/box.glb stays on disk."));
    cancel_button->click();
    CHECK_FALSE(confirm->isVisibleTo(&dock));
    CHECK(removed.empty());

    // With material bindings, the question says they go too, for good.
    remove->trigger();
    bindings_answer(true);
    CHECK(confirm->text() ==
          QStringLiteral("Remove model box from the project? assets/models/box.glb stays on disk. Its material "
                         "assignments are removed with it and cannot be put back."));
    remove_button->click();
    REQUIRE(removed == std::vector<std::string>{"box"});
    CHECK_FALSE(confirm->isVisibleTo(&dock));
    // What the engine answered is the dock's one line.
    removal_said("box is used by 1 object: Crate (model.in_use@1).");
    CHECK(dock.noticeLine()->isVisibleTo(&dock));
    CHECK(dock.noticeLine()->toolTip() == QStringLiteral("box is used by 1 object: Crate (model.in_use@1)."));

    // The window finds a row's model from list_assets, whose path is read
    // as the project names it (EB-1: relative; an absolute one still reads).
    CHECK(projectRelativeAssetPath(root, (root / "assets" / "models" / "box.glb").string()) ==
          "assets/models/box.glb");
    CHECK(projectRelativeAssetPath(root, "project://assets/models/box.glb") == "assets/models/box.glb");
    CHECK(projectRelativeAssetPath(root, "assets/models/box.glb") == "assets/models/box.glb");
    CHECK(projectRelativeAssetPath(root, "engine://primitives/cube").empty());
    CHECK(projectRelativeAssetPath(root, (root.parent_path() / "elsewhere.glb").string()).empty());
}

TEST_CASE("GL-5e decision 9: list_assets' project-relative path (engine EB-1) finds a registered model, "
          "and its material rows are not models") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const std::filesystem::path root = directory.path().toStdWString();
    // EB-1's shape: `path` as asset_data spells it, `resolved_path` the file.
    const auto resolved = [&](const char *relative) {
        return (root / std::filesystem::path{relative}).generic_string();
    };
    const Json result{
        {"assets",
         Json::array({
             {{"id", "engine://primitives/cube"}, {"kind", "model"}, {"path", "engine://primitives/cube"},
              {"store", "engine"}, {"status", "builtin"}},
             {{"id", "checker_box"}, {"kind", "model"}, {"path", "assets/models/checker_box.glb"},
              {"store", "project"}, {"status", "loaded"},
              {"resolved_path", resolved("assets/models/checker_box.glb")}},
             {{"id", "crate"}, {"kind", "model"}, {"path", "project://assets/models/crate.glb"},
              {"store", "project"}, {"status", "loaded"}, {"resolved_path", resolved("assets/models/crate.glb")}},
             {{"id", "paint"}, {"kind", "material"}, {"path", "materials/paint.material.json"},
              {"store", "project"}, {"status", "loaded"},
              {"resolved_path", resolved("materials/paint.material.json")}},
             {{"id", "materials/gone.material.json"}, {"kind", "material"},
              {"path", "materials/gone.material.json"}, {"store", "project"}, {"status", "missing"}},
         })}};
    CHECK(projectRelativeAssetPath(root, "assets/models/checker_box.glb") == "assets/models/checker_box.glb");
    const auto files = registeredModelFiles(root, result);
    CHECK(files == std::map<std::string, std::string>{{"assets/models/checker_box.glb", "checker_box"},
                                                      {"assets/models/crate.glb", "crate"}});
    CHECK(registeredModelFiles(root, Json::object()).empty());
}
