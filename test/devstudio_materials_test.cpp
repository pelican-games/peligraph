// MT-1s: the material editor -- the Materials dock (list_materials, the
// editor made from get_surface_schema, New material… on list_surfaces,
// Assign to selected object, Undo) and the Inspector's Material rows
// (get_material_bindings, bind_material) -- on the real widgets, with the
// test answering for the engine (MT-1, and MT-1b's list_surfaces) through
// the widgets' own hooks.

#include "../src/devstudio/model/inspectormodel.hpp"
#include "../src/devstudio/model/materialsmodel.hpp"
#include "graphcanvastagpicker.hpp"
#include "materialassignwidget.hpp"
#include "materialsdock.hpp"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
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
    rows.engineAvailable();
    CHECK(rows.isHidden());
    rows.setSubject(Cube, "Cube");
    answer(rows, engine, "list_materials", listResult());
    answer(rows, engine, "get_material_bindings",
           Json{{"model", Cube},
                {"material_bindings", nullptr},
                {"bindings_digest", nullptr},
                {"primitives",
                 Json::array({{{"mesh", 0},
                               {"primitive", 0},
                               {"slot", "pelican.primitive.default"},
                               {"material", "pelican.primitive.default"},
                               {"usd_path", nullptr}}})}});
    const Json scene{{"objects",
                      Json::array({{{"name", "Cube"},
                                    {"components", Json::array({{{"name", "simplemodelview"}, {"model", Cube}}})}},
                                   {{"name", "Cube_001"},
                                    {"components", Json::array({{{"name", "simplemodelview"}, {"model", Cube}}})}},
                                   {{"name", "Ball"},
                                    {"components", Json::array({{{"name", "simplemodelview"},
                                                                 {"model", "engine://primitives/sphere"}}})}}})}};
    const auto snapshot = answer(rows, engine, "export_scene_snapshot",
                                 Json{{"schema_version", 2}, {"semantic_scene_bytes", scene.dump()}});
    CHECK(snapshot.params == Json{{"schema_version", 2}, {"allow_pending", true}});
    CHECK_FALSE(rows.isHidden());
    auto *choice = named<QComboBox>(rows, QStringLiteral("pelican.inspectorMaterialChoice"));
    CHECK(choice->currentText() == QStringLiteral("Model default"));
    CHECK(named<QLabel>(rows, QStringLiteral("pelican.inspectorMaterialScope"))->text() ==
          QStringLiteral("Applies to every Cube in this project (2 objects)."));
    CHECK(rows.findChild<QWidget *>(QStringLiteral("pelican.inspectorMaterialPrimitives"))->isHidden());

    choice->setCurrentIndex(choice->findText(QStringLiteral("paint")));
    auto sent = engine.take("bind_material");
    REQUIRE(sent.has_value());
    CHECK(sent->params == Json{{"model", Cube}, {"material", "paint"}});

    // A model of two primitives: a combo each, which binds that one.
    FakeEngine ship_engine;
    MaterialAssignWidget ship{ship_engine.assignHooks()};
    ship.engineAvailable();
    ship.setSubject("ship", "Ship");
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
    rows.engineAvailable();
    rows.setSubject(Cube, "Cube");
    answer(rows, inspector_engine, "list_materials", listResult());
    answer(rows, inspector_engine, "get_material_bindings",
           Json{{"model", Cube},
                {"primitives", Json::array({{{"mesh", 0},
                                             {"primitive", 0},
                                             {"slot", "pelican.primitive.default"},
                                             {"material", "pelican.primitive.default"}}})}});
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
