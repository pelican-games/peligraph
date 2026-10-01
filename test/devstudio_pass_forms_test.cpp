// UX-4a. Each pass form on its own: a declaration's value goes in, the same
// JSON comes out when nothing is touched -- keys the form does not show
// included -- and a choice made in the form is the change the JSON shows.

#include "passforms/attachmentopsform.hpp"
#include "passforms/materialroutingform.hpp"
#include "passforms/materialsettingsform.hpp"
#include "passforms/rasterstateform.hpp"
#include "passforms/shaderdefinesform.hpp"

#include "rasterpass.hpp"
#include "renderingpass/passshaderdefines.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>

#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;
using namespace PelicanStudio;

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_pass_forms_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

const QString prefix = QStringLiteral("test");

template <typename Widget>
Widget &required(QWidget &form, const QString &suffix) {
    auto *result = form.findChild<Widget *>(prefix + QLatin1Char('.') + suffix);
    if (result == nullptr)
        throw std::runtime_error("missing widget " + suffix.toStdString());
    return *result;
}

void choose(QComboBox &combo, const QString &data) {
    const int index = combo.findData(data);
    if (index < 0) throw std::runtime_error("no entry " + data.toStdString());
    combo.setCurrentIndex(index);
}

struct ChangeCounter {
    int count = 0;
    explicit ChangeCounter(PassForm &form) {
        QObject::connect(&form, &PassForm::changed, [this] { ++count; });
    }
};

} // namespace

TEST_CASE("UX-4a RasterStateForm gives back the raster_state and draw it was given, "
          "and a choice is the key it names",
          "[devstudio][pass-forms][ux4a]") {
    (void)application();
    RasterStateForm form{prefix};
    ChangeCounter changes{form};
    form.setDrawShown(true);
    form.setColorAttachments({"albedo", "normal"});
    const Json custom_blend{
        {"color", {{"src", "one"}, {"dst", "one"}, {"op", "add"}}},
        {"alpha", {{"src", "one"}, {"dst", "zero"}, {"op", "add"}}}};
    const Json state{{"cull", "none"},
                     {"depth_test", true},
                     {"depth_compare", "greater_equal"},
                     {"future_field", 7},
                     {"color_attachments",
                      Json::array({{{"blend", custom_blend}}, {{"write_mask", "rg"}}})}};
    const Json draw{{"vertex_count", 3}, {"first_instance", 2}, {"implementation", "test.custom@1"}};
    form.load(state, draw);
    CHECK(changes.count == 0);
    // Read and given back unchanged: the same JSON, the unknown key too.
    CHECK(form.rasterState() == state);
    CHECK(form.draw() == draw);

    choose(required<QComboBox>(form, "rasterState.topology"), "line_list");
    CHECK(changes.count == 1);
    choose(required<QComboBox>(form, "rasterState.cull"), "");
    choose(required<QComboBox>(form, "rasterState.depthWrite"), "false");
    choose(required<QComboBox>(form, "rasterState.blend.1"), "additive");
    choose(required<QComboBox>(form, "rasterState.writeMask.1"), "");
    required<QSpinBox>(form, "draw.instanceCount").setValue(4);
    Json expected = state;
    expected["topology"] = "line_list";
    expected.erase("cull");
    expected["depth_write"] = false;
    expected["color_attachments"][1] = {{"blend", "additive"}};
    CHECK(form.rasterState() == expected);
    CHECK(form.draw() == Json{{"vertex_count", 3}, {"first_instance", 2},
                              {"implementation", "test.custom@1"}, {"instance_count", 4}});
    // The engine's own reader takes what the form wrote.
    Json pass{{"raster_state", form.rasterState()}, {"draw", form.draw()}};
    pass["raster_state"].erase("future_field");
    CHECK_NOTHROW(Pelican::parseRasterPassContract(pass, 2, true, "ux4a"));

    // Indexed: the index buffer is chosen, the direct counts leave.
    form.setIndexBufferCandidates({"indices"});
    choose(required<QComboBox>(form, "draw.operation"), "indexed");
    required<QComboBox>(form, "draw.indexBuffer").setCurrentIndex(0);
    required<QSpinBox>(form, "draw.indexCount").setValue(36);
    CHECK(form.draw() == Json{{"operation", "indexed"}, {"index_buffer", "indices"},
                              {"index_count", 36}, {"first_instance", 2},
                              {"implementation", "test.custom@1"}, {"instance_count", 4}});

    // Every choice back to Default: nothing is left but what the form does
    // not know.
    form.load(Json{{"cull", "back"}}, nullptr);
    choose(required<QComboBox>(form, "rasterState.cull"), "");
    CHECK(form.rasterState().is_null());

    // A fullscreen pass: no depth rows, no draw, one attachment entry.
    RasterStateForm fullscreen{prefix};
    fullscreen.setDepthShown(false);
    fullscreen.setColorAttachments({"Output"});
    CHECK(required<QComboBox>(fullscreen, "rasterState.depthTest").isHidden());
    CHECK(fullscreen.draw().is_null());
    choose(required<QComboBox>(fullscreen, "rasterState.blend.0"), "additive");
    CHECK(fullscreen.rasterState() ==
          Json{{"color_attachments", Json::array({{{"blend", "additive"}}})}});
    CHECK_NOTHROW(Pelican::parseFullscreenRasterFixedFunctionState(
        Json{{"raster_state", fullscreen.rasterState()}}));
}

