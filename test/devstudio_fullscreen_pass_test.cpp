#include "fullscreenpasswidget.hpp"
#include "mainwindow.hpp"

#include "passfieldownership.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QWidget>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef PELICAN_TEST_SOURCE_DIR
#error "PELICAN_TEST_SOURCE_DIR must name the repository root"
#endif

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;
using StringSet = std::set<std::string, std::less<>>;

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_fullscreen_pass_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("could not read " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

Json readJson(const std::filesystem::path &path) {
    return Json::parse(readText(path));
}

template <typename Widget>
Widget &required(FullscreenPassWidget &form, const char *object_name) {
    auto *result = form.findChild<Widget *>(QString::fromLatin1(object_name));
    if (result == nullptr) {
        throw std::runtime_error(std::string{"missing widget "} + object_name);
    }
    return *result;
}

const QWidget &requiredWidget(const FullscreenPassWidget &form,
                              const char *object_name) {
    auto *result = form.findChild<QWidget *>(QString::fromLatin1(object_name));
    if (result == nullptr) {
        throw std::runtime_error(std::string{"missing widget "} + object_name);
    }
    return *result;
}

struct RefreshHarness {
    bool available = true;
    qint64 next_id = 40;
    std::vector<qint64> requests;

    FramePlanRefreshDriver driver() {
        return FramePlanRefreshDriver{
            .ready = [this] { return available; },
            .request = [this](QString *) {
                requests.push_back(++next_id);
                return requests.back();
            },
        };
    }
};

Json minimalFramePlan(std::string graph, const Json &resources,
                      std::uint64_t generation = 1) {
    return Json{
        {"schema", "pelican.frame_plan"},
        {"version", 1},
        {"graph", std::move(graph)},
        {"runtime_generation", generation},
        {"nodes", Json::array()},
        {"barriers", Json::array()},
        {"resources", resources},
    };
}

Json resource(std::string name, std::string kind = "render_target",
              std::string source = "project") {
    return Json{
        {"name", std::move(name)},
        {"kind", std::move(kind)},
        {"source", std::move(source)},
    };
}

StringSet resourceNames(const Json &plan) {
    StringSet result;
    for (const auto &entry : plan.at("resources")) {
        result.insert(entry.at("name").get<std::string>());
    }
    return result;
}

StringSet comboNames(const QComboBox &combo) {
    StringSet result;
    for (int index = 0; index < combo.count(); ++index) {
        result.insert(combo.itemText(index).toStdString());
    }
    return result;
}

void chooseCandidate(FullscreenPassWidget &form, const char *combo_name,
                     const char *button_name, std::string_view target) {
    auto &combo = required<QComboBox>(form, combo_name);
    const int index = combo.findText(
        QString::fromUtf8(target.data(), static_cast<qsizetype>(target.size())),
        Qt::MatchExactly);
    if (index < 0) {
        throw std::runtime_error("target choice is absent: " +
                                 std::string{target});
    }
    combo.setCurrentIndex(index);
    required<QPushButton>(form, button_name).click();
}

void setDepth(FullscreenPassWidget &form, const Json &depth) {
    auto &combo = required<QComboBox>(
        form, "pelican.fullscreenPass.depthTarget");
    if (depth.is_null()) {
        combo.setCurrentIndex(-1);
        return;
    }
    const int index = combo.findText(
        QString::fromStdString(depth.get<std::string>()), Qt::MatchExactly);
    if (index < 0) {
        throw std::runtime_error("depth target choice is absent");
    }
    combo.setCurrentIndex(index);
}

void driveFromDeclaration(FullscreenPassWidget &form,
                          const Json &declaration) {
    required<QLineEdit>(form, "pelican.fullscreenPass.name")
        .setText(QString::fromStdString(
            declaration.at("name").get<std::string>()));
    required<QLineEdit>(form, "pelican.fullscreenPass.shader.vertex")
        .setText(QString::fromStdString(
            declaration.at("shader").at("vertex").get<std::string>()));
    required<QLineEdit>(form, "pelican.fullscreenPass.shader.fragment")
        .setText(QString::fromStdString(
            declaration.at("shader").at("fragment").get<std::string>()));
    for (const auto &input : declaration.at("input")) {
        chooseCandidate(form, "pelican.fullscreenPass.inputTarget",
                        "pelican.fullscreenPass.addInput",
                        input.get_ref<const std::string &>());
    }
    for (const auto &color : declaration.at("output").at("color")) {
        chooseCandidate(form, "pelican.fullscreenPass.colorTarget",
                        "pelican.fullscreenPass.addColor",
                        color.get_ref<const std::string &>());
    }
    setDepth(form, declaration.at("output").at("depth"));
    QApplication::processEvents();
}

Json outputJson(FullscreenPassWidget &form) {
    return Json::parse(
        required<QPlainTextEdit>(form, "pelican.fullscreenPass.json")
            .toPlainText()
            .toStdString());
}

StringSet reportedOmissions(const FullscreenPassWidget &form) {
    const auto &label = requiredWidget(
        form, "pelican.fullscreenPass.omittedKeys");
    StringSet result;
    for (const auto &key :
         label.property("pelicanOmittedKeys").toStringList()) {
        result.insert(key.toStdString());
    }
    return result;
}

StringSet actualDifference(const Json &declaration, const Json &projection) {
    StringSet result;
    for (const auto &[key, value] : declaration.items()) {
        (void)value;
        if (!projection.contains(key)) {
            result.insert(key);
        }
    }
    return result;
}

std::string stripCMakeComments(const std::string &text) {
    std::string result;
    result.reserve(text.size());
    bool in_comment = false;
    for (const char character : text) {
        if (character == '#') {
            in_comment = true;
        } else if (character == '\n') {
            in_comment = false;
        }
        if (!in_comment) {
            result.push_back(character);
        }
    }
    return result;
}

StringSet formOwnedKeysFromAuthority(const FullscreenPassWidget &form) {
    StringSet any_type_owned;
    StringSet fullscreen_owned;
    for (const auto &entry : Pelican::passFieldOwnershipTable()) {
        for (const auto field : entry.fields) {
            any_type_owned.insert(std::string{field});
            if (entry.type == Pelican::RenderPassType::fullscreen) {
                fullscreen_owned.insert(std::string{field});
            }
        }
    }

    StringSet result;
    for (const QWidget *widget : form.findChildren<QWidget *>()) {
        const QString field_value =
            widget->property("pelicanPassField").toString();
        if (field_value.isEmpty()) {
            continue;
        }
        const std::string field = field_value.toStdString();
        if (!any_type_owned.contains(field) ||
            fullscreen_owned.contains(field)) {
            result.insert(field);
        }
    }
    return result;
}

Json projectToKeys(const Json &declaration, const StringSet &keys) {
    Json result = Json::object();
    for (const auto &key : keys) {
        if (declaration.contains(key)) {
            result[key] = declaration.at(key);
        }
    }
    return result;
}

const Json &findPass(const Json &config, std::string_view graph,
                     std::string_view name) {
    for (const auto &graph_entry : config.at("rendering_passes")) {
        if (std::string_view{
                graph_entry.at("name").get_ref<const std::string &>()} !=
            graph) {
            continue;
        }
        for (const auto &pass : graph_entry.at("passes")) {
            if (std::string_view{
                    pass.at("name").get_ref<const std::string &>()} == name) {
                return pass;
            }
        }
    }
    throw std::runtime_error("pass not found in real rendering config");
}

std::string axisState(const FullscreenPassWidget &form,
                      const char *object_name) {
    return requiredWidget(form, object_name)
        .property("pelicanValidationState")
        .toString()
        .toStdString();
}

QString axisText(const FullscreenPassWidget &form, const char *object_name) {
    const auto *label = qobject_cast<const QLabel *>(
        &requiredWidget(form, object_name));
    if (label == nullptr) {
        throw std::runtime_error("validation axis is not a label");
    }
    return label->text();
}

void driveMinimalDraft(FullscreenPassWidget &form, std::string_view name,
                       std::string_view fragment, std::string_view input,
                       std::string_view color) {
    required<QLineEdit>(form, "pelican.fullscreenPass.name")
        .setText(QString::fromUtf8(name.data(),
                                   static_cast<qsizetype>(name.size())));
    required<QLineEdit>(form, "pelican.fullscreenPass.shader.vertex")
        .setText(QStringLiteral("engine://fullscreen"));
    required<QLineEdit>(form, "pelican.fullscreenPass.shader.fragment")
        .setText(QString::fromUtf8(fragment.data(),
                                   static_cast<qsizetype>(fragment.size())));
    chooseCandidate(form, "pelican.fullscreenPass.inputTarget",
                    "pelican.fullscreenPass.addInput", input);
    chooseCandidate(form, "pelican.fullscreenPass.colorTarget",
                    "pelican.fullscreenPass.addColor", color);
    QApplication::processEvents();
}

struct ProjectionObservation {
    Json output;
    StringSet reported_omissions;
};

ProjectionObservation unawarePartialForm(Json selected_output) {
    // This is the required ineffective side of the contrast: it can emit the
    // same selected JSON but has no source-declaration awareness and therefore
    // falsely reports an empty difference.
    return ProjectionObservation{
        .output = std::move(selected_output),
        .reported_omissions = {},
    };
}

QStringList projectStatusIgnoringShaderCache() {
    QProcess process;
    process.setWorkingDirectory(QString::fromUtf8(PELICAN_TEST_SOURCE_DIR));
    process.start(QStringLiteral("git"),
                  {QStringLiteral("status"), QStringLiteral("--short"),
                   QStringLiteral("--ignored"), QStringLiteral("--"),
                   QStringLiteral("projects")});
    if (!process.waitForFinished(30000) || process.exitCode() != 0) {
        throw std::runtime_error(
            "git status --ignored failed: " +
            process.readAllStandardError().toStdString());
    }
    QStringList result;
    for (const auto &line : QString::fromUtf8(process.readAllStandardOutput())
                                .split('\n', Qt::SkipEmptyParts)) {
        if (!line.contains(
                QStringLiteral("/.pelican/shader_cache/"))) {
            result.push_back(line.trimmed());
        }
    }
    result.sort();
    return result;
}

} // namespace

