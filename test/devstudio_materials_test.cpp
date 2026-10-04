// MT-1s: the material editor -- the Materials dock (list_materials, the
// editor made from get_surface_schema, New material… on list_surfaces,
// Assign to selected object, Undo) and the Inspector's Material rows
// (get_material_bindings, bind_material) -- on the real widgets, with the
// test answering for the engine (MT-1, and MT-1b's list_surfaces) through
// the widgets' own hooks.

#include "../src/devstudio/model/inspectormodel.hpp"
#include "../src/devstudio/model/materialsmodel.hpp"
#include "../src/devstudio/model/problemsmodel.hpp"
#include "graphcanvastagpicker.hpp"
#include "materialassignwidget.hpp"
#include "materialsdock.hpp"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextCursor>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace PelicanStudio;
using Json = nlohmann::json;

namespace {

QApplication &application() {
    static int argc = 1;
    static char name[] = "devstudio_materials_test";
    static char *argv[] = {name, nullptr};
    static QApplication app{argc, argv};
    return app;
}

constexpr const char *OpenPbr = "engine://surfaces/openpbr/opaque_single.surface";
constexpr const char *Paint = "materials/paint.material.json";
constexpr const char *Cube = "engine://primitives/cube";

void write(const std::filesystem::path &path, const std::string &bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << bytes;
}

// A KTX2 header: identifier, then vkFormat, typeSize, width, height, depth,
// layerCount, faceCount.
std::string ktx2(std::uint32_t faces) {
    std::string bytes{"\xABKTX 20\xBB\r\n\x1A\n", 12};
    for (const std::uint32_t word : {109u, 1u, 4u, 4u, 0u, 0u, faces, 1u})
        for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>((word >> shift) & 0xFF));
    return bytes;
}

std::filesystem::path project(const QTemporaryDir &temporary) {
    const std::filesystem::path root{temporary.path().toStdWString()};
    write(root / "project.json", R"({"schema":"pelican.project","version":1,"name":"materials"})");
    write(root / "assets" / "textures" / "rock.png", "png");
    write(root / "assets" / "textures" / "sky.ktx2", ktx2(6));
    write(root / "assets" / "textures" / "notes.txt", "not an image");
    return root;
}

struct FakeEngine {
    struct Request {
        qint64 id = 0;
        std::string method;
        Json params;
    };
    std::vector<Request> pending;
    qint64 next = 100;
    std::vector<QString> restarts;
    std::optional<MaterialsDock::SelectedObject> selected;
    std::vector<std::string> bound;
    int materials_changed = 0;
    // MT-2s: the set_object_material requests handed to the Inspector's
    // editor session, and whether it takes the next one.
    struct ObjectEdit {
        Json params;
        std::string description;
    };
    std::vector<ObjectEdit> object_edits;
    bool session_takes_edits = true;

    std::function<qint64(const QString &, const QJsonObject &, QString *)> sender() {
        return [this](const QString &method, const QJsonObject &params, QString *) {
            pending.push_back({++next, method.toStdString(),
                               Json::parse(QJsonDocument(params).toJson(QJsonDocument::Compact).toStdString())});
            return next;
        };
    }

    MaterialsDock::Hooks dockHooks() {
        return MaterialsDock::Hooks{
            .send = sender(),
            .restart = [this](const QString &reason) { restarts.push_back(reason); },
            .selected_object = [this] { return selected; },
            .materials_changed = [this] { ++materials_changed; },
            .bound = [this](const std::string &model) { bound.push_back(model); },
        };
    }