TEST_CASE("UX-4a AttachmentOpsForm keeps output.color, clear_colors and the depth "
          "output as given, and writes the ops chosen",
          "[devstudio][pass-forms][ux4a]") {
    (void)application();
    AttachmentOpsForm form{prefix};
    ChangeCounter changes{form};
    form.setDepthOpsShown(true);
    const Json color = Json::array(
        {{{"target", "lit"}, {"load_op", "Clear"}, {"subresource", {{"mip", 0}}}}, "bloom"});
    const Json clear{{"lit", Json::array({0.0, 0.0, 0.5, 1.0})}};
    const Json depth{{"target", "scene_depth"}, {"load_op", "Load"}};
    form.load(color, clear, depth);
    CHECK(changes.count == 0);
    CHECK(form.color() == color);
    CHECK(form.clearColors() == clear);
    CHECK(form.depth() == depth);
    CHECK(required<QComboBox>(form, "outputLoad.0").currentData().toString() == "clear");
    CHECK(required<QComboBox>(form, "depthLoad").currentData().toString() == "load");

    // The outputs change: one that stays keeps its fields, a new one is a name.
    form.setColorTargets({"bloom", "lit", "extra"});
    CHECK(form.color() == Json::array({"bloom", color.at(0), "extra"}));
    choose(required<QComboBox>(form, "outputLoad.0"), "clear");
    required<QLineEdit>(form, "outputClearColor.0").setText("1, 0.5, 0");
    choose(required<QComboBox>(form, "outputStore.2"), "dont_care");
    CHECK(changes.count == 3);
    CHECK(form.color() ==
          Json::array({{{"target", "bloom"}, {"load_op", "clear"}}, color.at(0),
                       {{"target", "extra"}, {"store_op", "dont_care"}}}));
    CHECK(form.clearColors() == Json{{"lit", Json::array({0.0, 0.0, 0.5, 1.0})},
                                     {"bloom", Json::array({1.0, 0.5, 0.0, 1.0})}});
    // Back to Default: the entry is the output's name again.
    choose(required<QComboBox>(form, "outputStore.2"), "");
    CHECK(form.color().at(2) == "extra");

    // The depth output: retargeted, its fields stay; Store chosen.
    form.setDepthTarget("new_depth");
    choose(required<QComboBox>(form, "depthStore"), "store");
    CHECK(form.depth() ==
          Json{{"target", "new_depth"}, {"load_op", "Load"}, {"store_op", "store"}});
    form.setDepthTarget("");
    CHECK(form.depth().is_null());

    required<QLineEdit>(form, "outputClearColor.0").setText("red");
    CHECK_THROWS(form.clearColors());

    // One attachment object, not a list, stays one object.
    const Json single{{"target", "lit"}, {"load_op", "load"}};
    form.load(single, nullptr, "scene_depth");
    CHECK(form.color() == single);
    CHECK(form.depth() == "scene_depth");
}