TEST_CASE(
    "WP321a form projects real fullscreen passes and reports the dynamic omitted-key difference",
    "[devstudio][fullscreen-pass][wp321a][projection][negative-contrast]") {
    (void)application();
    const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const auto config_path = root / "projects" / "example" / "passes" /
                             "main_rendering_config.json";
    const Json config = readJson(config_path);
    const QByteArray frame_plan = QByteArray::fromStdString(readText(
        root / "test" / "fixtures" / "devstudio" /
        "example_frame_plan.json"));
    const std::string graph =
        config.at("rendering_passes").at(0).at("name").get<std::string>();

    RefreshHarness refresh;
    for (const std::string_view pass_name :
         std::array{std::string_view{"ssao_pass"},
                    std::string_view{"UpsampleBlend_3"}}) {
        const Json &declaration = findPass(config, graph, pass_name);
        FullscreenPassWidget form{refresh.driver()};
        form.openProjectReadOnly(root / "projects" / "example");
        form.receiveResult(frame_plan);
        driveFromDeclaration(form, declaration);

        const Json output = outputJson(form);
        const StringSet owned_keys = formOwnedKeysFromAuthority(form);
        REQUIRE(output == projectToKeys(declaration, owned_keys));
        const StringSet expected_difference =
            actualDifference(declaration, output);
        REQUIRE_FALSE(expected_difference.empty());
        REQUIRE(reportedOmissions(form) == expected_difference);

        const ProjectionObservation unaware = unawarePartialForm(output);
        REQUIRE(unaware.output == output);
        REQUIRE(unaware.reported_omissions.empty());
        REQUIRE(unaware.reported_omissions != reportedOmissions(form));

        // Existing-name collision is a separate axis and must not suppress the
        // central JSON projection observation.
        REQUIRE(axisState(
                    form,
                    "pelican.fullscreenPass.axis.nameCollision") ==
                "invalid");
        REQUIRE_FALSE(output.empty());
    }
}