    MaterialAssignWidget::Hooks assignHooks() {
        return MaterialAssignWidget::Hooks{
            .send = sender(),
            .restart = [this](const QString &reason) { restarts.push_back(reason); },
            .edit_object =
                [this](const QJsonObject &params, const QString &description, QString *error) {
                    if (!session_takes_edits) {
                        if (error != nullptr) *error = QStringLiteral("Finish the edit in progress first.");
                        return false;
                    }
                    object_edits.push_back(
                        {Json::parse(QJsonDocument(params).toJson(QJsonDocument::Compact).toStdString()),
                         description.toStdString()});
                    return true;
                },
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

template <typename Widget>
FakeEngine::Request answer(Widget &widget, FakeEngine &engine, const std::string &method, const Json &result) {
    INFO("answering " << method);
    const auto request = engine.take(method);
    REQUIRE(request.has_value());
    REQUIRE(widget.receiveResult(request->id, QByteArray::fromStdString(result.dump())));
    return *request;
}

template <typename Widget>
FakeEngine::Request refuse(Widget &widget, FakeEngine &engine, const std::string &method, const std::string &code,
                           const QJsonObject &fields, const QString &message = QStringLiteral("refused")) {
    const auto request = engine.take(method);
    REQUIRE(request.has_value());
    REQUIRE(widget.receiveRefusal(request->id, -32000, message,
                                  QJsonObject{{QStringLiteral("error_code"), QString::fromStdString(code)},
                                              {QStringLiteral("error_fields"), fields}}));
    return *request;
}

Json listResult(const std::string &digest = "d1", Json extra = Json::array()) {
    Json documents = Json::array({
        {{"path", Paint},
         {"material_digest", digest},
         {"status", "loaded"},
         {"materials",
          Json::array({{{"name", "paint"},
                        {"surface", OpenPbr},
                        {"tags", Json::array({"shiny"})},
                        {"id", "paint@materials/paint.material.json"},
                        {"registered", true}}})}},
        {{"path", "materials/gone.material.json"},
         {"material_digest", nullptr},
         {"status", "missing"},
         {"reason", "cannot resolve materials/gone.material.json"},
         {"materials", Json::array()}},
    });
    for (const auto &document : extra) documents.push_back(document);
    return Json{{"asset_data_digest", "a1"}, {"documents", documents}};
}

Json paintMaterial(const std::string &digest = "d1", Json values = Json{{"base_color", {1.0, 0.0, 0.0, 1.0}}}) {
    return Json{{"path", Paint},
                {"material_digest", digest},
                {"name", "paint"},
                {"names", Json::array({"paint"})},
                {"registered", true},
                {"material",
                 {{"name", "paint"},
                  {"tags", Json::array({"shiny"})},
                  {"surface", OpenPbr},
                  {"values", values},
                  {"routing", {{"alpha_mode", "opaque"}, {"double_sided", false}}}}}};
}

Json schema() {
    return Json{
        {"surface", OpenPbr},
        {"params",
         Json::array({
             {{"name", "base_color"},
              {"type", "color"},
              {"default", {0.8, 0.8, 0.8, 1.0}},
              {"min", 0.0},
              {"max", 1.0},
              {"encoding", "linear"}},
             {{"name", "specular_roughness"}, {"type", "float"}, {"default", 0.3}, {"min", 0.0}, {"max", 1.0},
              {"encoding", "linear"}},
             {{"name", "specular_ior"}, {"type", "float"}, {"default", 1.5}, {"min", 0.0}, {"encoding", "linear"}},
             {{"name", "layers"}, {"type", "int"}, {"default", 2}, {"encoding", "linear"}},
         })},
        {"textures",
         Json::array({
             {{"name", "base_color_map"},
              {"default", "engine://textures/white"},
              {"color_space", "srgb"},
              {"role", "color"},
              {"dimension", "2d"}},
             {{"name", "environment_map"},
              {"default", "engine://textures/black_cube"},
              {"color_space", "linear"},
              {"role", "data"},
              {"dimension", "cube"}},
         })},
    };
}

Json surfaces() {
    return Json{{"surfaces",
                 Json::array({
                     {{"reference", "project://shaders/broken.surface"},
                      {"source", "project"},
                      {"params", 0},
                      {"textures", 0},
                      {"error", "line 1: not a pelican.surface header"}},
                     {{"reference", OpenPbr},
                      {"source", "engine"},
                      {"name", "opaque_single_sided"},
                      {"params", 21},
                      {"textures", 19},
                      {"routing", {{"alpha_mode", "opaque"}, {"double_sided", false}}}},
                 })}};
}

Json edited(const std::string &digest, Json previous, Json restart = nullptr) {
    return Json{{"committed", true},
                {"changed", true},
                {"path", Paint},
                {"name", "paint"},
                {"material_digest", digest},
                {"previous_material_digest", "d1"},
                {"previous", previous},
                {"applied", restart.is_null() ? "live" : "restart_required"},
                {"restart_required", restart}};
}

template <typename T>
T *named(QWidget &root, const QString &name) {
    auto *found = root.findChild<T *>(name);
    REQUIRE(found != nullptr);
    return found;
}

QTreeWidgetItem *rowNamed(QTreeWidget &tree, const QString &text) {
    const auto items = tree.findItems(text, Qt::MatchExactly | Qt::MatchRecursive);
    return items.isEmpty() ? nullptr : items.front();
}

// MT-2s: the Inspector's selected object, read at `revision`.
MaterialAssignSession::Subject subjectOf(const std::string &model, const std::string &name,
                                         std::uint64_t object = 7, std::uint64_t revision = 1) {
    return {.model = model, .name = name, .object = object, .scene_id = "default_scene", .revision = revision};
}

Json cubeBindings(std::size_t objects = 2, std::size_t overrides = 0) {
    return Json{{"model", Cube},
                {"material_bindings", nullptr},
                {"bindings_digest", nullptr},
                {"primitives",
                 Json::array({{{"mesh", 0},
                               {"primitive", 0},
                               {"slot", "pelican.primitive.default"},
                               {"material", "pelican.primitive.default"},
                               {"usd_path", nullptr}}})},
                {"objects", objects},
                {"object_overrides", overrides}};
}

// get_object_materials of the cube: `own` its all-primitive row, or none.
Json cubeObject(const std::string &own = {}, bool resolved = true) {
    auto rows = Json::array();
    if (!own.empty()) rows.push_back({{"row_id", 1}, {"material", own}, {"resolved", resolved}});
    const bool applies = !own.empty() && resolved;
    return Json{{"object", 7},
                {"scene_id", "default_scene"},
                {"model", Cube},
                {"materials", rows},
                {"primitives",
                 Json::array({{{"mesh", 0},
                               {"primitive", 0},
                               {"slot", "pelican.primitive.default"},
                               {"material", applies ? own : "pelican.primitive.default"},
                               {"source", applies ? "object" : "model"},
                               {"model_material", "pelican.primitive.default"},
                               {"drawn", applies ? own : "pelican.primitive.default"}}})}};
}

// The Inspector's rows on the cube, everything read.
void readyCube(MaterialAssignWidget &rows, FakeEngine &engine, const Json &object = cubeObject(),
               const Json &bindings = cubeBindings()) {
    rows.engineAvailable();
    rows.setSubject(subjectOf(Cube, "Cube"));
    answer(rows, engine, "list_materials", listResult());
    answer(rows, engine, "get_material_bindings", bindings);
    const auto read = answer(rows, engine, "get_object_materials", object);
    CHECK(read.params == Json{{"object", 7}, {"scene_id", "default_scene"}});
}

// The dock on a project, the engine up, the list read and paint selected,
// its material and its surface's schema read.
void readyWithPaint(MaterialsDock &dock, FakeEngine &engine, const std::filesystem::path &root) {
    dock.open(root);
    dock.engineAvailable();
    answer(dock, engine, "list_materials", listResult());
    answer(dock, engine, "list_surfaces", surfaces());
    QTreeWidgetItem *paint = rowNamed(*dock.list(), QStringLiteral("paint"));
    REQUIRE(paint != nullptr);
    dock.list()->setCurrentItem(paint);
    const auto read = answer(dock, engine, "get_material", paintMaterial());
    CHECK(read.params == Json{{"path", Paint}, {"name", "paint"}});
    const auto surface = answer(dock, engine, "get_surface_schema", schema());
    CHECK(surface.params == Json{{"surface", OpenPbr}});
}

} // namespace

TEST_CASE("MT-1s decision 1: the Materials dock lists the documents and their materials, a missing one "
          "greyed with its reason, and a poll whose digests are the same changes nothing",
          "[devstudio][materials][mt1s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = project(temporary);
    FakeEngine engine;
    MaterialsDock dock{engine.dockHooks()};
    readyWithPaint(dock, engine, root);
    auto &tree = *dock.list();
    REQUIRE(tree.topLevelItemCount() == 2);
    QTreeWidgetItem *document = tree.topLevelItem(0);
    CHECK(document->text(0) == QStringLiteral("paint.material.json"));
    REQUIRE(document->childCount() == 1);
    CHECK(document->child(0)->text(0) == QStringLiteral("paint"));
    CHECK(document->child(0)->text(1) == QString::fromLatin1(OpenPbr));
    QTreeWidgetItem *missing = tree.topLevelItem(1);
    CHECK(missing->text(1).startsWith(QStringLiteral("missing: cannot resolve")));
    CHECK_FALSE(missing->flags().testFlag(Qt::ItemIsSelectable));
    CHECK(missing->foreground(0).color() == dock.palette().color(QPalette::Disabled, QPalette::Text));
    CHECK(engine.materials_changed == 1);

    // The poll: the same digests change nothing -- no row is made again, the
    // material is not read again.
    QTreeWidgetItem *before = tree.topLevelItem(0);
    dock.session().poll();
    answer(dock, engine, "list_materials", listResult());
    CHECK(tree.topLevelItem(0) == before);
    CHECK_FALSE(engine.asked("get_material"));
    CHECK(engine.materials_changed == 1);

    // Written outside Studio: the shown material is read again.
    dock.session().poll();
    answer(dock, engine, "list_materials", listResult("d9"));
    CHECK(engine.asked("get_material"));
    CHECK(engine.materials_changed == 2);
    CHECK(dock.session().digestOf(Paint) == "d9");
}

TEST_CASE("MT-1s decision 2: the editor's rows are made from the surface's params -- a slider and a "
          "spin, a swatch, a spin, a texture combo by dimension -- and each change is one edit_material",
          "[devstudio][materials][mt1s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = project(temporary);
    FakeEngine engine;
    MaterialsDock dock{engine.dockHooks()};
    readyWithPaint(dock, engine, root);

    // float with min and max: a slider and a spin; without: a spin alone;
    // color: a swatch (shown in sRGB); int: a spin.
    auto *slider = named<QSlider>(dock, QStringLiteral("pelican.materials.param.specular_roughness.slider"));
    auto *roughness = named<QDoubleSpinBox>(dock, QStringLiteral("pelican.materials.param.specular_roughness.spin"));
    CHECK(roughness->value() == 0.3);
    CHECK(dock.findChild<QSlider *>(QStringLiteral("pelican.materials.param.specular_ior.slider")) == nullptr);
    named<QDoubleSpinBox>(dock, QStringLiteral("pelican.materials.param.specular_ior.spin"));
    CHECK(named<QSpinBox>(dock, QStringLiteral("pelican.materials.param.layers.spin"))->value() == 2);
    auto *swatch = named<QPushButton>(dock, QStringLiteral("pelican.materials.param.base_color.swatch"));
    CHECK(swatch->text() == QStringLiteral("#ff0000"));
    // What differs from the default is bold.
    CHECK(named<QLabel>(dock, QStringLiteral("pelican.materials.param.base_color.label"))->font().bold());
    CHECK_FALSE(named<QLabel>(dock, QStringLiteral("pelican.materials.param.specular_roughness.label"))->font().bold());

    // A spin's value, confirmed: one edit_material against the digest held.
    roughness->setValue(0.8);
    auto sent = engine.take("edit_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"path", Paint},
                               {"name", "paint"},
                               {"base_material_digest", "d1"},
                               {"values", {{"specular_roughness", 0.8}}}});
    REQUIRE(dock.receiveResult(sent->id, QByteArray::fromStdString(
                                             edited("d2", {{"values", {{"specular_roughness", nullptr}}}}).dump())));
    answer(dock, engine, "get_material",
           paintMaterial("d2", {{"base_color", {1.0, 0.0, 0.0, 1.0}}, {"specular_roughness", 0.8}}));
    answer(dock, engine, "list_materials", listResult("d2"));

    // A drag sends nothing until the slider is let go, then once.
    slider->setSliderDown(true);
    slider->setValue(250);
    CHECK_FALSE(engine.asked("edit_material"));
    slider->setSliderDown(false);
    sent = engine.take("edit_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params.at("values") == Json{{"specular_roughness", 0.25}});
    CHECK(sent->params.at("base_material_digest") == "d2");
    CHECK_FALSE(engine.asked("edit_material"));
    REQUIRE(dock.receiveResult(sent->id, QByteArray::fromStdString(
                                             edited("d3", {{"values", {{"specular_roughness", 0.8}}}}).dump())));
    answer(dock, engine, "get_material",
           paintMaterial("d3", {{"base_color", {1.0, 0.0, 0.0, 1.0}}, {"specular_roughness", 0.25}}));

    // Reset to default: the value taken away.
    auto *reset = dock.findChild<QAction *>(QStringLiteral("pelican.materials.param.base_color.reset"));
    REQUIRE(reset != nullptr);
    CHECK(reset->isEnabled());
    reset->trigger();
    sent = engine.take("edit_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params.at("values") == Json{{"base_color", nullptr}});
    REQUIRE(dock.receiveResult(sent->id, QByteArray::fromStdString(
                                             edited("d4", {{"values", {{"base_color", {1.0, 0.0, 0.0, 1.0}}}}}).dump())));
    answer(dock, engine, "get_material", paintMaterial("d4", {{"specular_roughness", 0.25}}));
    // The default shows, in sRGB: 0.8 linear is #e7e7e7.
    CHECK(swatch->text() == QStringLiteral("#e7e7e7"));

    // The swatch opens the colour dialog, named: its hex field and OK.
    swatch->click();
    auto *dialog = dock.findChild<QColorDialog *>(QStringLiteral("pelican.materials.colorDialog"));
    REQUIRE(dialog != nullptr);
    auto *hex = named<QLineEdit>(*dialog, QStringLiteral("pelican.materials.colorDialog.hex"));
    hex->selectAll();
    QTest::keyClicks(hex, QStringLiteral("#00ff00"));
    named<QPushButton>(*dialog, QStringLiteral("pelican.materials.colorDialog.ok"))->click();
    sent = engine.take("edit_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params.at("values") == Json{{"base_color", {0.0, 1.0, 0.0, 1.0}}});

    // A texture slot lists the project's images of its dimension, and None.
    auto *color_map = named<QComboBox>(dock, QStringLiteral("pelican.materials.texture.base_color_map"));
    REQUIRE(color_map->count() == 2);
    CHECK(color_map->itemText(0).startsWith(QStringLiteral("None")));
    CHECK(color_map->itemData(1).toString() == QStringLiteral("project://assets/textures/rock.png"));
    auto *environment = named<QComboBox>(dock, QStringLiteral("pelican.materials.texture.environment_map"));
    REQUIRE(environment->count() == 2);
    CHECK(environment->itemData(1).toString() == QStringLiteral("project://assets/textures/sky.ktx2"));
    CHECK(environment->itemData(0, Qt::ToolTipRole).isNull());
    CHECK(environment->toolTip().contains(QStringLiteral("cube")));
}

TEST_CASE("MT-1s decision 3: tags, routing and surface are one edit each, and restart_required restarts "
          "the engine as every other panel's edit does",
          "[devstudio][materials][mt1s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = project(temporary);
    FakeEngine engine;
    MaterialsDock dock{engine.dockHooks()};
    readyWithPaint(dock, engine, root);

    // The tags are UX-11's picker, with the tags the materials carry.
    auto *tags = static_cast<GraphCanvasTagPicker *>(named<QToolButton>(dock, QStringLiteral("pelican.materials.tags")));
    CHECK(tags->selected() == std::vector<std::string>{"shiny"});
    REQUIRE(tags->addTag("glass"));
    auto sent = engine.take("edit_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params.at("tags") == Json::array({"shiny", "glass"}));
    const Json reason{{"reason", "tags change how material 'paint' is registered"}};
    REQUIRE(dock.receiveResult(sent->id,
                               QByteArray::fromStdString(edited("d2", {{"tags", {"shiny"}}}, reason).dump())));
    REQUIRE(engine.restarts.size() == 1);
    CHECK(engine.restarts.back().contains(QStringLiteral("tags change")));
    CHECK(dock.noticeLine()->text().startsWith(QStringLiteral("Restart the engine to apply")));
    CHECK(named<QPushButton>(dock, QStringLiteral("pelican.materials.restartEngine"))->isVisibleTo(&dock));
    answer(dock, engine, "get_material", paintMaterial("d2"));

    // Routing: the alpha mode's combo and the double-sided check.
    named<QComboBox>(dock, QStringLiteral("pelican.materials.alphaMode"))->setCurrentIndex(2);
    sent = engine.take("edit_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params.at("routing") == Json{{"alpha_mode", "blend"}, {"double_sided", false}});
    REQUIRE(dock.receiveResult(sent->id, QByteArray::fromStdString(
                                             edited("d3", {{"routing", nullptr}}, reason).dump())));
    CHECK(engine.restarts.size() == 2);
    answer(dock, engine, "get_material", paintMaterial("d3"));

    // The surface combo lists the engine's surfaces; one that cannot be read
    // cannot be chosen.
    auto *surface = named<QComboBox>(dock, QStringLiteral("pelican.materials.surface"));
    REQUIRE(surface->count() == 2);
    CHECK(surface->currentText() == QStringLiteral("openpbr / opaque_single_sided"));
    auto *model = qobject_cast<QStandardItemModel *>(surface->model());
    REQUIRE(model != nullptr);
    CHECK_FALSE(model->item(0)->isEnabled());
    CHECK(surface->itemData(0, Qt::ToolTipRole).toString().contains(QStringLiteral("not a pelican.surface")));
}

TEST_CASE("MT-1s decision 4: New material… offers the engine's surfaces, proposes a free name and its "
          "file, creates it and selects it",
          "[devstudio][materials][mt1s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = project(temporary);
    // A file is already where "material" would go.
    write(root / "materials" / "material.material.json", "{}");
    FakeEngine engine;
    MaterialsDock dock{engine.dockHooks()};
    dock.open(root);
    dock.engineAvailable();
    answer(dock, engine, "list_materials", listResult());
    answer(dock, engine, "list_surfaces", surfaces());

    named<QPushButton>(dock, QStringLiteral("pelican.materials.newMaterial"))->click();
    if (engine.asked("list_surfaces")) answer(dock, engine, "list_surfaces", surfaces());
    auto *surface = named<QComboBox>(dock, QStringLiteral("pelican.materials.new.surface"));
    auto *name = named<QLineEdit>(dock, QStringLiteral("pelican.materials.new.name"));
    REQUIRE(surface->count() == 2);
    // The engine's list, in its order; the broken one greyed with its reason.
    CHECK(surface->itemText(0) == QStringLiteral("shaders/broken"));
    CHECK_FALSE(qobject_cast<QStandardItemModel *>(surface->model())->item(0)->isEnabled());
    CHECK(surface->currentText() == QStringLiteral("openpbr / opaque_single_sided"));
    CHECK(name->text() == QStringLiteral("material_2"));
    CHECK(named<QLabel>(dock, QStringLiteral("pelican.materials.new.path"))->text() ==
          QStringLiteral("materials/material_2.material.json"));
    auto *create = named<QPushButton>(dock, QStringLiteral("pelican.materials.new.create"));
    CHECK(create->isEnabled());
    name->setText(QStringLiteral("paint"));
    CHECK_FALSE(create->isEnabled());
    name->setText(QStringLiteral("material_2"));

    create->click();
    const auto sent = engine.take("create_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"path", "materials/material_2.material.json"},
                               {"surface", OpenPbr},
                               {"name", "material_2"},
                               {"routing", {{"alpha_mode", "opaque"}, {"double_sided", false}}}});
    REQUIRE(dock.receiveResult(sent->id, QByteArray::fromStdString(Json{{"committed", true},
                                                                        {"changed", true},
                                                                        {"path", "materials/material_2.material.json"},
                                                                        {"name", "material_2"},
                                                                        {"material_digest", "n1"},
                                                                        {"asset_data_digest", "a2"},
                                                                        {"previous_asset_data_digest", "a1"},
                                                                        {"applied", "live"},
                                                                        {"restart_required", nullptr}}
                                                                       .dump())));
    // Not undoable: there is no delete_material.
    CHECK(dock.session().undoEntries().empty());
    answer(dock, engine, "list_materials",
           listResult("d1", Json::array({{{"path", "materials/material_2.material.json"},
                                          {"material_digest", "n1"},
                                          {"status", "loaded"},
                                          {"materials",
                                           Json::array({{{"name", "material_2"},
                                                         {"surface", OpenPbr},
                                                         {"tags", Json::array()},
                                                         {"id", "material_2@x"},
                                                         {"registered", true}}})}}})));
    const auto read = engine.take("get_material");
    REQUIRE(read.has_value());
    CHECK(read->params == Json{{"path", "materials/material_2.material.json"}, {"name", "material_2"}});
    REQUIRE(dock.list()->currentItem() != nullptr);
    CHECK(dock.list()->currentItem()->text(0) == QStringLiteral("material_2"));

    // An engine without list_surfaces: nothing can be made, and it says so.
    FakeEngine old_engine;
    MaterialsDock old_dock{old_engine.dockHooks()};
    old_dock.open(root);
    old_dock.engineAvailable();
    const auto asked = old_engine.take("list_surfaces");
    REQUIRE(asked.has_value());
    REQUIRE(old_dock.receiveRefusal(asked->id, -32601, QStringLiteral("Method not found"), QJsonValue{}));
    named<QPushButton>(old_dock, QStringLiteral("pelican.materials.newMaterial"))->click();
    CHECK(named<QLabel>(old_dock, QStringLiteral("pelican.materials.new.notice"))
              ->text()
              .contains(QStringLiteral("list_surfaces")));
    CHECK_FALSE(named<QPushButton>(old_dock, QStringLiteral("pelican.materials.new.create"))->isEnabled());
}

TEST_CASE("MT-1s decision 5: the Inspector's Material combo binds every primitive of the model, says it is "
          "the model's, has a row per primitive when there are several; the dock assigns to the selected object",
          "[devstudio][materials][mt1s]") {
    application();
    FakeEngine engine;
    MaterialAssignWidget rows{engine.assignHooks()};
    CHECK(rows.isHidden());
    readyCube(rows, engine);
    // MT-2s: the model's bind is Every Cube.
    named<QComboBox>(rows, QStringLiteral("pelican.inspectorMaterialTarget"))->setCurrentIndex(1);
    CHECK_FALSE(rows.isHidden());
    auto *choice = named<QComboBox>(rows, QStringLiteral("pelican.inspectorMaterialChoice"));
    CHECK(choice->currentText() == QStringLiteral("Model default"));
    CHECK(named<QLabel>(rows, QStringLiteral("pelican.inspectorMaterialScope"))->text() ==
          QStringLiteral("Applies to every Cube in this project (2 objects, 0 with their own material)."));
    CHECK(rows.findChild<QWidget *>(QStringLiteral("pelican.inspectorMaterialPrimitives"))->isHidden());

    choice->setCurrentIndex(choice->findText(QStringLiteral("paint")));
    auto sent = engine.take("bind_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"model", Cube}, {"material", "paint"}});

    // A model of two primitives: a combo each, which binds that one.
    FakeEngine ship_engine;
    MaterialAssignWidget ship{ship_engine.assignHooks()};
    ship.engineAvailable();
    ship.setSubject(subjectOf("ship", "Ship"));
    ship.session().setScope(MaterialAssignSession::Scope::Model);
    answer(ship, ship_engine, "list_materials", listResult());
    answer(ship, ship_engine, "get_material_bindings",
           Json{{"model", "ship"},
                {"primitives", Json::array({{{"mesh", 0}, {"primitive", 0}, {"slot", "hull"}, {"material", "hull"}},
                                            {{"mesh", 1}, {"primitive", 0}, {"slot", "sail"}, {"material", "paint"}}})}});
    CHECK(named<QComboBox>(ship, QStringLiteral("pelican.inspectorMaterialChoice"))->currentText() ==
          QStringLiteral("(differs per primitive)"));
    auto *second = named<QComboBox>(ship, QStringLiteral("pelican.inspectorMaterialPrimitive.1"));
    CHECK(second->currentText() == QStringLiteral("paint"));
    second->setCurrentIndex(0);
    sent = ship_engine.take("bind_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"model", "ship"},
                               {"bindings", Json::array({{{"mesh", 1}, {"primitive", 0}, {"material", "sail"}}})}});

    // The dock's Assign to selected object.
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    FakeEngine dock_engine;
    MaterialsDock dock{dock_engine.dockHooks()};
    readyWithPaint(dock, dock_engine, project(temporary));
    dock_engine.selected = MaterialsDock::SelectedObject{Cube, "Cube"};
    named<QPushButton>(dock, QStringLiteral("pelican.materials.assignSelected"))->click();
    sent = dock_engine.take("bind_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"model", Cube}, {"material", "paint"}});
    REQUIRE(dock.receiveResult(
        sent->id,
        QByteArray::fromStdString(
            Json{{"committed", true},
                 {"changed", true},
                 {"model", Cube},
                 {"previous",
                  {{"bindings", Json::array({{{"mesh", 0}, {"primitive", 0}, {"material", "pelican.primitive.default"}}})}}},
                 {"applied", "live"},
                 {"restart_required", nullptr}}
                .dump())));
    CHECK(dock_engine.bound == std::vector<std::string>{Cube});
}

TEST_CASE("MT-1s decision 6: Undo sends an edit's previous against the digest after it, and a bind's "
          "previous bindings; a new material is not in it",
          "[devstudio][materials][mt1s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = project(temporary);
    FakeEngine engine;
    MaterialsDock dock{engine.dockHooks()};
    readyWithPaint(dock, engine, root);
    auto *undo = dock.undoAction();
    CHECK_FALSE(undo->isEnabled());

    named<QDoubleSpinBox>(dock, QStringLiteral("pelican.materials.param.specular_roughness.spin"))->setValue(0.7);
    auto sent = engine.take("edit_material");
    REQUIRE(sent.has_value());
    REQUIRE(dock.receiveResult(sent->id, QByteArray::fromStdString(
                                             edited("d2", {{"values", {{"specular_roughness", nullptr}}}}).dump())));
    answer(dock, engine, "get_material", paintMaterial("d2", {{"specular_roughness", 0.7}}));
    CHECK(undo->isEnabled());
    CHECK(undo->text() == QStringLiteral("Undo specular_roughness"));
    undo->trigger();
    sent = engine.take("edit_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"path", Paint},
                               {"name", "paint"},
                               {"base_material_digest", "d2"},
                               {"values", {{"specular_roughness", nullptr}}}});
    REQUIRE(dock.receiveResult(sent->id, QByteArray::fromStdString(
                                             edited("d3", {{"values", {{"specular_roughness", 0.7}}}}).dump())));
    CHECK(dock.session().undoEntries().empty());

    // A bind is taken back with the bindings it replaced.
    engine.selected = MaterialsDock::SelectedObject{Cube, "Cube"};
    named<QPushButton>(dock, QStringLiteral("pelican.materials.assignSelected"))->click();
    sent = engine.take("bind_material");
    REQUIRE(sent.has_value());
    const Json previous =
        Json::array({{{"mesh", 0}, {"primitive", 0}, {"material", "pelican.primitive.default"}}});
    REQUIRE(dock.receiveResult(sent->id, QByteArray::fromStdString(Json{{"committed", true},
                                                                        {"changed", true},
                                                                        {"model", Cube},
                                                                        {"previous", {{"bindings", previous}}},
                                                                        {"applied", "live"},
                                                                        {"restart_required", nullptr}}
                                                                       .dump())));
    undo->trigger();
    sent = engine.take("bind_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"model", Cube}, {"bindings", previous}});

    // The Inspector's rows keep their own Undo.
    FakeEngine inspector_engine;
    MaterialAssignWidget rows{inspector_engine.assignHooks()};
    readyCube(rows, inspector_engine);
    rows.session().setScope(MaterialAssignSession::Scope::Model);
    REQUIRE(rows.session().choose("paint"));
    sent = inspector_engine.take("bind_material");
    REQUIRE(sent.has_value());
    REQUIRE(rows.receiveResult(sent->id, QByteArray::fromStdString(Json{{"committed", true},
                                                                        {"changed", true},
                                                                        {"model", Cube},
                                                                        {"previous", {{"bindings", previous}}},
                                                                        {"applied", "live"},
                                                                        {"restart_required", nullptr}}
                                                                       .dump())));
    named<QToolButton>(rows, QStringLiteral("pelican.inspectorMaterialUndo"))->click();
    sent = inspector_engine.take("bind_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"model", Cube}, {"bindings", previous}});
}

TEST_CASE("MT-1s decision 7: a refused value is said under its row; any other refusal is one sentence "
          "in the dock's notice",
          "[devstudio][materials][mt1s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = project(temporary);
    FakeEngine engine;
    MaterialsDock dock{engine.dockHooks()};
    readyWithPaint(dock, engine, root);

    named<QDoubleSpinBox>(dock, QStringLiteral("pelican.materials.param.specular_ior.spin"))->setValue(-1.0 + 1.0);
    refuse(dock, engine, "edit_material", "material.value_invalid@1",
           QJsonObject{{QStringLiteral("path"), QString::fromLatin1(Paint)},
                       {QStringLiteral("param"), QStringLiteral("specular_ior")},
                       {QStringLiteral("reason"), QStringLiteral("value 0 is below the declared min 1")}});
    answer(dock, engine, "get_material", paintMaterial());
    auto *error = named<QLabel>(dock, QStringLiteral("pelican.materials.param.specular_ior.error"));
    CHECK(error->isVisibleTo(&dock));
    CHECK(error->toolTip() ==
          QStringLiteral("specular_ior cannot take that value: value 0 is below the declared min 1 "
                         "(material.value_invalid@1)."));
    CHECK(dock.noticeLine()->text().isEmpty());

    // The digest moved under it: said in the notice, and read again.
    named<QComboBox>(dock, QStringLiteral("pelican.materials.alphaMode"))->setCurrentIndex(1);
    refuse(dock, engine, "edit_material", "external_modification",
           QJsonObject{{QStringLiteral("path"), QString::fromLatin1(Paint)}});
    CHECK(dock.noticeLine()->text().startsWith(QStringLiteral("materials/paint.material.json changed outside Studio")));
    CHECK(engine.asked("list_materials"));

    CHECK(materialRefusalDescription(Json{{"code", "material.name_conflict@1"},
                                          {"error_fields", {{"name", "paint"}, {"path", Paint}}}}) ==
          "A material named paint exists already (material.name_conflict@1).");
}

TEST_CASE("MT-2s decision 1: a choice applies to this object first (set_object_material through the "
          "Inspector's session); Every Cube is the model's bind_material",
          "[devstudio][materials][mt2s]") {
    application();
    FakeEngine engine;
    MaterialAssignWidget rows{engine.assignHooks()};
    readyCube(rows, engine);
    auto *target = named<QComboBox>(rows, QStringLiteral("pelican.inspectorMaterialTarget"));
    CHECK(target->currentText() == QStringLiteral("This object"));
    CHECK(target->itemText(1) == QStringLiteral("Every Cube"));
    CHECK(named<QLabel>(rows, QStringLiteral("pelican.inspectorMaterialScope"))->text() ==
          QStringLiteral("Applies to Cube only."));

    auto *choice = named<QComboBox>(rows, QStringLiteral("pelican.inspectorMaterialChoice"));
    choice->setCurrentIndex(choice->findText(QStringLiteral("paint")));
    REQUIRE(engine.object_edits.size() == 1);
    CHECK(engine.object_edits.back().params ==
          Json{{"object", 7}, {"scene_id", "default_scene"}, {"materials", Json::array({{{"material", "paint"}}})}});
    CHECK(engine.object_edits.back().description == "paint on Cube");
    CHECK_FALSE(engine.asked("bind_material"));
    // While the edit is out the rows wait, holding what was asked.
    CHECK_FALSE(choice->isEnabled());
    CHECK(choice->currentText() == QStringLiteral("paint"));
    CHECK(choice->itemText(0) == QStringLiteral("paint (this object)"));
    // The session took it, ran it and is idle again: the object is read anew.
    rows.setSceneBusy(true);
    rows.setSceneBusy(false);
    answer(rows, engine, "get_object_materials", cubeObject("paint"));
    CHECK(choice->isEnabled());

    // What it carries already is not sent again.
    choice->setCurrentIndex(choice->findText(QStringLiteral("paint")));
    CHECK(engine.object_edits.size() == 1);
    // A session that cannot take an edit now says why, and nothing waits.
    engine.session_takes_edits = false;
    named<QToolButton>(rows, QStringLiteral("pelican.inspectorMaterialUseModelDefault"))->click();
    CHECK(engine.object_edits.size() == 1);
    CHECK(named<QLabel>(rows, QStringLiteral("pelican.inspectorMaterialNotice"))
              ->toolTip()
              .contains(QStringLiteral("Finish the edit")));
    CHECK(choice->isEnabled());

    // Every Cube: the model's bind, as MT-1s.
    target->setCurrentIndex(1);
    choice->setCurrentIndex(choice->findText(QStringLiteral("paint")));
    const auto sent = engine.take("bind_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"model", Cube}, {"material", "paint"}});
    CHECK(engine.object_edits.size() == 1);
}

TEST_CASE("MT-2s decision 2: the combo shows what is drawn and where it comes from; Use model default "
          "takes the object's own away",
          "[devstudio][materials][mt2s]") {
    application();
    FakeEngine engine;
    MaterialAssignWidget rows{engine.assignHooks()};
    readyCube(rows, engine);
    auto *choice = named<QComboBox>(rows, QStringLiteral("pelican.inspectorMaterialChoice"));
    auto *use_default = named<QToolButton>(rows, QStringLiteral("pelican.inspectorMaterialUseModelDefault"));
    CHECK(choice->currentText() == QStringLiteral("(model default: pelican.primitive.default)"));
    CHECK_FALSE(use_default->isEnabled());

    // Its own row: the drawn material, "(this object)".
    rows.setSubject(subjectOf(Cube, "Cube", 7, 2));
    answer(rows, engine, "get_object_materials", cubeObject("paint"));
    CHECK(choice->currentText() == QStringLiteral("paint (this object)"));
    REQUIRE(use_default->isEnabled());
    use_default->click();
    REQUIRE(engine.object_edits.size() == 1);
    CHECK(engine.object_edits.back().params == Json{{"object", 7},
                                                    {"scene_id", "default_scene"},
                                                    {"materials", Json::array({{{"material", nullptr}}})}});
    CHECK(engine.object_edits.back().description == "Model default on Cube");
}

TEST_CASE("MT-2s decision 3: with several primitives, each row follows the target -- This object sends "
          "set_object_material for that primitive",
          "[devstudio][materials][mt2s]") {
    application();
    FakeEngine engine;
    MaterialAssignWidget ship{engine.assignHooks()};
    ship.engineAvailable();
    ship.setSubject(subjectOf("ship", "Ship", 9));
    answer(ship, engine, "list_materials", listResult());
    answer(ship, engine, "get_material_bindings",
           Json{{"model", "ship"},
                {"primitives", Json::array({{{"mesh", 0}, {"primitive", 0}, {"slot", "hull"}, {"material", "hull"}},
                                            {{"mesh", 1}, {"primitive", 0}, {"slot", "sail"}, {"material", "sail"}}})},
                {"objects", 1},
                {"object_overrides", 1}});
    answer(ship, engine, "get_object_materials",
           Json{{"object", 9},
                {"scene_id", "default_scene"},
                {"model", "ship"},
                {"materials", Json::array({{{"row_id", 3},
                                            {"material", "paint"},
                                            {"primitive", {{"mesh", 1}, {"primitive", 0}}},
                                            {"resolved", true}}})},
                {"primitives",
                 Json::array({{{"mesh", 0}, {"primitive", 0}, {"slot", "hull"}, {"material", "hull"},
                               {"source", "model"}, {"model_material", "hull"}, {"drawn", "hull"}},
                              {{"mesh", 1}, {"primitive", 0}, {"slot", "sail"}, {"material", "paint"},
                               {"source", "object"}, {"model_material", "sail"}, {"drawn", "paint"}}})}});
    CHECK(named<QComboBox>(ship, QStringLiteral("pelican.inspectorMaterialChoice"))->currentText() ==
          QStringLiteral("(differs per primitive)"));
    auto *first = named<QComboBox>(ship, QStringLiteral("pelican.inspectorMaterialPrimitive.0"));
    auto *second = named<QComboBox>(ship, QStringLiteral("pelican.inspectorMaterialPrimitive.1"));
    CHECK(first->currentText() == QStringLiteral("(model default: hull)"));
    CHECK(second->currentText() == QStringLiteral("paint (this object)"));

    first->setCurrentIndex(first->findText(QStringLiteral("paint")));
    REQUIRE(engine.object_edits.size() == 1);
    CHECK(engine.object_edits.back().params == Json{{"object", 9},
                                                    {"scene_id", "default_scene"},
                                                    {"material", "paint"},
                                                    {"primitive", {{"mesh", 0}, {"primitive", 0}}}});
    ship.setSceneBusy(true);
    ship.setSceneBusy(false);
    REQUIRE(engine.take("get_object_materials").has_value());
    // The primitive's own row taken away: Model default (sail).
    second->setCurrentIndex(second->findText(QStringLiteral("Model default (sail)")));
    REQUIRE(engine.object_edits.size() == 2);
    CHECK(engine.object_edits.back().params == Json{{"object", 9},
                                                    {"scene_id", "default_scene"},
                                                    {"material", nullptr},
                                                    {"primitive", {{"mesh", 1}, {"primitive", 0}}}});
    CHECK_FALSE(engine.asked("bind_material"));
}

TEST_CASE("MT-2s decision 4: N and M are get_material_bindings' objects and object_overrides, read again "
          "whenever the scene's revision moves",
          "[devstudio][materials][mt2s]") {
    application();
    FakeEngine engine;
    MaterialAssignWidget rows{engine.assignHooks()};
    readyCube(rows, engine, cubeObject(), cubeBindings(2, 1));
    rows.session().setScope(MaterialAssignSession::Scope::Model);
    auto *scope = named<QLabel>(rows, QStringLiteral("pelican.inspectorMaterialScope"));
    CHECK(scope->text() == QStringLiteral("Applies to every Cube in this project (2 objects, 1 with their own material)."));
    CHECK_FALSE(engine.asked("export_scene_snapshot"));

    // Another Cube is added: the same model, a new revision -- counted again.
    rows.setSubject(subjectOf(Cube, "Cube", 7, 2));
    answer(rows, engine, "get_material_bindings", cubeBindings(3, 1));
    answer(rows, engine, "get_object_materials", cubeObject());
    CHECK(scope->text() == QStringLiteral("Applies to every Cube in this project (3 objects, 1 with their own material)."));
    // The same revision asks nothing.
    rows.setSubject(subjectOf(Cube, "Cube", 7, 2));
    CHECK_FALSE(engine.asked("get_material_bindings"));
    CHECK(parseModelObjectCounts(Json{{"primitives", Json::array()}}).objects == std::nullopt);
}

TEST_CASE("MT-2s decision 5: an object's material is the scene's Undo; the rows' own Undo takes a bind "
          "back to the model it was sent to, whatever is selected by then",
          "[devstudio][materials][mt2s]") {
    application();
    FakeEngine engine;
    MaterialAssignWidget rows{engine.assignHooks()};
    readyCube(rows, engine);
    // This object: handed to the scene session, not kept here.
    REQUIRE(rows.session().choose("paint"));
    REQUIRE(engine.object_edits.size() == 1);
    CHECK(rows.session().undoEntries().empty());
    rows.setSceneBusy(true);
    rows.setSceneBusy(false);
    answer(rows, engine, "get_object_materials", cubeObject("paint"));

    // Every Cube: a bind, kept with the model it went to.
    rows.session().setScope(MaterialAssignSession::Scope::Model);
    REQUIRE(rows.session().choose("paint"));
    auto sent = engine.take("bind_material");
    REQUIRE(sent.has_value());
    const Json previous =
        Json::array({{{"mesh", 0}, {"primitive", 0}, {"material", "pelican.primitive.default"}}});
    REQUIRE(rows.receiveResult(sent->id, QByteArray::fromStdString(Json{{"committed", true},
                                                                        {"changed", true},
                                                                        {"model", Cube},
                                                                        {"previous", {{"bindings", previous}}},
                                                                        {"applied", "live"},
                                                                        {"restart_required", nullptr}}
                                                                       .dump())));
    REQUIRE(rows.session().undoEntries().size() == 1);
    CHECK(rows.session().undoEntries().back().model == Cube);

    // A ship is selected now; Undo still goes to the Cube.
    rows.setSubject(subjectOf("ship", "Ship", 9));
    auto *undo = named<QToolButton>(rows, QStringLiteral("pelican.inspectorMaterialUndo"));
    CHECK(undo->text() == QStringLiteral("Undo paint on every Cube"));
    undo->click();
    sent = engine.take("bind_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"model", Cube}, {"bindings", previous}});
}

TEST_CASE("MT-2s decision 6: a simplemodelview's materials list is chosen in the Material rows, not typed "
          "in the generic list",
          "[devstudio][materials][mt2s]") {
    CHECK_FALSE(inspectorListShown("simplemodelview", "/materials"));
    CHECK(inspectorListShown("animation", "/parameters"));
    CHECK(inspectorListShown("simplemodelview", "/other"));
    // Engine MT-2 lists materials among simplemodelview's flat fields as a
    // "list": no widget of its own (its rows come from the schema node), and
    // the model field is planned as before.
    const Json component{
        {"name", "simplemodelview"},
        {"component_index", 1},
        {"authored_json", {{"name", "simplemodelview"}, {"model", Cube}}},
        {"schema",
         {{"state", "available"},
          {"fields", Json::array({{{"name", "model"}, {"type", "string"}}, {{"name", "materials"}, {"type", "list"}}})}}}};
    const auto plan = makeInspectorWidgetPlan(component, 7);
    REQUIRE(plan.size() == 1);
    CHECK(plan.front().field_name == "model");
}

TEST_CASE("MT-2s decision 7: a refused set_object_material is one sentence in the rows; a material that is "
          "not loaded is marked",
          "[devstudio][materials][mt2s]") {
    application();
    FakeEngine engine;
    MaterialAssignWidget rows{engine.assignHooks()};
    readyCube(rows, engine, cubeObject("gone", false));
    auto *missing = named<QLabel>(rows, QStringLiteral("pelican.inspectorMaterialMissing"));
    CHECK(missing->isVisibleTo(&rows));
    CHECK(missing->toolTip() == QStringLiteral("Material gone is missing; drawing the model default"));
    CHECK(named<QComboBox>(rows, QStringLiteral("pelican.inspectorMaterialChoice"))->currentText() ==
          QStringLiteral("(model default: pelican.primitive.default)"));

    REQUIRE(rows.session().choose("paint"));
    const auto sentence = rows.objectEditRefused(
        QStringLiteral("material.object_unknown@1"), QStringLiteral("set_object_material: object 7 draws no model"),
        QJsonObject{{QStringLiteral("error_code"), QStringLiteral("material.object_unknown@1")},
                    {QStringLiteral("error_fields"),
                     QJsonObject{{QStringLiteral("object"), 7},
                                 {QStringLiteral("reason"), QStringLiteral("the object has no simplemodelview")}}}});
    CHECK(sentence == QStringLiteral("Object 7 cannot take a material of its own: the object has no "
                                     "simplemodelview (material.object_unknown@1)."));
    CHECK(named<QLabel>(rows, QStringLiteral("pelican.inspectorMaterialNotice"))->toolTip().isEmpty() == false);
    // The rows read the object again and take choices again.
    CHECK(engine.asked("get_object_materials"));
    CHECK_FALSE(rows.session().objectEditPending());
    // The table's existing material.* sentences are not repeated for it.
    CHECK(materialRefusalDescription(Json{{"code", "material.name_unknown@1"},
                                          {"error_fields", {{"name", "gone"}, {"reason", "not registered"}}}}) ==
          "No material is named gone: not registered (material.name_unknown@1).");
}

TEST_CASE("MT-2s-b decision 1: a refused object is called by its name when the engine gives one",
          "[devstudio][materials][mt2s]") {
    // Engine MT-2b: error_fields are {object, scene_id?, name?, reason}.
    CHECK(materialRefusalDescription(Json{{"code", "material.object_unknown@1"},
                                          {"error_fields",
                                           {{"object", 7},
                                            {"scene_id", 1},
                                            {"name", "Cube_001"},
                                            {"reason", "the object has no simplemodelview"}}}}) ==
          "Cube_001 cannot take a material of its own: the object has no simplemodelview "
          "(material.object_unknown@1).");
    // An object without a name (or an id that is not there) keeps the id.
    CHECK(materialRefusalDescription(Json{{"code", "material.object_unknown@1"},
                                          {"error_fields", {{"object", 9}, {"reason", "no such object"}}}}) ==
          "Object 9 cannot take a material of its own: no such object (material.object_unknown@1).");
    CHECK(materialRefusalDescription(Json{{"code", "material.object_unknown@1"},
                                          {"error_fields", {{"object", 9}, {"name", ""}, {"reason", "no such object"}}}}) ==
          "Object 9 cannot take a material of its own: no such object (material.object_unknown@1).");
}

namespace {

// Engine MT-2b's answer to a bind_material that puts every primitive back on
// the model's own material: the declaration is taken off.
Json unboundResult(bool changed, const Json &previous) {
    return Json{{"committed", true},
                {"changed", changed},
                {"model", Cube},
                {"material_bindings", nullptr},
                {"bindings_digest", nullptr},
                {"previous", {{"bindings", previous}}},
                {"previous_bindings_digest", changed ? Json("b1") : Json(nullptr)},
                {"asset_data_digest", "a2"},
                {"previous_asset_data_digest", "a1"},
                {"applied", "live"}};
}

Json redCubeBindings() {
    auto bindings = cubeBindings();
    bindings["material_bindings"] = "materials/cube.bindings.json";
    bindings["bindings_digest"] = "b1";
    bindings["primitives"][0]["material"] = "paint";
    return bindings;
}

} // namespace

TEST_CASE("MT-2s-b decision 2: Model default on Every Cube is taken off (material_bindings null), reads as "
          "Model default, and its Undo binds the material again",
          "[devstudio][materials][mt2s]") {
    application();
    FakeEngine engine;
    MaterialAssignWidget rows{engine.assignHooks()};
    readyCube(rows, engine, cubeObject(), redCubeBindings());
    rows.session().setScope(MaterialAssignSession::Scope::Model);
    auto *choice = named<QComboBox>(rows, QStringLiteral("pelican.inspectorMaterialChoice"));
    CHECK(choice->currentText() == QStringLiteral("paint"));

    choice->setCurrentIndex(choice->findText(QStringLiteral("Model default")));
    auto sent = engine.take("bind_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params ==
          Json{{"model", Cube},
               {"bindings", Json::array({{{"mesh", 0}, {"primitive", 0}, {"material", "pelican.primitive.default"}}})}});
    const Json previous = Json::array({{{"mesh", 0}, {"primitive", 0}, {"material", "paint"}}});
    REQUIRE(rows.receiveResult(sent->id, QByteArray::fromStdString(unboundResult(true, previous).dump())));
    REQUIRE(rows.session().undoEntries().size() == 1);
    CHECK(rows.session().undoEntries().back().bindings == previous);
    // The model read again has no declaration: Model default.
    answer(rows, engine, "get_material_bindings", cubeBindings());
    answer(rows, engine, "get_object_materials", cubeObject());
    CHECK(rows.session().sharedMaterial() == std::string{});
    CHECK(choice->currentText() == QStringLiteral("Model default"));
    CHECK(choice->isEnabled());
    CHECK(rows.session().notice().empty());

    // Undo: the model's material again, by its previous bindings.
    auto *undo = named<QToolButton>(rows, QStringLiteral("pelican.inspectorMaterialUndo"));
    CHECK(undo->text() == QStringLiteral("Undo Model default on every Cube"));
    undo->click();
    sent = engine.take("bind_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"model", Cube}, {"bindings", previous}});
    REQUIRE(rows.receiveResult(
        sent->id,
        QByteArray::fromStdString(Json{{"committed", true},
                                       {"changed", true},
                                       {"model", Cube},
                                       {"material_bindings", "materials/cube.bindings.json"},
                                       {"previous",
                                        {{"bindings", Json::array({{{"mesh", 0},
                                                                    {"primitive", 0},
                                                                    {"material", "pelican.primitive.default"}}})}}},
                                       {"applied", "live"}}
                                      .dump())));
    CHECK(rows.session().undoEntries().empty());
    answer(rows, engine, "get_material_bindings", redCubeBindings());
    CHECK(choice->currentText() == QStringLiteral("paint"));
}

TEST_CASE("MT-2s-b decision 3: a Model default the engine has already (changed:false) leaves the rows "
          "choosable, with nothing to undo",
          "[devstudio][materials][mt2s]") {
    application();
    FakeEngine engine;
    MaterialAssignWidget rows{engine.assignHooks()};
    // Read while the cube was bound; another client took it off since.
    readyCube(rows, engine, cubeObject(), redCubeBindings());
    rows.session().setScope(MaterialAssignSession::Scope::Model);
    REQUIRE(rows.session().choose(""));
    auto sent = engine.take("bind_material");
    REQUIRE(sent.has_value());
    CHECK(rows.session().bindPending());
    REQUIRE(rows.receiveResult(sent->id, QByteArray::fromStdString(unboundResult(false, Json::array()).dump())));
    CHECK_FALSE(rows.session().bindPending());
    CHECK(rows.session().undoEntries().empty());
    CHECK(named<QToolButton>(rows, QStringLiteral("pelican.inspectorMaterialUndo"))->isHidden());
    // What the engine has now is read again, and the rows take choices.
    answer(rows, engine, "get_material_bindings", cubeBindings());
    answer(rows, engine, "get_object_materials", cubeObject());
    auto *choice = named<QComboBox>(rows, QStringLiteral("pelican.inspectorMaterialChoice"));
    CHECK(choice->currentText() == QStringLiteral("Model default"));
    CHECK(choice->isEnabled());
    CHECK(rows.session().choosable());
    REQUIRE(rows.session().choose("paint"));
    CHECK(engine.take("bind_material").has_value());
}

TEST_CASE("MT-3s decision 5: an edit the engine rebuilt in place restarts nothing and says how many models "
          "it rebuilt; a routing edit's rewritten bindings are read again, its Undo too; a bound "
          "material's routing taken off is one sentence",
          "[devstudio][materials][mt3s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = project(temporary);
    FakeEngine engine;
    MaterialsDock dock{engine.dockHooks()};
    readyWithPaint(dock, engine, root);
    const auto rebuilt = [](const std::string &digest, Json previous, Json models, Json rewritten) {
        return Json{{"committed", true},
                    {"changed", true},
                    {"path", Paint},
                    {"name", "paint"},
                    {"material_digest", digest},
                    {"previous", std::move(previous)},
                    {"applied", "rebuilt"},
                    {"restart_required", nullptr},
                    {"rebuilt_models", std::move(models)},
                    {"rewritten_bindings", std::move(rewritten)}};
    };

    // A tag: registered again in place, no restart.
    auto *tags = static_cast<GraphCanvasTagPicker *>(named<QToolButton>(dock, QStringLiteral("pelican.materials.tags")));
    REQUIRE(tags->addTag("glass"));
    auto sent = engine.take("edit_material");
    REQUIRE(sent.has_value());
    REQUIRE(dock.receiveResult(sent->id, QByteArray::fromStdString(
                                             rebuilt("d2", {{"tags", {"shiny"}}}, Json::array({Cube}),
                                                     Json::array())
                                                 .dump())));
    CHECK(engine.restarts.empty());
    CHECK(dock.noticeLine()->text() == QStringLiteral("Applied; rebuilt 1 model."));
    CHECK_FALSE(named<QPushButton>(dock, QStringLiteral("pelican.materials.restartEngine"))->isVisibleTo(&dock));
    CHECK(engine.bound.empty());
    answer(dock, engine, "get_material", paintMaterial("d2"));

    // The routing: the bindings that name paint are rewritten in the same
    // write, and the rows showing them read them again.
    auto *alpha_mode = named<QComboBox>(dock, QStringLiteral("pelican.materials.alphaMode"));
    alpha_mode->setCurrentIndex(2);
    sent = engine.take("edit_material");
    REQUIRE(sent.has_value());
    // The choice stays shown while the edit is out (it used to flick back to
    // the document's value until the reply).
    CHECK(alpha_mode->currentText() == QStringLiteral("blend"));
    const Json opaque{{"alpha_mode", "opaque"}, {"double_sided", false}};
    const Json rewritten = Json::array({{{"model", Cube},
                                         {"material_bindings", "assets/bindings/cube.json"},
                                         {"bindings_digest", "b2"},
                                         {"previous_bindings_digest", "b1"}}});
    REQUIRE(dock.receiveResult(sent->id,
                               QByteArray::fromStdString(
                                   rebuilt("d3", {{"routing", opaque}}, Json::array({Cube, "models/box"}), rewritten)
                                       .dump())));
    CHECK(engine.restarts.empty());
    CHECK(dock.noticeLine()->text() == QStringLiteral("Applied; rebuilt 2 models."));
    CHECK(engine.bound == std::vector<std::string>{Cube});
    answer(dock, engine, "get_material", paintMaterial("d3"));

    // Undo sends the previous routing; the engine puts the bindings back with
    // it, and they are read again.
    auto *undo = dock.undoAction();
    REQUIRE(undo->isEnabled());
    undo->trigger();
    sent = engine.take("edit_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"path", Paint}, {"name", "paint"}, {"base_material_digest", "d3"}, {"routing", opaque}});
    REQUIRE(dock.receiveResult(sent->id, QByteArray::fromStdString(
                                             rebuilt("d4", {{"routing", {{"alpha_mode", "blend"},
                                                                         {"double_sided", false}}}},
                                                     Json::array({Cube}), rewritten)
                                                 .dump())));
    CHECK(engine.bound == std::vector<std::string>{Cube, Cube});
    CHECK(engine.restarts.empty());

    // restart_required is still a restart (a material the engine has not
    // registered, a rebuild that failed).
    named<QComboBox>(dock, QStringLiteral("pelican.materials.alphaMode"))->setCurrentIndex(1);
    sent = engine.take("edit_material");
    REQUIRE(sent.has_value());
    REQUIRE(dock.receiveResult(sent->id, QByteArray::fromStdString(
                                             edited("d5", {{"routing", opaque}},
                                                    {{"reason", "the models could not be rebuilt in place"}})
                                                 .dump())));
    CHECK(engine.restarts.size() == 1);

    // The routing of a bound material taken off: one sentence, the models
    // named.
    CHECK(materialRefusalDescription(Json{
              {"code", "material.routing_conflict@1"},
              {"error_fields",
               {{"path", Paint},
                {"material", "paint"},
                {"routing", nullptr},
                {"bound_by", Json::array({{{"model", Cube}, {"routing", opaque}},
                                          {{"model", "models/box"}, {"routing", opaque}}})}}}}) ==
          "paint keeps its routing: engine://primitives/cube, models/box draw it through their bindings; bind "
          "them to another material first (material.routing_conflict@1).");
}

TEST_CASE("FO-1s decision 2: a material document the engine refused at its start is fixed from the Problems "
          "row -- its row in the Materials dock says the line and the column, Show the document writes the "
          "text back, Register again takes it in or says a restart does",
          "[devstudio][materials][fo1s]") {
    application();
    // The Problems rows (engine FO-1's get_problems): the document is its row
    // in the Materials dock; the model whose bindings name it, its bindings.
    const auto invalid = readEngineProblem(
        {{"id", "2b52acdd03ac27ff"},
         {"severity", "error"},
         {"source", "startup"},
         {"code", "material.document_invalid@1"},
         {"message", "material.document_invalid@1: the material document 'materials/paint.material.json' could not "
                     "be read; the engine started without its materials: syntax error"},
         {"document", "project://materials/paint.material.json"},
         {"document_kind", "material"},
         {"line", 4},
         {"column", 1},
         {"fields", {{"path", Paint}, {"reason", "syntax error"}, {"line", 4}, {"column", 1}}}});
    REQUIRE(invalid);
    CHECK(invalid->place == ProblemRow::Place::Material);
    CHECK(invalid->document == Paint);
    CHECK(invalid->material.empty());
    CHECK(invalid->place_text == "Materials: paint.material.json");
    CHECK(invalid->lineText() == "4:1");
    const auto unresolved = readEngineProblem(
        {{"id", "5efd70425699e3ac"},
         {"severity", "warning"},
         {"source", "startup"},
         {"code", "material.binding_unresolved@1"},
         {"message", "material.binding_unresolved@1: model 'engine://primitives/cube' binds 'paint', which no "
                     "registered material document holds; its primitives draw with the model's own material"},
         {"document_kind", "material"},
         {"fields", {{"model", Cube}, {"materials", {"paint"}}}}});
    REQUIRE(unresolved);
    CHECK(unresolved->place == ProblemRow::Place::Material);
    CHECK(unresolved->model == Cube);
    CHECK(unresolved->place_text == "Materials: Cube");
    CHECK(unresolved->sentence == "Cube binds paint, which no loaded material document holds; it draws with the "
                                  "model's own material (material.binding_unresolved@1).");

    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = project(temporary);
    const std::string broken = "{\n  \"schema\": \"pelican.material\",\n  \"version\": 1,\n  \"materials\": [\n"
                               "    {\"name\": \"paint\", \"surface\": \"" +
                               std::string{OpenPbr} + "\"}\n  ]\n";
    write(root / "materials" / "paint.material.json", broken);
    FakeEngine engine;
    MaterialsDock dock{engine.dockHooks()};
    dock.show();
    dock.open(root);
    dock.engineAvailable();
    const Json refused{{"asset_data_digest", "a1"},
                       {"documents", Json::array({{{"path", Paint},
                                                   {"status", "invalid"},
                                                   {"reason", "syntax error while parsing value"},
                                                   {"line", 7},
                                                   {"column", 1},
                                                   {"material_digest", "d0"},
                                                   {"materials", Json::array()}}})}};
    answer(dock, engine, "list_materials", refused);
    answer(dock, engine, "list_surfaces", surfaces());

    // The Problems row goes to the document's row: what is wrong, and where.
    REQUIRE(dock.revealMaterial(invalid->document, invalid->material));
    REQUIRE(dock.session().broken());
    CHECK(*dock.session().broken() == Paint);
    CHECK(named<QLabel>(dock, QStringLiteral("pelican.materials.broken"))->text() ==
          QStringLiteral("materials/paint.material.json would not load at line 7, column 1: syntax error while "
                         "parsing value"));
    CHECK_FALSE(engine.asked("get_material"));

    // Show the document: the bytes on disk, the cursor where the parser
    // stopped; Save waits for JSON and writes the file whole.
    named<QPushButton>(dock, QStringLiteral("pelican.materials.showDocument"))->click();
    QPointer<QDialog> dialog = named<QDialog>(dock, QStringLiteral("pelican.materials.documentDialog"));
    auto *text = named<QPlainTextEdit>(*dialog, QStringLiteral("pelican.materials.documentDialog.text"));
    auto *save = named<QPushButton>(*dialog, QStringLiteral("pelican.materials.documentDialog.save"));
    CHECK(text->textCursor().blockNumber() == 6);
    CHECK_FALSE(save->isEnabled());
    CHECK(named<QLabel>(*dialog, QStringLiteral("pelican.materials.documentDialog.notice"))
              ->text()
              .startsWith(QStringLiteral("Not valid JSON yet at line ")));
    text->moveCursor(QTextCursor::End);
    text->insertPlainText(QStringLiteral("}\n"));
    REQUIRE(save->isEnabled());
    save->click();
    QApplication::processEvents();
    {
        std::ifstream in{root / "materials" / "paint.material.json", std::ios::binary};
        const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        CHECK(bytes == broken + "}\n");
    }
    // MR-1s, decision 1: Save asks the engine to take it in at once. An
    // engine that holds it listed already, and still not loaded when the
    // list is read again, reads it only when it starts again.
    const auto first = refuse(dock, engine, "register_material", "material.path_conflict@1",
                              QJsonObject{{QStringLiteral("path"), QString::fromLatin1(Paint)}});
    CHECK(first.params == Json{{"path", Paint}});
    answer(dock, engine, "list_materials", refused);
    CHECK(dock.noticeLine()->text() == QStringLiteral("The engine still holds materials/paint.material.json as it "
                                                      "read it before; a restart reads the file again "
                                                      "(material.path_conflict@1)."));
    // The window stays open with the same sentence.
    REQUIRE_FALSE(dialog.isNull());
    CHECK(dialog->isVisible());
    CHECK(named<QLabel>(*dialog, QStringLiteral("pelican.materials.documentDialog.notice"))->text() ==
          dock.noticeLine()->text());
    dialog->close();
    QApplication::processEvents();
    auto *restart = named<QPushButton>(dock, QStringLiteral("pelican.materials.restartEngine"));
    CHECK(restart->isVisibleTo(&dock));
    restart->click();
    REQUIRE(engine.restarts.size() == 1);
    CHECK(engine.restarts.front().contains(QStringLiteral("materials/paint.material.json")));
    QApplication::processEvents();

    // An engine that takes it in: registered, read again, and its material
    // is the one the editor shows.
    named<QPushButton>(dock, QStringLiteral("pelican.materials.registerAgain"))->click();
    (void)answer(dock, engine, "register_material",
                 Json{{"committed", true}, {"path", Paint}, {"names", {"paint"}}, {"material_digest", "d1"},
                      {"applied", "live"}, {"restart_required", nullptr}});
    CHECK(dock.noticeLine()->text() == QStringLiteral("Registered paint from materials/paint.material.json."));
    answer(dock, engine, "list_materials", listResult());
    CHECK_FALSE(dock.session().broken());
    REQUIRE(dock.session().selection());
    CHECK(dock.session().selection()->name == "paint");
    CHECK(engine.asked("get_material"));

    // The warning's row: the model's bindings in the dock, what is wrong
    // above them.
    dock.revealModel(unresolved->model, unresolved->sentence);
    CHECK(named<QFrame>(dock, QStringLiteral("pelican.materials.model"))->isVisibleTo(&dock));
    CHECK(named<QLabel>(dock, QStringLiteral("pelican.materials.model.heading"))->text() ==
          QStringLiteral("Bindings of Cube"));
    CHECK(named<QLabel>(dock, QStringLiteral("pelican.materials.model.notice"))->text() ==
          QString::fromStdString(unresolved->sentence));
    const auto bindings = engine.take("get_material_bindings");
    REQUIRE(bindings.has_value());
    CHECK(bindings->params == Json{{"model", Cube}});
}

TEST_CASE("FO-1s decision 3: Delete… and Rename… on a material's row -- in_use is one sentence with a button to "
          "each place, a delete is undone with register_material and a rename with the rename back",
          "[devstudio][materials][fo1s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = project(temporary);
    FakeEngine engine;
    std::vector<QString> objects;
    std::vector<std::pair<QString, QString>> documents;
    auto hooks = engine.dockHooks();
    hooks.reveal_object = [&objects](const QString &object) { objects.push_back(object); };
    hooks.reveal_document = [&documents](const QString &document, const QString &find) {
        documents.emplace_back(document, find);
    };
    MaterialsDock dock{std::move(hooks)};
    dock.show();
    readyWithPaint(dock, engine, root);
    auto *remove = named<QAction>(dock, QStringLiteral("pelican.materials.delete"));
    auto *rename = named<QAction>(dock, QStringLiteral("pelican.materials.rename"));
    CHECK(remove->isEnabled());
    CHECK(rename->isEnabled());
    // A document's row is not a material.
    dock.list()->setCurrentItem(dock.list()->topLevelItem(1));
    CHECK_FALSE(remove->isEnabled());
    dock.list()->setCurrentItem(rowNamed(*dock.list(), QStringLiteral("paint")));
    REQUIRE(remove->isEnabled());

    // Delete… asks first.
    remove->trigger();
    CHECK(named<QLabel>(dock, QStringLiteral("pelican.materials.deleteConfirm"))->text() ==
          QStringLiteral("Delete material paint? materials/paint.material.json is taken off the project's list and "
                         "stays on disk; Undo puts it back."));
    CHECK(engine.pending.empty());
    named<QPushButton>(dock, QStringLiteral("pelican.materials.deleteConfirmed"))->click();
    const auto first = refuse(dock, engine, "delete_material", "material.in_use@1",
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("paint")},
                                          {QStringLiteral("bindings"), QJsonArray{QString::fromLatin1(Cube)}},
                                          {QStringLiteral("objects"), QJsonArray{QStringLiteral("Cube_001")}},
                                          {QStringLiteral("count"), 1},
                                          {QStringLiteral("render_documents"),
                                           QJsonArray{QStringLiteral("passes/x.json")}}},
                              QStringLiteral("delete_material: material 'paint' is bound by 1 model(s)"));
    CHECK(first.params == Json{{"name", "paint"}});
    CHECK(dock.noticeLine()->text() ==
          QStringLiteral("paint is used by the bindings of Cube, 1 object (Cube_001) and render document "
                         "passes/x.json; take it off them first (material.in_use@1)."));
    // A button to each place.
    named<QPushButton>(dock, QStringLiteral("pelican.materials.uses.object#Cube_001"))->click();
    REQUIRE(objects.size() == 1);
    CHECK(objects.front() == QStringLiteral("Cube_001"));
    named<QPushButton>(dock, QStringLiteral("pelican.materials.uses.document#passes/x.json"))->click();
    REQUIRE(documents.size() == 1);
    CHECK(documents.front() == std::pair{QStringLiteral("passes/x.json"), QStringLiteral("\"paint\"")});
    named<QPushButton>(dock, QStringLiteral("pelican.materials.uses.model#engine://primitives/cube"))->click();
    CHECK(named<QFrame>(dock, QStringLiteral("pelican.materials.model"))->isVisibleTo(&dock));
    // The dock read its list again after the refusal, and the model's row
    // reads it too.
    (void)answer(dock, engine, "list_materials", listResult());
    (void)answer(dock, engine, "list_materials", listResult());
    (void)answer(dock, engine, "get_material_bindings",
                 Json{{"model", Cube},
                      {"material_bindings", "materials/cube.bindings.json"},
                      {"primitives", Json::array({{{"mesh", 0},
                                                   {"primitive", 0},
                                                   {"slot", "pelican.primitive.default"},
                                                   {"material", "paint"}}})}});
    auto *choice = named<QComboBox>(dock, QStringLiteral("pelican.materials.model.choice"));
    CHECK(choice->currentText() == QStringLiteral("paint"));
    REQUIRE(choice->isEnabled());
    // Model default: every primitive back on its own material.
    choice->setCurrentIndex(choice->findText(QStringLiteral("Model default")));
    const auto unbind = engine.take("bind_material");
    REQUIRE(unbind.has_value());
    CHECK(unbind->params ==
          Json{{"model", Cube},
               {"bindings", Json::array({{{"mesh", 0}, {"primitive", 0}, {"material", "pelican.primitive.default"}}})}});

    // Taken off them, the delete goes; Undo registers the document again.
    remove->trigger();
    named<QPushButton>(dock, QStringLiteral("pelican.materials.deleteConfirmed"))->click();
    CHECK_FALSE(named<QWidget>(dock, QStringLiteral("pelican.materials.uses"))->isVisibleTo(&dock));
    (void)answer(dock, engine, "delete_material",
                 Json{{"committed", true}, {"name", "paint"}, {"removed", {{"path", Paint}}},
                      {"applied", "live"}, {"restart_required", nullptr}});
    CHECK(dock.noticeLine()->text() ==
          QStringLiteral("Deleted paint; materials/paint.material.json stays on disk."));
    CHECK(dock.undoAction()->text() == QStringLiteral("Undo Delete paint"));
    // Every list read: the model's row reads the list too.
    const auto lists = [&](const Json &result) {
        while (engine.asked("list_materials")) answer(dock, engine, "list_materials", result);
    };
    lists(Json{{"asset_data_digest", "a2"}, {"documents", Json::array()}});
    dock.undoAction()->trigger();
    const auto back = answer(dock, engine, "register_material",
                             Json{{"committed", true}, {"path", Paint}, {"names", {"paint"}},
                                  {"applied", "live"}, {"restart_required", nullptr}});
    CHECK(back.params == Json{{"path", Paint}});
    CHECK(dock.noticeLine()->text() == QStringLiteral("Put paint back from materials/paint.material.json."));
    CHECK(dock.session().undoEntries().empty());
    lists(listResult());
    REQUIRE(dock.session().selection());
    CHECK(dock.session().selection()->name == "paint");
    // The last read of the material is the one the dock waits for.
    std::optional<FakeEngine::Request> read;
    while (auto next = engine.take("get_material")) read = next;
    REQUIRE(read.has_value());
    REQUIRE(dock.receiveResult(read->id, QByteArray::fromStdString(paintMaterial().dump())));

    // Rename…: the name typed, checked before it is sent.
    dock.list()->setCurrentItem(rowNamed(*dock.list(), QStringLiteral("paint")));
    rename->trigger();
    auto *field = named<QLineEdit>(dock, QStringLiteral("pelican.materials.rename.name"));
    auto *ok = named<QPushButton>(dock, QStringLiteral("pelican.materials.rename.ok"));
    CHECK(field->text() == QStringLiteral("paint"));
    CHECK_FALSE(ok->isEnabled());
    field->setText(QStringLiteral("not ok"));
    CHECK_FALSE(ok->isEnabled());
    CHECK(named<QLabel>(dock, QStringLiteral("pelican.materials.rename.notice"))->text() ==
          QStringLiteral("A material name is letters, digits and _."));
    field->setText(QStringLiteral("coat"));
    REQUIRE(ok->isEnabled());
    ok->click();
    const auto renamed = answer(dock, engine, "rename_material",
                                Json{{"committed", true},
                                     {"changed", true},
                                     {"path", Paint},
                                     {"name", "coat"},
                                     {"previous_name", "paint"},
                                     {"material_digest", "d5"},
                                     {"rewritten_bindings", Json::array({{{"model", Cube}}})},
                                     {"applied", "rebuilt"},
                                     {"restart_required", nullptr},
                                     {"rebuilt_models", Json::array({Cube})}});
    CHECK(renamed.params == Json{{"name", "paint"}, {"new_name", "coat"}});
    CHECK(dock.noticeLine()->text() == QStringLiteral("Renamed paint to coat."));
    CHECK(std::ranges::find(engine.bound, std::string{Cube}) != engine.bound.end());
    CHECK(dock.undoAction()->text() == QStringLiteral("Undo Rename paint to coat"));
    dock.undoAction()->trigger();
    const auto undone = refuse(dock, engine, "rename_material", "material.in_use@1",
                               QJsonObject{{QStringLiteral("name"), QStringLiteral("coat")},
                                           {QStringLiteral("bindings"), QJsonArray{}},
                                           {QStringLiteral("objects"), QJsonArray{}},
                                           {QStringLiteral("count"), 0},
                                           {QStringLiteral("render_documents"),
                                            QJsonArray{QStringLiteral("passes/eb1_reference.json")}}});
    CHECK(undone.params == Json{{"name", "coat"}, {"new_name", "paint"}});
    CHECK(dock.noticeLine()->text() ==
          QStringLiteral("coat is named by render document passes/eb1_reference.json, which a rename does not "
                         "rewrite (material.in_use@1)."));

    // The other refusals of a delete.
    CHECK(materialDeleteRefusalDescription("material.document_shared@1",
                                           {{"name", "a"}, {"path", "materials/two.material.json"},
                                            {"names", {"a", "b"}}},
                                           false) ==
          "materials/two.material.json holds a and b: only a document of one material is deleted, so a stays "
          "(material.document_shared@1).");
    CHECK(materialDeleteRefusalDescription("material.name_invalid@1",
                                           {{"name", "pelican.primitive.default"}, {"reason", "engine_material"}},
                                           false) ==
          "pelican.primitive.default is the engine's own material; it is not deleted (material.name_invalid@1).");
}

// --- MR-1s (engine EB-2): a broken material document comes back without a
// restart.

namespace {

// The engine's start refused paint (FO-1): listed, not loaded, nothing in it.
Json refusedAtStart() {
    return Json{{"asset_data_digest", "a1"},
                {"documents", Json::array({{{"path", Paint},
                                            {"status", "invalid"},
                                            {"reason", "syntax error while parsing value"},
                                            {"line", 7},
                                            {"column", 1},
                                            {"material_digest", "d0"},
                                            {"materials", Json::array()}}})}};
}

std::string paintText() {
    return "{\n  \"schema\": \"pelican.material\",\n  \"version\": 1,\n  \"materials\": [\n"
           "    {\"name\": \"paint\", \"surface\": \"" +
           std::string{OpenPbr} + "\"}\n  ]\n}\n";
}

std::string readBack(const std::filesystem::path &file) {
    std::ifstream in{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

Json recovered() {
    return Json{{"committed", true},           {"recovered", true},
                {"path", Paint},               {"names", {"paint"}},
                {"rebuilt_models", {Cube}},    {"material_digest", "d1"},
                {"asset_data_digest", "a1"},   {"previous_asset_data_digest", "a1"},
                {"applied", "live"},           {"restart_required", nullptr}};
}

// engine EB-2: a registered document whose file the watch could not read.
Json staleProblem(int line) {
    return Json{{"id", "stale-" + std::to_string(line)},
                {"severity", "error"},
                {"source", "hot_reload"},
                {"code", "material.document_invalid@1"},
                {"message", "material.document_invalid@1: the material document 'materials/paint.material.json' "
                            "could not be read; the engine keeps drawing its materials as it read them before: "
                            "syntax error"},
                {"document", "project://materials/paint.material.json"},
                {"document_kind", "material"},
                {"line", line},
                {"column", 1},
                {"fields", {{"path", Paint}, {"reason", "syntax error"}, {"line", line}, {"column", 1}}}};
}

MaterialProblems problemsOf(const std::vector<Json> &problems) {
    std::vector<ProblemRow> rows;
    for (const auto &problem : problems)
        if (auto row = readEngineProblem(problem)) rows.push_back(std::move(*row));
    return materialProblemsOf(rows);
}

} // namespace

TEST_CASE("MR-1s decision 1: Save in Show the document takes a document refused at start in with "
          "register_material -- a refusal stays in the window at its line and column, recovered closes it",
          "[devstudio][materials][mr1s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = project(temporary);
    const auto fixed = paintText();
    write(root / "materials" / "paint.material.json", fixed.substr(0, fixed.size() - 2));
    FakeEngine engine;
    auto hooks = engine.dockHooks();
    int problem_reads = 0;
    hooks.problems_changed = [&] { ++problem_reads; };
    MaterialsDock dock{hooks};
    dock.show();
    dock.open(root);
    dock.engineAvailable();
    answer(dock, engine, "list_materials", refusedAtStart());
    answer(dock, engine, "list_surfaces", surfaces());
    REQUIRE(dock.revealMaterial(Paint, {}));

    named<QPushButton>(dock, QStringLiteral("pelican.materials.showDocument"))->click();
    QPointer<QDialog> dialog = named<QDialog>(dock, QStringLiteral("pelican.materials.documentDialog"));
    auto *text = named<QPlainTextEdit>(*dialog, QStringLiteral("pelican.materials.documentDialog.text"));
    auto *save = named<QPushButton>(*dialog, QStringLiteral("pelican.materials.documentDialog.save"));
    auto *notice = named<QLabel>(*dialog, QStringLiteral("pelican.materials.documentDialog.notice"));
    text->moveCursor(QTextCursor::End);
    text->insertPlainText(QStringLiteral("}\n"));
    REQUIRE(save->isEnabled());
    save->click();
    CHECK(readBack(root / "materials" / "paint.material.json") == fixed);

    // Saved: register_material of the document, the window open meanwhile.
    REQUIRE(engine.asked("register_material"));
    REQUIRE(dialog);
    CHECK(dialog->isVisible());
    CHECK(notice->text() == QStringLiteral("Saved; the engine takes materials/paint.material.json in\xE2\x80\xA6"));
    CHECK_FALSE(save->isEnabled());

    // Still not read: the parser's sentence, at its line and column, in the
    // window; it stays open and Save can go again.
    const auto first = refuse(
        dock, engine, "register_material", "material.document_invalid@1",
        QJsonObject{{QStringLiteral("path"), QString::fromLatin1(Paint)},
                    {QStringLiteral("reason"), QStringLiteral("[json.exception.parse_error.101] parse error at line 3, "
                                                              "column 5: syntax error while parsing value - invalid "
                                                              "literal")}});
    CHECK(first.params == Json{{"path", Paint}});
    REQUIRE(dialog);
    CHECK(dialog->isVisible());
    const QString why = QStringLiteral("materials/paint.material.json would not load at line 3, column 5: syntax "
                                       "error while parsing value - invalid literal (material.document_invalid@1).");
    CHECK(notice->text() == why);
    CHECK(save->isEnabled());
    CHECK(problem_reads == 0);
    answer(dock, engine, "list_materials", refusedAtStart());

    // Taken in (recovered): the window closes, the dock says so, the list
    // and the problems are read again and the material is the one shown.
    save->click();
    answer(dock, engine, "register_material", recovered());
    QApplication::processEvents();
    CHECK((dialog.isNull() || !dialog->isVisible()));
    CHECK(dock.noticeLine()->text() == QStringLiteral("paint is loaded again."));
    CHECK(problem_reads == 1);
    CHECK_FALSE(named<QPushButton>(dock, QStringLiteral("pelican.materials.restartEngine"))->isVisibleTo(&dock));
    answer(dock, engine, "list_materials", listResult());
    CHECK_FALSE(dock.session().broken());
    REQUIRE(dock.session().selection());
    CHECK(dock.session().selection()->name == "paint");
    CHECK(engine.restarts.empty());
}

TEST_CASE("MR-1s decision 2: Register again takes a document fixed in another editor in, the engine's watch "
          "taking it first reads as loaded, and a material problem that comes or goes reads the list again",
          "[devstudio][materials][mr1s]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = project(temporary);
    write(root / "materials" / "paint.material.json", paintText());
    FakeEngine engine;
    auto hooks = engine.dockHooks();
    int problem_reads = 0;
    hooks.problems_changed = [&] { ++problem_reads; };
    MaterialsDock dock{hooks};
    dock.show();
    dock.open(root);
    dock.engineAvailable();
    answer(dock, engine, "list_materials", refusedAtStart());
    answer(dock, engine, "list_surfaces", surfaces());
    REQUIRE(dock.revealMaterial(Paint, {}));

    // Fixed outside Studio: Register again, one register_material.
    auto *again = named<QPushButton>(dock, QStringLiteral("pelican.materials.registerAgain"));
    CHECK(again->isVisibleTo(&dock));
    again->click();
    const auto sent = answer(dock, engine, "register_material", recovered());
    CHECK(sent.params == Json{{"path", Paint}});
    CHECK(dock.noticeLine()->text() == QStringLiteral("paint is loaded again."));
    CHECK(problem_reads == 1);
    answer(dock, engine, "list_materials", listResult());
    REQUIRE(dock.session().selection());
    CHECK(dock.session().selection()->name == "paint");

    // The engine's start refused it again (another run); its watch took the
    // fixed file in before Register again arrived: listed already, and the
    // list says loaded -- loaded again, not a restart.
    dock.open(root);
    dock.engineAvailable();
    answer(dock, engine, "list_materials", refusedAtStart());
    answer(dock, engine, "list_surfaces", surfaces());
    REQUIRE(dock.revealMaterial(Paint, {}));
    named<QPushButton>(dock, QStringLiteral("pelican.materials.registerAgain"))->click();
    refuse(dock, engine, "register_material", "material.path_conflict@1",
           QJsonObject{{QStringLiteral("path"), QString::fromLatin1(Paint)}});
    answer(dock, engine, "list_materials", listResult());
    CHECK(dock.noticeLine()->text() == QStringLiteral("paint is loaded again."));
    CHECK(problem_reads == 2);
    CHECK(engine.restarts.empty());
    CHECK_FALSE(named<QPushButton>(dock, QStringLiteral("pelican.materials.restartEngine"))->isVisibleTo(&dock));

    // With the watch, the engine takes documents in by itself: a material
    // problem that comes or goes reads list_materials again (PB-1s).
    CHECK_FALSE(engine.asked("list_materials"));
    dock.observeProblems(problemsOf({staleProblem(4)}));
    CHECK(engine.asked("list_materials"));
    answer(dock, engine, "list_materials", listResult());
    dock.observeProblems(problemsOf({staleProblem(4)}));
    CHECK_FALSE(engine.asked("list_materials"));
    dock.observeProblems(problemsOf({}));
    CHECK(engine.asked("list_materials"));
}

TEST_CASE("MR-1s decision 3: a registered document the engine's watch refused after start is marked, its "
          "Problems row shows why at its line, and a Save the watch takes in reads as loaded again",
          "[devstudio][materials][mr1s]") {
    application();
    // The Problems row: source hot_reload, the document is its row in the
    // Materials dock, and the dock reads it as a file kept from before.
    const auto row = readEngineProblem(staleProblem(4));
    REQUIRE(row);
    CHECK(row->place == ProblemRow::Place::Material);
    CHECK(row->document == Paint);
    const auto problems = problemsOf({staleProblem(4)});
    REQUIRE(problems.stale.size() == 1);
    CHECK(problems.stale.front() ==
          MaterialFileProblem{.path = Paint, .code = "material.document_invalid@1", .reason = "syntax error",
                              .line = 4, .column = 1});
    // A start's refusal is not one (FO-1's, decision 1 and 2).
    CHECK(problemsOf({{{"id", "s"},
                       {"severity", "error"},
                       {"source", "startup"},
                       {"code", "material.document_invalid@1"},
                       {"document", "project://materials/paint.material.json"},
                       {"document_kind", "material"},
                       {"fields", {{"path", Paint}, {"reason", "x"}}}}})
              .stale.empty());

    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const auto root = project(temporary);
    const auto fixed = paintText();
    write(root / "materials" / "paint.material.json", fixed.substr(0, fixed.size() - 2));
    FakeEngine engine;
    MaterialsDock dock{engine.dockHooks()};
    dock.show();
    readyWithPaint(dock, engine, root);
    dock.observeProblems(problems);
    // list_materials reads the file itself: not JSON now.
    answer(dock, engine, "list_materials",
           Json{{"asset_data_digest", "a1"},
                {"documents", Json::array({{{"path", Paint},
                                            {"status", "invalid"},
                                            {"reason", "[json.exception.parse_error.101] parse error"},
                                            {"material_digest", "d9"},
                                            {"materials", Json::array()}}})}});
    QTreeWidgetItem *document = rowNamed(*dock.list(), QStringLiteral("paint.material.json"));
    REQUIRE(document != nullptr);
    CHECK(document->text(1) == QStringLiteral("the file on disk does not load; the engine keeps the last values"));

    REQUIRE(dock.revealMaterial(row->document, row->material));
    REQUIRE(dock.session().broken());
    CHECK(named<QLabel>(dock, QStringLiteral("pelican.materials.broken"))->text() ==
          QStringLiteral("materials/paint.material.json: the file on disk does not load; the engine keeps the last "
                         "values: line 4, column 1: syntax error"));
    CHECK_FALSE(named<QPushButton>(dock, QStringLiteral("pelican.materials.registerAgain"))->isVisibleTo(&dock));

    // Show the document at the watch's line; Save sends nothing -- the watch
    // reads the file.
    named<QPushButton>(dock, QStringLiteral("pelican.materials.showDocument"))->click();
    QPointer<QDialog> dialog = named<QDialog>(dock, QStringLiteral("pelican.materials.documentDialog"));
    auto *text = named<QPlainTextEdit>(*dialog, QStringLiteral("pelican.materials.documentDialog.text"));
    auto *save = named<QPushButton>(*dialog, QStringLiteral("pelican.materials.documentDialog.save"));
    auto *notice = named<QLabel>(*dialog, QStringLiteral("pelican.materials.documentDialog.notice"));
    CHECK(text->textCursor().blockNumber() == 3);
    text->moveCursor(QTextCursor::End);
    text->insertPlainText(QStringLiteral("}\n"));
    save->click();
    CHECK(readBack(root / "materials" / "paint.material.json") == fixed);
    CHECK_FALSE(engine.asked("register_material"));
    CHECK(dock.noticeLine()->text() ==
          QStringLiteral("Saved materials/paint.material.json; the engine's watch takes it in."));
    CHECK(notice->text() ==
          QStringLiteral("Saved; the engine's watch reads materials/paint.material.json again\xE2\x80\xA6"));

    // The watch refused it again, elsewhere: said in the window.
    dock.observeProblems(problemsOf({staleProblem(6)}));
    REQUIRE(dialog);
    CHECK(dialog->isVisible());
    CHECK(notice->text() == QStringLiteral("materials/paint.material.json: the file on disk does not load; the "
                                           "engine keeps the last values: line 6, column 1: syntax error"));
    answer(dock, engine, "list_materials", listResult());

    // Saved again, and the watch took it in: the problem went.
    save->click();
    dock.observeProblems(problemsOf({}));
    QApplication::processEvents();
    CHECK((dialog.isNull() || !dialog->isVisible()));
    CHECK(dock.noticeLine()->text() == QStringLiteral("paint is loaded again."));
    answer(dock, engine, "list_materials", listResult());
    CHECK_FALSE(dock.session().broken());
    REQUIRE(dock.session().selection());
    CHECK(dock.session().selection()->name == "paint");
    CHECK(engine.restarts.empty());
}

TEST_CASE("QA-1: the Material rows go with their object -- switched off and naming no model while the next "
          "object is read, so a wait for the rows to be read is not answered by the previous object's",
          "[devstudio][materials][qa1]") {
    application();
    FakeEngine engine;
    MaterialAssignWidget rows{engine.assignHooks()};
    readyCube(rows, engine);
    auto *target = named<QComboBox>(rows, QStringLiteral("pelican.inspectorMaterialTarget"));
    auto *choice = named<QComboBox>(rows, QStringLiteral("pelican.inspectorMaterialChoice"));
    CHECK(target->itemText(1) == QStringLiteral("Every Cube"));
    CHECK(target->isEnabled());
    CHECK(choice->isEnabled());

    // Another object picked in the Outliner: the Inspector drops what it had
    // read and asks for the new one, so for a while there is no subject.
    rows.setSubject({});
    CHECK(rows.isHidden());
    CHECK_FALSE(target->isEnabled());
    CHECK_FALSE(choice->isEnabled());
    CHECK(target->findText(QStringLiteral("Every Cube")) < 0);

    // The sphere's object is read: the rows name it at once, and can be
    // chosen in once its materials are read -- not before.
    constexpr const char *Sphere = "engine://primitives/sphere";
    rows.setSubject(subjectOf(Sphere, "Sphere", 8));
    CHECK_FALSE(rows.isHidden());
    CHECK(target->itemText(1) == QStringLiteral("Every Sphere"));
    CHECK_FALSE(choice->isEnabled());
    answer(rows, engine, "list_materials", listResult());
    Json bindings = cubeBindings(1);
    bindings["model"] = Sphere;
    answer(rows, engine, "get_material_bindings", bindings);
    CHECK_FALSE(choice->isEnabled());
    Json object = cubeObject();
    object["object"] = 8;
    object["model"] = Sphere;
    answer(rows, engine, "get_object_materials", object);
    CHECK(target->isEnabled());
    CHECK(choice->isEnabled());
}

TEST_CASE("QA-1: New material…'s surfaces follow an answer of as many surfaces but other ones",
          "[devstudio][materials][qa1]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    FakeEngine engine;
    MaterialsDock dock{engine.dockHooks()};
    dock.open(project(temporary));
    dock.engineAvailable();
    answer(dock, engine, "list_materials", listResult());
    answer(dock, engine, "list_surfaces", surfaces());

    // The person fixed the broken surface's file under another name while the
    // dock was open: the list is as long as before, and not the same.
    named<QPushButton>(dock, QStringLiteral("pelican.materials.newMaterial"))->click();
    auto *surface = named<QComboBox>(dock, QStringLiteral("pelican.materials.new.surface"));
    REQUIRE(surface->count() == 2);
    CHECK(surface->itemText(0) == QStringLiteral("shaders/broken"));
    Json fixed = surfaces();
    fixed["surfaces"][0] = Json{{"reference", "project://shaders/query.surface"},
                                {"source", "project"},
                                {"params", 0},
                                {"textures", 0}};
    answer(dock, engine, "list_surfaces", fixed);
    REQUIRE(surface->count() == 2);
    CHECK(surface->itemText(0) == QStringLiteral("shaders/query"));
    CHECK(qobject_cast<QStandardItemModel *>(surface->model())->item(0)->isEnabled());
    CHECK(surface->itemData(0, Qt::ToolTipRole).toString() == QStringLiteral("project://shaders/query.surface"));

    // What is chosen is what Create sends.
    surface->setCurrentIndex(0);
    named<QPushButton>(dock, QStringLiteral("pelican.materials.new.create"))->click();
    const auto sent = engine.take("create_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params.value("surface", std::string{}) == "project://shaders/query.surface");
}