TEST_CASE("UX-4a ShaderDefinesForm is shader.defines a row each, and refuses what "
          "the engine refuses in its words",
          "[devstudio][pass-forms][ux4a]") {
    (void)application();
    ShaderDefinesForm form{prefix};
    ChangeCounter changes{form};
    form.setPassName("tint");
    const Json defines = Json::array({"USE_FOG", "SAMPLES=4", "EMPTY="});
    form.load(defines);
    CHECK(changes.count == 0);
    CHECK(form.defines() == defines);
    CHECK(form.refusal().isEmpty());
    auto &table = required<QTableWidget>(form, "shaderDefines.table");
    REQUIRE(table.rowCount() == 3);
    CHECK(table.item(1, 0)->text() == "SAMPLES");
    CHECK(table.item(1, 1)->text() == "4");

    table.item(1, 1)->setText("8");
    CHECK(form.defines() == Json::array({"USE_FOG", "SAMPLES=8", "EMPTY="}));

    // A name the engine defines itself: the row is refused, as the engine
    // would refuse it, in the engine's sentence.
    required<QPushButton>(form, "shaderDefines.add").click();
    table.item(3, 0)->setText("PELICAN_VIEW_COUNT");
    table.item(3, 1)->setText("2");
    const Json refused = form.defines();
    REQUIRE(refused.size() == 4);
    std::string engine_sentence;
    try {
        (void)Pelican::parsePassShaderDefines(Json{{"defines", refused}}, "tint");
    } catch (const Pelican::PassShaderDefineError &error) {
        engine_sentence = error.what();
        CHECK(error.info().reason == Pelican::passShaderDefineReserved);
    }
    REQUIRE_FALSE(engine_sentence.empty());
    CHECK(form.refusal().toStdString() == engine_sentence);
    CHECK(form.rowRefusal(3).toStdString() == engine_sentence);
    CHECK(form.rowRefusal(0).isEmpty());

    // Twice the same name, and a name that is not one: the engine's too.
    for (const auto *name : {"USE_FOG", "1BAD"}) {
        table.item(3, 0)->setText(QString::fromLatin1(name));
        std::string sentence;
        try {
            (void)Pelican::parsePassShaderDefines(Json{{"defines", form.defines()}}, "tint");
        } catch (const Pelican::PassShaderDefineError &error) {
            sentence = error.what();
        }
        REQUIRE_FALSE(sentence.empty());
        CHECK(form.refusal().toStdString() == sentence);
    }

    table.setCurrentCell(3, 0);
    required<QPushButton>(form, "shaderDefines.remove").click();
    CHECK(form.defines() == Json::array({"USE_FOG", "SAMPLES=8", "EMPTY="}));
    CHECK(form.refusal().isEmpty());

    // An entry that is not a string is kept as it is (and refused).
    form.load(Json::array({"A", 42}));
    CHECK(form.defines() == Json::array({"A", 42}));
    CHECK_FALSE(form.refusal().isEmpty());
    form.load(nullptr);
    CHECK(form.defines().is_null());
}