TEST_CASE(
    "WP321a every target chooser is exactly the current frame plan resources set",
    "[devstudio][fullscreen-pass][wp321a][resources][negative-contrast]") {
    (void)application();
    const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const Json captured = readJson(root / "test" / "fixtures" /
                                   "devstudio" /
                                   "example_frame_plan.json");
    const Json synthetic = minimalFramePlan(
        "synthetic_graph",
        Json::array({resource("synthetic_input"),
                     resource("synthetic_color"),
                     resource("synthetic_frame", "frame_target", "engine")}));
    REQUIRE(resourceNames(captured) != resourceNames(synthetic));

    RefreshHarness refresh;
    FullscreenPassWidget form{refresh.driver()};
    for (const Json *plan : std::array{&captured, &synthetic}) {
        form.receiveResult(QByteArray::fromStdString(plan->dump()));
        const StringSet expected = resourceNames(*plan);
        REQUIRE(comboNames(required<QComboBox>(
                    form, "pelican.fullscreenPass.inputTarget")) ==
                expected);
        REQUIRE(comboNames(required<QComboBox>(
                    form, "pelican.fullscreenPass.colorTarget")) ==
                expected);
        REQUIRE(comboNames(required<QComboBox>(
                    form, "pelican.fullscreenPass.depthTarget")) ==
                expected);
    }
}

TEST_CASE(
    "WP321a names every unchecked validation axis and never predicts core-only rejection",
    "[devstudio][fullscreen-pass][wp321a][unchecked][negative-contrast]") {
    (void)application();
    const Json plan = minimalFramePlan(
        "validation_graph",
        Json::array({resource("safe_input"), resource("safe_output"),
                     resource("offscreen_depth"), resource("opaque_color"),
                     resource("swapchain", "frame_target", "engine")}));

    const auto make_form = [&] {
        auto refresh = std::make_unique<RefreshHarness>();
        auto form = std::make_unique<FullscreenPassWidget>(refresh->driver());
        form->receiveResult(QByteArray::fromStdString(plan.dump()));
        return std::pair{std::move(refresh), std::move(form)};
    };

    for (const std::string_view fragment :
         std::array{std::string_view{"engine://ssao.frag"},
                    std::string_view{"engine://taa_resolve"}}) {
        auto [refresh, form] = make_form();
        driveMinimalDraft(*form, "shader_case", fragment, "safe_input",
                          "safe_output");
        REQUIRE(axisState(
                    *form,
                    "pelican.fullscreenPass.axis.shaderResolution") ==
                "not_checked");
        REQUIRE(axisText(
                    *form,
                    "pelican.fullscreenPass.axis.shaderResolution")
                    .contains(QStringLiteral("Shader stem resolution: Not checked")));
        REQUIRE(axisState(
                    *form,
                    "pelican.fullscreenPass.axis.fieldOwnership") ==
                "valid");
    }

    for (const auto &[input, color] :
         std::array{std::pair{std::string_view{"offscreen_depth"},
                              std::string_view{"safe_output"}},
                    std::pair{std::string_view{"safe_input"},
                              std::string_view{"opaque_color"}}}) {
        auto [refresh, form] = make_form();
        driveMinimalDraft(*form, "usage_case", "engine://fullscreen", input,
                          color);
        REQUIRE(axisState(
                    *form,
                    "pelican.fullscreenPass.axis.targetUsage") ==
                "not_checked");
        REQUIRE(axisText(*form,
                         "pelican.fullscreenPass.axis.targetUsage")
                    .contains(QStringLiteral(
                        "Target usage compatibility: Not checked")));
        REQUIRE(axisState(
                    *form,
                    "pelican.fullscreenPass.axis.targetNames") ==
                "valid");
    }

    auto [refresh, swapchain_form] = make_form();
    driveMinimalDraft(*swapchain_form, "frame_target_case",
                      "engine://fullscreen", "swapchain", "safe_output");
    REQUIRE(axisState(
                *swapchain_form,
                "pelican.fullscreenPass.axis.targetNames") == "invalid");
    REQUIRE(axisText(
                *swapchain_form,
                "pelican.fullscreenPass.axis.targetNames")
                .contains(QStringLiteral("frame_target")));

    const std::array unchecked_axes{
        "pelican.fullscreenPass.axis.targetUsage",
        "pelican.fullscreenPass.axis.generationOrder",
        "pelican.fullscreenPass.axis.shaderResolution",
    };
    QStringList displayed;
    QStringList falsely_all_checked;
    for (const char *axis : unchecked_axes) {
        REQUIRE(axisState(*swapchain_form, axis) == "not_checked");
        const QString actual = axisText(*swapchain_form, axis);
        REQUIRE(actual.contains(QStringLiteral("Not checked")));
        displayed.push_back(actual);
        QString false_display = actual;
        false_display.replace(QStringLiteral("Not checked"),
                              QStringLiteral("Valid"));
        falsely_all_checked.push_back(false_display);
    }
    REQUIRE(displayed != falsely_all_checked);
}