TEST_CASE("UX-4a MaterialSettingsForm gives back the ten settings it was given, "
          "and the rows are the outputs and bindings",
          "[devstudio][pass-forms][ux4a]") {
    (void)application();
    MaterialSettingsForm form{prefix};
    ChangeCounter changes{form};
    form.setResourceCandidates({"opaque_color", "opaque_depth", "lut"});
    form.setVariantCandidates({"shadow"});
    const Json declaration{
        {"name", "forward"},
        {"type", "material"},
        {"output", {{"color", Json::array({"lit"})}, {"depth", "depth"}}},
        {"material_contract", "forward_opaque_v1"},
        {"material_filter", {{"include", {"hero", "prop"}}, {"future", true}}},
        {"material_variant", "shadow"},
        {"material_range", {{"start", 2}, {"count", 3}}},
        {"material_outputs",
         {{"schema", "pelican.material_outputs"},
          {"version", 1},
          {"name", "two"},
          {"outputs",
           Json::array({{{"name", "color"}, {"type", "vec4"}, {"source", "custom"}},
                        {{"name", "id"}, {"type", "uint"}, {"source", "custom"}}})}}},
        {"material_output_states", {{"color", {{"blend", "blend"}}}}},
        {"screen_inputs",
         {{"opaque_color", "opaque_color"},
          {"scene_depth", {{"resource", "opaque_depth"}, {"footprint", "point"}}}}},
        {"material_resources", {{"lut", "lut"}}},
        {"gpu_draw_source", {{"commands", "cmd"}, {"count", "cnt"}, {"max_draw_count", 8}}},
    };
    form.load(declaration);
    CHECK(changes.count == 0);
    Json expected = Json::object();
    for (const auto &key : MaterialSettingsForm::keys())
        if (declaration.contains(key)) expected[key] = declaration.at(key);
    CHECK(form.settings() == expected);

    // Contract, tags, variant.
    choose(required<QComboBox>(form, "materialContract"), "forward_transparent_v1");
    required<QLineEdit>(form, "materialFilter.exclude").setText("decal");
    required<QComboBox>(form, "materialVariant").setEditText("");
    expected["material_contract"] = "forward_transparent_v1";
    expected["material_filter"]["exclude"] = {"decal"};
    expected.erase("material_variant");
    // An output's blend and write mask, and its type.
    choose(required<QComboBox>(form, "materialOutputs.0.blend"), "");
    choose(required<QComboBox>(form, "materialOutputs.1.writeMask"), "r");
    choose(required<QComboBox>(form, "materialOutputs.1.type"), "uvec2");
    expected["material_outputs"]["outputs"][1]["type"] = "uvec2";
    expected["material_output_states"] = {{"id", {{"write_mask", "r"}}}};
    // A binding read from another resource; the object's fields stay.
    required<QComboBox>(form, "materialResources.1.resource").setEditText("lut");
    expected["screen_inputs"]["scene_depth"]["resource"] = "lut";
    CHECK(changes.count > 0);
    CHECK(form.settings() == expected);

    // Removing a row and the range.
    required<QToolButton>(form, "materialResources.2.remove").click();
    expected.erase("material_resources");
    required<QCheckBox>(form, "materialRange").setChecked(false);
    expected.erase("material_range");
    CHECK(form.settings() == expected);

    // Other is JSON, and only for the settings with no fields.
    required<QPlainTextEdit>(form, "materialOther").setPlainText(R"({"material_variant":"x"})");
    CHECK_THROWS(form.settings());
    required<QPlainTextEdit>(form, "materialOther").clear();
    expected.erase("gpu_draw_source");
    CHECK(form.settings() == expected);

    // An empty pass: Add output starts a schema with one row.
    form.load(Json{{"type", "material"}});
    CHECK(form.settings() == Json::object());
    required<QPushButton>(form, "materialOutputs.add").click();
    const Json added = form.settings();
    CHECK(added.at("material_outputs").at("schema") == "pelican.material_outputs");
    CHECK(added.at("material_outputs").at("outputs").size() == 1);
    required<QPushButton>(form, "materialResources.add").click();
    CHECK(form.settings().at("screen_inputs") == Json{{"opaque_color", ""}});
}

TEST_CASE("UX-4a MaterialRoutingForm is the three routes, each a material pass",
          "[devstudio][pass-forms][ux4a]") {
    (void)application();
    MaterialRoutingForm form{prefix};
    ChangeCounter changes{form};
    form.setPassCandidates({{"geometry", "deferred_geometry_v1"},
                            {"opaque", "forward_opaque_v1"},
                            {"transparent", "forward_transparent_v1"},
                            {"other", ""}});
    const Json routing{{"policy", "hybrid_auto_v1"},
                       {"routes",
                        {{"deferred_geometry", "geometry"},
                         {"forward_opaque", "opaque"},
                         {"forward_transparent", "transparent"}}},
                       {"note", "kept"}};
    form.load(routing);
    CHECK(changes.count == 0);
    CHECK(form.routing() == routing);

    choose(required<QComboBox>(form, "materialRouting.forward_opaque"), "other");
    Json expected = routing;
    expected["routes"]["forward_opaque"] = "other";
    CHECK(form.routing() == expected);
    CHECK(changes.count == 1);
    required<QCheckBox>(form, "materialRouting.enabled").setChecked(false);
    CHECK(form.routing().is_null());

    // No routing yet: turning it on fills each route with its contract's pass.
    form.load(nullptr);
    CHECK(form.routing().is_null());
    required<QCheckBox>(form, "materialRouting.enabled").setChecked(true);
    CHECK(form.routing() == Json{{"policy", "hybrid_auto_v1"},
                                 {"routes",
                                  {{"deferred_geometry", "geometry"},
                                   {"forward_opaque", "opaque"},
                                   {"forward_transparent", "transparent"}}}});
}