TEST_CASE(
    "WP321a production MainWindow constructs the form from a pelican_studio source",
    "[devstudio][fullscreen-pass][wp321a][production-wiring]") {
    (void)application();
    MainWindow window;
    auto *child = window.findChild<QWidget *>(
        QStringLiteral("pelican.fullscreenPass"));
    REQUIRE(child != nullptr);
    REQUIRE(dynamic_cast<FullscreenPassWidget *>(child) != nullptr);

    // Comments must be stripped before searching: a plain substring scan
    // accepts "# fullscreenpasswidget.cpp", which is not wired into the
    // studio at all.
    const std::string cmake = stripCMakeComments(readText(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "src" /
        "devstudio" / "view" / "CMakeLists.txt"));
    const auto target_sources =
        cmake.find("target_sources(pelican_studio PRIVATE");
    REQUIRE(target_sources != std::string::npos);
    const auto end = cmake.find(')', target_sources);
    REQUIRE(end != std::string::npos);
    const std::string_view production_sources{cmake.data() + target_sources,
                                              end - target_sources};
    REQUIRE(production_sources.find("fullscreenpasswidget.cpp") !=
            std::string_view::npos);
    REQUIRE(production_sources.find("fullscreenpasswidget.hpp") !=
            std::string_view::npos);

    // Negative control for the stripper itself: the same scan over a copy
    // whose entries are commented out must not find them.
    std::string commented{cmake};
    for (const std::string_view entry :
         std::array{std::string_view{"fullscreenpasswidget.cpp"},
                    std::string_view{"fullscreenpasswidget.hpp"}}) {
        const auto at = commented.find(entry);
        REQUIRE(at != std::string::npos);
        commented.insert(at, "# ");
    }
    const std::string stripped_again = stripCMakeComments(commented);
    REQUIRE(stripped_again.find("fullscreenpasswidget.cpp") ==
            std::string::npos);
    REQUIRE(stripped_again.find("fullscreenpasswidget.hpp") ==
            std::string::npos);
}

TEST_CASE(
    "WP321a form has no project write or engine-apply path",
    "[devstudio][fullscreen-pass][wp321a][no-side-effects]") {
    (void)application();
    const QStringList projects_before = projectStatusIgnoringShaderCache();

    RefreshHarness refresh;
    FullscreenPassWidget form{refresh.driver()};
    const Json plan = minimalFramePlan(
        "side_effect_graph",
        Json::array({resource("input"), resource("output")}));
    form.receiveResult(QByteArray::fromStdString(plan.dump()));
    driveMinimalDraft(form, "draft", "engine://fullscreen", "input",
                      "output");
    REQUIRE_FALSE(outputJson(form).empty());
    REQUIRE(projectStatusIgnoringShaderCache() == projects_before);

    const auto view_dir = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                          "src" / "devstudio" / "view";
    const std::string source =
        readText(view_dir / "fullscreenpasswidget.hpp") +
        readText(view_dir / "fullscreenpasswidget.cpp");
    for (const std::string_view forbidden :
         std::array{std::string_view{"LayoutPresetManager"},
                    std::string_view{"QStandardPaths"},
                    std::string_view{"QSaveFile"},
                    std::string_view{"WriteOnly"}}) {
        REQUIRE(source.find(forbidden) == std::string::npos);
    }

    std::size_t request_count = 0;
    std::size_t position = 0;
    while ((position = source.find("requestRpc(", position)) !=
           std::string::npos) {
        const auto statement_end = source.find(';', position);
        REQUIRE(statement_end != std::string::npos);
        const std::string_view statement{source.data() + position,
                                         statement_end - position};
        REQUIRE(statement.find("QStringLiteral(\"get_frame_plan\")") !=
                std::string_view::npos);
        ++request_count;
        position = statement_end + 1;
    }
    REQUIRE(request_count > 0);

    const auto &scope = required<QLabel>(
        form, "pelican.fullscreenPass.scopeNotice");
    REQUIRE(scope.text().contains(QStringLiteral("not written")));
    REQUIRE(scope.text().contains(QStringLiteral("not applied")));
}

TEST_CASE(
    "WP321a Refresh invalidates a draft on graph-resource rebinding but not runtime generation",
    "[devstudio][fullscreen-pass][wp321a][refresh][stale]") {
    (void)application();
    RefreshHarness refresh;
    FullscreenPassWidget form{refresh.driver()};
    auto &button = required<QPushButton>(
        form, "pelican.fullscreenPass.refresh");

    const Json first = minimalFramePlan(
        "refresh_graph",
        Json::array({resource("first_input"), resource("first_output")}),
        10);
    button.click();
    REQUIRE(refresh.requests.size() == 1);
    form.receiveRefreshResult(
        refresh.requests.back(), QByteArray::fromStdString(first.dump()));
    driveMinimalDraft(form, "fresh_pass", "engine://fullscreen",
                      "first_input", "first_output");
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.targetNames") == "valid");

    const Json second = minimalFramePlan(
        "refresh_graph",
        Json::array({resource("second_input"), resource("second_output")}),
        11);
    button.click();
    REQUIRE(refresh.requests.size() == 2);
    form.receiveRefreshResult(
        refresh.requests.back(), QByteArray::fromStdString(second.dump()));
    REQUIRE(required<QLineEdit>(form, "pelican.fullscreenPass.name")
                .text()
                .isEmpty());
    REQUIRE(required<QListWidget>(form, "pelican.fullscreenPass.inputs")
                .count() == 0);
    REQUIRE(required<QListWidget>(form, "pelican.fullscreenPass.colors")
                .count() == 0);
    REQUIRE(comboNames(required<QComboBox>(
                form, "pelican.fullscreenPass.inputTarget")) ==
            resourceNames(second));
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.targetNames") != "valid");
    REQUIRE(required<QLabel>(form, "pelican.fullscreenPass.planStatus")
                .text()
                .contains(QStringLiteral("discarded")));

    driveMinimalDraft(form, "preserved_pass", "engine://fullscreen",
                      "second_input", "second_output");
    Json generation_only = second;
    generation_only["runtime_generation"] = 999999;
    button.click();
    REQUIRE(refresh.requests.size() == 3);
    form.receiveRefreshResult(
        refresh.requests.back(),
        QByteArray::fromStdString(generation_only.dump()));
    REQUIRE(required<QLineEdit>(form, "pelican.fullscreenPass.name").text() ==
            QStringLiteral("preserved_pass"));
    REQUIRE(required<QListWidget>(form, "pelican.fullscreenPass.inputs")
                .count() == 1);
    REQUIRE(required<QListWidget>(form, "pelican.fullscreenPass.colors")
                .count() == 1);
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.targetNames") == "valid");
}

} // namespace PelicanStudio
