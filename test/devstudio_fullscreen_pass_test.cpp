#include "fullscreenpasswidget.hpp"
#include "mainwindow.hpp"
#include "renderfeatureswidget.hpp"
#include "../src/devstudio/model/frameplanmodel.hpp"

#include "passfieldownership.hpp"
#include "passshapepolicy.hpp"
#include "renderpipeline.hpp"
#include "renderingpass/frameplanner.hpp"
#include "renderingpass/passdefinitionjsonparser.hpp"
#include "renderingpass/rendertargetmetadataresolver.hpp"
#include "renderingpass/rendertargetnameresolver.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QWidget>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_set>
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

std::filesystem::path filesystemPath(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path{path.toStdWString()};
#else
    return std::filesystem::path{path.toStdString()};
#endif
}

QString displayPath(const std::filesystem::path &path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
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
    FramePlanReadCapability::ReadyHandler ready_handler;
    FramePlanReadCapability::ResultHandler result_handler;
    FramePlanReadCapability::FailureHandler failure_handler;
    std::size_t ready_subscriptions = 0;
    std::size_t result_subscriptions = 0;
    std::size_t failure_subscriptions = 0;

    FramePlanReadCapability driver() {
        return FramePlanReadCapability{
            .ready = [this](
                         QObject *,
                         FramePlanReadCapability::ReadyHandler handler) {
                ++ready_subscriptions;
                ready_handler = std::move(handler);
                ready_handler(available, {});
            },
            .requestFramePlan = [this](QString *) {
                requests.push_back(++next_id);
                return requests.back();
            },
            .result = [this](
                          QObject *,
                          FramePlanReadCapability::ResultHandler handler) {
                ++result_subscriptions;
                result_handler = std::move(handler);
            },
            .failure = [this](
                           QObject *,
                           FramePlanReadCapability::FailureHandler handler) {
                ++failure_subscriptions;
                failure_handler = std::move(handler);
            },
        };
    }

    void succeed(qint64 request_id, const QByteArray &result_json) const {
        if (!result_handler) {
            throw std::runtime_error("result callback was not subscribed");
        }
        result_handler(request_id, result_json);
    }

    void fail(qint64 request_id, const QString &message) const {
        if (!failure_handler) {
            throw std::runtime_error("failure callback was not subscribed");
        }
        failure_handler(request_id, message);
    }
};

struct AuthoringHarness {
    struct Request {
        qint64 id = 0;
        QString method;
        QJsonObject params;
    };

    bool available = true;
    qint64 next_id = 1000;
    std::vector<Request> requests;
    RenderPassAuthoringCapability::ReadyHandler ready_handler;
    RenderPassAuthoringCapability::ResultHandler result_handler;
    RenderPassAuthoringCapability::FailureHandler failure_handler;

    RenderPassAuthoringCapability driver() {
        const auto request = [this](QString method,
                                    QJsonObject params) {
            const auto id = ++next_id;
            requests.push_back(
                Request{.id = id,
                        .method = std::move(method),
                        .params = std::move(params)});
            return id;
        };
        return RenderPassAuthoringCapability{
            .ready = [this](
                         QObject *,
                         RenderPassAuthoringCapability::ReadyHandler handler) {
                ready_handler = std::move(handler);
                ready_handler(available, {});
            },
            .requestContext = [request](QString *) mutable {
                return request(
                    QStringLiteral("get_render_authoring_context"), {});
            },
            .addAuthoredPass =
                [request](const QJsonObject &params, QString *) mutable {
                    return request(QStringLiteral("add_authored_pass"),
                                   params);
                },
            .removeAuthoredPass =
                [request](const QJsonObject &params, QString *) mutable {
                    return request(QStringLiteral("remove_authored_pass"),
                                   params);
                },
            .requestEditResult =
                [request](const QString &ticket, QString *) mutable {
                    return request(
                        QStringLiteral("get_edit_result"),
                        QJsonObject{{QStringLiteral("ticket"), ticket}});
                },
            .result = [this](
                          QObject *,
                          RenderPassAuthoringCapability::ResultHandler
                              handler) {
                result_handler = std::move(handler);
            },
            .failure = [this](
                           QObject *,
                           RenderPassAuthoringCapability::FailureHandler
                               handler) {
                failure_handler = std::move(handler);
            },
        };
    }

    void succeed(qint64 request_id, const Json &result) const {
        if (!result_handler) {
            throw std::runtime_error(
                "authoring result callback was not subscribed");
        }
        result_handler(request_id,
                       QByteArray::fromStdString(result.dump()));
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
    auto &raster_state = required<QPlainTextEdit>(
        form, "pelican.fullscreenPass.rasterState");
    raster_state.setPlainText(
        declaration.contains("raster_state")
            ? QString::fromStdString(
                  declaration.at("raster_state").dump())
            : QString{});
    for (const auto &input : declaration.at("input")) {
        chooseCandidate(form, "pelican.fullscreenPass.inputTarget",
                        "pelican.fullscreenPass.addInput",
                        input.get_ref<const std::string &>());
    }
    const auto &color = declaration.at("output").at("color");
    if (color.is_array()) {
        for (const auto &entry : color) {
            chooseCandidate(form, "pelican.fullscreenPass.colorTarget",
                            "pelican.fullscreenPass.addColor",
                            entry.get_ref<const std::string &>());
        }
    } else if (!color.is_null()) {
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

StringSet formOwnedKeysFromAuthority() {
    StringSet result;
    for (const auto field : Pelican::passAuthoringProjectionFields(
             Pelican::RenderPassType::fullscreen)) {
        result.insert(std::string{field});
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

StringSet shapeViolations(const FullscreenPassWidget &form) {
    StringSet result;
    for (const auto &name :
         requiredWidget(form, "pelican.fullscreenPass.axis.passShape")
             .property("pelicanPassShapeViolations")
             .toStringList()) {
        result.insert(name.toStdString());
    }
    return result;
}

void addInputReference(FullscreenPassWidget &form,
                       std::string_view authored) {
    const auto input =
        Pelican::parsePassShapeInputReference(authored);
    chooseCandidate(
        form, "pelican.fullscreenPass.inputTarget",
        input.history ? "pelican.fullscreenPass.addHistoryInput"
                      : "pelican.fullscreenPass.addInput",
        input.name);
}

void driveShapeDraft(FullscreenPassWidget &form,
                     const std::vector<std::string> &inputs,
                     const std::vector<std::string> &colors,
                     const std::optional<std::string> &depth) {
    required<QLineEdit>(form, "pelican.fullscreenPass.name")
        .setText(QStringLiteral("shape_case"));
    required<QLineEdit>(form, "pelican.fullscreenPass.shader.vertex")
        .setText(QStringLiteral("engine://fullscreen"));
    required<QLineEdit>(form, "pelican.fullscreenPass.shader.fragment")
        .setText(QStringLiteral("engine://fullscreen"));
    for (const auto &input : inputs) {
        addInputReference(form, input);
    }
    for (const auto &color : colors) {
        chooseCandidate(form, "pelican.fullscreenPass.colorTarget",
                        "pelican.fullscreenPass.addColor", color);
    }
    setDepth(form, depth ? Json(*depth) : Json(nullptr));
    QApplication::processEvents();
}

// Drives evaluator and defensive-validation states that role-filtered
// choosers intentionally cannot author.
void driveUncheckedShapeDraft(
    FullscreenPassWidget &form,
    const std::vector<std::string> &inputs,
    const std::vector<std::string> &colors,
    const std::optional<std::string> &depth) {
    required<QLineEdit>(form, "pelican.fullscreenPass.shader.vertex")
        .setText(QStringLiteral("engine://fullscreen"));
    required<QLineEdit>(form, "pelican.fullscreenPass.shader.fragment")
        .setText(QStringLiteral("engine://fullscreen"));
    auto &input_list =
        required<QListWidget>(form, "pelican.fullscreenPass.inputs");
    auto &color_list =
        required<QListWidget>(form, "pelican.fullscreenPass.colors");
    input_list.clear();
    color_list.clear();
    for (const auto &input : inputs) {
        input_list.addItem(QString::fromStdString(input));
    }
    for (const auto &color : colors) {
        color_list.addItem(QString::fromStdString(color));
    }
    auto &depth_combo =
        required<QComboBox>(form, "pelican.fullscreenPass.depthTarget");
    if (!depth) {
        depth_combo.setCurrentIndex(-1);
    } else {
        const QString target = QString::fromStdString(*depth);
        int index = depth_combo.findText(target, Qt::MatchExactly);
        if (index < 0) {
            depth_combo.addItem(target);
            index = depth_combo.count() - 1;
        }
        depth_combo.setCurrentIndex(index);
    }
    // QListWidget insertion is intentionally not a production edit signal;
    // changing a real form control triggers one complete recomputation.
    required<QLineEdit>(form, "pelican.fullscreenPass.name")
        .setText(QStringLiteral("unchecked_shape_case"));
    QApplication::processEvents();
}

struct CoreShapeResources {
    std::vector<std::string> render_targets;
    std::unordered_set<std::string> buffers;
    std::unordered_set<std::string> history_targets;
};

const CoreShapeResources &defaultCoreShapeResources() {
    static const CoreShapeResources resources{
        .render_targets = {"input_a", "output_a", "output_b", "depth_a",
                           "taa_accum"},
        .buffers = {},
        .history_targets = {"input_a", "output_a", "output_b", "depth_a",
                            "taa_accum"},
    };
    return resources;
}

Pelican::PassDefinition parseCoreShape(
    const Json &pass, const Pelican::PassShapePolicy &policy,
    const CoreShapeResources &resources = defaultCoreShapeResources()) {
    const Pelican::RenderTargetNameResolver names{
        [&resources](const std::string &name) {
            const auto found =
                std::ranges::find(resources.render_targets, name);
            return found == resources.render_targets.end()
                       ? Pelican::noRenderTargetId()
                       : Pelican::GlobalRenderTargetId{
                             static_cast<int>(
                                 found - resources.render_targets.begin())};
        }};
    const Pelican::RenderTargetMetadataResolver metadata{
        [&resources](Pelican::GlobalRenderTargetId id) {
            if (id.value < 0 ||
                static_cast<std::size_t>(id.value) >=
                    resources.render_targets.size()) {
                throw std::runtime_error(
                    "unexpected shape target metadata lookup");
            }
            const auto &name = resources.render_targets[
                static_cast<std::size_t>(id.value)];
            return Pelican::RenderTargetMetadata{
                .name = name,
                .usage = vk::ImageUsageFlagBits::eSampled |
                         vk::ImageUsageFlagBits::eColorAttachment |
                         vk::ImageUsageFlagBits::eDepthStencilAttachment,
                .format = vk::Format::eR8G8B8A8Unorm,
                .extent = vk::Extent2D{16, 16},
                .history = resources.history_targets.contains(name),
            };
        }};
    return Pelican::parsePassDefinitionFromJson(
        pass, names, metadata, policy, resources.buffers);
}

bool coreAcceptsShape(
    const Json &pass, const Pelican::PassShapePolicy &policy,
    const CoreShapeResources &resources = defaultCoreShapeResources()) {
    try {
        (void)parseCoreShape(pass, policy, resources);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

Json resolvedFramePlanFromAuthoredConfig(const Json &authored,
                                         std::string_view source_name) {
    const auto resolved = Pelican::resolveRenderPipeline(
        Pelican::RenderPipelineRequest{
            .authored_config = authored,
            .source_name = std::string{source_name},
        },
        Pelican::RenderEnvironmentCapabilities{});
    const auto compiled = Pelican::compileRenderPipeline(resolved);
    const auto graphs = Pelican::parseFrameGraphDefinitionsFromConfigJson(
        resolved.normalized_config);
    if (graphs.size() != 1) {
        throw std::runtime_error(
            "real-file test expected exactly one resolved frame graph");
    }
    return Pelican::framePlanToJson(
        Pelican::planFrameGraph(graphs.front()), &compiled);
}

Json shapePlan() {
    return minimalFramePlan(
        "shape_graph",
        Json::array({resource("input_a"), resource("output_a"),
                     resource("output_b"), resource("depth_a"),
                     resource("taa_accum"),
                     resource("swapchain", "frame_target", "engine")}));
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

void clearDraftThroughControls(FullscreenPassWidget &form) {
    required<QLineEdit>(form, "pelican.fullscreenPass.name").clear();
    required<QLineEdit>(form,
                        "pelican.fullscreenPass.shader.vertex")
        .clear();
    required<QLineEdit>(form,
                        "pelican.fullscreenPass.shader.fragment")
        .clear();
    required<QPlainTextEdit>(
        form, "pelican.fullscreenPass.rasterState")
        .clear();
    for (const auto &[list_name, remove_name] :
         std::array{
             std::pair{"pelican.fullscreenPass.inputs",
                       "pelican.fullscreenPass.removeInput"},
             std::pair{"pelican.fullscreenPass.colors",
                       "pelican.fullscreenPass.removeColor"}}) {
        auto &list = required<QListWidget>(form, list_name);
        auto &remove = required<QPushButton>(form, remove_name);
        while (list.count() > 0) {
            list.setCurrentRow(0);
            remove.click();
        }
    }
    required<QPushButton>(form,
                          "pelican.fullscreenPass.clearDepth")
        .click();
    QApplication::processEvents();
}

struct TreeSnapshot {
    StringSet directories;
    std::map<std::string, std::string, std::less<>> files;

    bool operator==(const TreeSnapshot &) const = default;
};

TreeSnapshot snapshotTree(const std::filesystem::path &root) {
    TreeSnapshot result;
    if (!std::filesystem::exists(root)) {
        return result;
    }
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator{root}) {
        const std::string relative =
            entry.path().lexically_relative(root).generic_string();
        if (entry.is_directory()) {
            result.directories.insert(relative);
        } else if (entry.is_regular_file()) {
            result.files.emplace(relative, readText(entry.path()));
        }
    }
    return result;
}

class ScopedEnvironment final {
    struct SavedValue {
        QByteArray name;
        QByteArray value;
        bool existed = false;
    };
    std::vector<SavedValue> saved_;

  public:
    void set(const char *name, const QString &value) {
        const QByteArray key{name};
        saved_.push_back(SavedValue{
            .name = key,
            .value = qgetenv(name),
            .existed = qEnvironmentVariableIsSet(name),
        });
        qputenv(name, value.toUtf8());
    }

    ~ScopedEnvironment() {
        for (auto current = saved_.rbegin(); current != saved_.rend();
             ++current) {
            if (current->existed) {
                qputenv(current->name.constData(), current->value);
            } else {
                qunsetenv(current->name.constData());
            }
        }
    }
};

class ScopedCurrentPath final {
    std::filesystem::path previous_ = std::filesystem::current_path();

  public:
    explicit ScopedCurrentPath(const std::filesystem::path &path) {
        std::filesystem::current_path(path);
    }
    ~ScopedCurrentPath() { std::filesystem::current_path(previous_); }
};

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
        form.receiveAuthoringConfig(
            QByteArray::fromStdString(config.dump()));
        form.receiveResult(frame_plan);
        driveFromDeclaration(form, declaration);

        const Json output = outputJson(form);
        const StringSet owned_keys = formOwnedKeysFromAuthority();
        REQUIRE(output == projectToKeys(declaration, owned_keys));
        const StringSet expected_difference =
            actualDifference(declaration, output);
        REQUIRE_FALSE(expected_difference.empty());
        REQUIRE(reportedOmissions(form) == expected_difference);
        REQUIRE(axisState(
                    form,
                    "pelican.fullscreenPass.axis.nameCollision") ==
                "invalid");

        // Exercise the ineffective side with the same widget instance: remove
        // its authored context, clear it through user controls, and drive the
        // same declaration again. The JSON values still resolve identically,
        // but an unaware form cannot resolve the omitted-key difference.
        form.receiveAuthoringConfig(QByteArrayLiteral("{}"));
        clearDraftThroughControls(form);
        driveFromDeclaration(form, declaration);
        REQUIRE(outputJson(form) == output);
        REQUIRE(reportedOmissions(form).empty());
        REQUIRE(required<QLabel>(
                    form, "pelican.fullscreenPass.authoringStatus")
                    .text()
                    .contains(QStringLiteral("unavailable")));

        // Removing authoring context also resolves the opposite collision
        // state without suppressing the central JSON projection observation.
        REQUIRE(axisState(
                    form,
                    "pelican.fullscreenPass.axis.nameCollision") ==
                "not_checked");
        REQUIRE_FALSE(output.empty());
    }
}

TEST_CASE(
    "WP327 target choosers derive distinct role sets from frame-plan resource kinds",
    "[devstudio][fullscreen-pass][wp327][resources][resource-role][negative-contrast]") {
    (void)application();
    const Json plan = minimalFramePlan(
        "role_graph",
        Json::array({
            resource("storage_buffer", "buffer"),
            resource("image_target", "render_target"),
            resource("swapchain", "frame_target", "engine"),
            resource("alternate_frame", "frame_target", "engine"),
        }));

    RefreshHarness refresh;
    FullscreenPassWidget form{refresh.driver()};
    form.receiveResult(QByteArray::fromStdString(plan.dump()));

    const StringSet inputs{"image_target", "storage_buffer"};
    const StringSet colors{"image_target", "swapchain"};
    const StringSet depths{"image_target"};
    REQUIRE(comboNames(required<QComboBox>(
                form, "pelican.fullscreenPass.inputTarget")) == inputs);
    REQUIRE(comboNames(required<QComboBox>(
                form, "pelican.fullscreenPass.colorTarget")) == colors);
    REQUIRE(comboNames(required<QComboBox>(
                form, "pelican.fullscreenPass.depthTarget")) == depths);
    REQUIRE(inputs != colors);
    REQUIRE(inputs != depths);
    REQUIRE(colors != depths);
    REQUIRE_FALSE(inputs.contains("alternate_frame"));
    REQUIRE_FALSE(colors.contains("alternate_frame"));
    REQUIRE_FALSE(depths.contains("alternate_frame"));

    auto &input = required<QComboBox>(
        form, "pelican.fullscreenPass.inputTarget");
    auto &add_history = required<QPushButton>(
        form, "pelican.fullscreenPass.addHistoryInput");
    input.setCurrentIndex(input.findText(QStringLiteral("storage_buffer")));
    REQUIRE_FALSE(add_history.isEnabled());
    input.setCurrentIndex(input.findText(QStringLiteral("image_target")));
    REQUIRE(add_history.isEnabled());
}

TEST_CASE(
    "WP327 resource-kind misdisplays are non-valid and each valid neighbor remains accepted",
    "[devstudio][fullscreen-pass][wp327][resource-role][history][negative-contrast]") {
    (void)application();
    const Json plan = minimalFramePlan(
        "role_validation_graph",
        Json::array({
            resource("storage_buffer", "buffer"),
            resource("image_target"),
            resource("no_history_target"),
            resource("output_target"),
            resource("swapchain", "frame_target", "engine"),
            resource("alternate_frame", "frame_target", "engine"),
        }));
    const CoreShapeResources core_resources{
        .render_targets = {"image_target", "no_history_target",
                           "output_target"},
        .buffers = {"storage_buffer"},
        .history_targets = {"image_target", "output_target"},
    };
    const auto &policy = Pelican::defaultPassShapePolicy();

    const auto authored = [](std::vector<std::string> inputs,
                             std::vector<std::string> colors) {
        return Json{
            {"name", "resource_role_case"},
            {"type", "fullscreen"},
            {"input", std::move(inputs)},
            {"output", {{"color", std::move(colors)}, {"depth", nullptr}}},
            {"shader",
             {{"vertex", "engine://fullscreen"},
              {"fragment", "engine://fullscreen"}}},
        };
    };
    struct Assessment {
        std::string target_names;
        std::string history_support;
    };
    const auto assess = [&](const Json &pass) {
        RefreshHarness refresh;
        FullscreenPassWidget form{refresh.driver(), policy};
        form.receiveResult(QByteArray::fromStdString(plan.dump()));
        driveUncheckedShapeDraft(
            form, pass.at("input").get<std::vector<std::string>>(),
            pass.at("output").at("color")
                .get<std::vector<std::string>>(),
            std::nullopt);
        return Assessment{
            .target_names = axisState(
                form, "pelican.fullscreenPass.axis.targetNames"),
            .history_support = axisState(
                form, "pelican.fullscreenPass.axis.historySupport"),
        };
    };

    const Json buffer_color = authored({}, {"storage_buffer"});
    const Json render_target_color = authored({}, {"output_target"});
    REQUIRE_FALSE(coreAcceptsShape(buffer_color, policy, core_resources));
    REQUIRE(coreAcceptsShape(render_target_color, policy, core_resources));
    REQUIRE(assess(buffer_color).target_names == "invalid");
    REQUIRE(assess(render_target_color).target_names == "valid");

    const Json alternate_frame_input =
        authored({"alternate_frame"}, {"output_target"});
    const Json render_target_input =
        authored({"image_target"}, {"output_target"});
    REQUIRE_FALSE(
        coreAcceptsShape(alternate_frame_input, policy, core_resources));
    REQUIRE(coreAcceptsShape(render_target_input, policy, core_resources));
    REQUIRE(assess(alternate_frame_input).target_names == "invalid");
    REQUIRE(assess(render_target_input).target_names == "valid");

    const Json alternate_frame_color = authored({}, {"alternate_frame"});
    const Json swapchain_color = authored({}, {"swapchain"});
    REQUIRE_FALSE(
        coreAcceptsShape(alternate_frame_color, policy, core_resources));
    REQUIRE(coreAcceptsShape(swapchain_color, policy, core_resources));
    REQUIRE(assess(alternate_frame_color).target_names == "invalid");
    REQUIRE(assess(swapchain_color).target_names == "valid");

    const Json buffer_history =
        authored({"storage_buffer@history"}, {"output_target"});
    const Json buffer_current =
        authored({"storage_buffer"}, {"output_target"});
    REQUIRE_FALSE(coreAcceptsShape(buffer_history, policy, core_resources));
    REQUIRE(coreAcceptsShape(buffer_current, policy, core_resources));
    REQUIRE(assess(buffer_history).history_support == "invalid");
    REQUIRE(assess(buffer_current).history_support == "valid");

    const Json unavailable_history =
        authored({"no_history_target@history"}, {"output_target"});
    const Json current_target =
        authored({"no_history_target"}, {"output_target"});
    REQUIRE_FALSE(
        coreAcceptsShape(unavailable_history, policy, core_resources));
    REQUIRE(coreAcceptsShape(current_target, policy, core_resources));
    REQUIRE(assess(unavailable_history).history_support == "not_checked");
    REQUIRE(assess(unavailable_history).history_support != "valid");
    REQUIRE(assess(current_target).history_support == "valid");
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

    auto [refresh, contrast_form] = make_form();
    driveMinimalDraft(*contrast_form, "partial_case",
                      "engine://fullscreen", "safe_input", "safe_output");
    auto &summary = required<QLabel>(
        *contrast_form, "pelican.fullscreenPass.validationSummary");
    auto &copy = required<QPushButton>(
        *contrast_form, "pelican.fullscreenPass.copyJson");
    REQUIRE(summary.property("pelicanValidationSummaryState").toString() ==
            QStringLiteral("partial"));
    REQUIRE(summary.text().contains(QStringLiteral("Not checked")));
    REQUIRE(copy.isEnabled());
    const QString partial_summary = summary.text();

    const std::array unchecked_axes{
        "pelican.fullscreenPass.axis.targetUsage",
        "pelican.fullscreenPass.axis.generationOrder",
        "pelican.fullscreenPass.axis.shaderResolution",
    };
    for (const char *axis : unchecked_axes) {
        REQUIRE(axisState(*contrast_form, axis) == "not_checked");
        REQUIRE(axisText(*contrast_form, axis)
                    .contains(QStringLiteral("Not checked")));
    }

    // The negative side is another real state of the same widget, not a
    // rewritten copy of its strings.
    clearDraftThroughControls(*contrast_form);
    driveMinimalDraft(*contrast_form, "feedback_case",
                      "engine://fullscreen", "safe_output", "safe_output");
    REQUIRE(axisState(
                *contrast_form,
                "pelican.fullscreenPass.axis.targetNames") == "valid");
    REQUIRE(axisState(
                *contrast_form,
                "pelican.fullscreenPass.axis.passShape") == "invalid");
    REQUIRE(shapeViolations(*contrast_form).contains(
        "current_frame_color_feedback"));
    REQUIRE(summary.property("pelicanValidationSummaryState").toString() ==
            QStringLiteral("invalid"));
    REQUIRE(summary.text() != partial_summary);
    REQUIRE_FALSE(copy.isEnabled());
}

TEST_CASE(
    "WP334b production MainWindow exposes engine-owned authored-pass save",
    "[devstudio][fullscreen-pass][wp334b][production-wiring]") {
    (void)application();
    MainWindow window;
    auto *child = window.findChild<QWidget *>(
        QStringLiteral("pelican.fullscreenPass"));
    REQUIRE(child != nullptr);
    REQUIRE(dynamic_cast<FullscreenPassWidget *>(child) != nullptr);
    auto *save = child->findChild<QPushButton *>(
        QStringLiteral("pelican.fullscreenPass.save"));
    auto *remove = child->findChild<QPushButton *>(
        QStringLiteral("pelican.fullscreenPass.remove"));
    REQUIRE(save != nullptr);
    REQUIRE(remove != nullptr);
    REQUIRE(save->toolTip().contains(
        QStringLiteral("add_authored_pass")));
    REQUIRE(remove->toolTip().contains(
        QStringLiteral("remove_authored_pass")));

    const auto source_root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const auto form_source = readText(
        source_root / "src" / "devstudio" / "view" /
        "fullscreenpasswidget.cpp");
    const auto window_source = readText(
        source_root / "src" / "devstudio" / "view" /
        "mainwindow.cpp");
    REQUIRE(form_source.find("openProjectReadOnly") ==
            std::string::npos);
    REQUIRE(form_source.find("ProjectPathResolver") ==
            std::string::npos);
    REQUIRE(window_source.find(
                "renderPassAuthoringCapability(*viewport_)") !=
            std::string::npos);
}

TEST_CASE(
    "WP334b fullscreen form saves and removes through the restricted engine capability",
    "[devstudio][fullscreen-pass][wp334b][save][remove][rpc]") {
    (void)application();
    RefreshHarness refresh;
    AuthoringHarness authoring;
    FullscreenPassWidget form{refresh.driver(), authoring.driver()};
    REQUIRE(authoring.requests.size() == 1);
    REQUIRE(authoring.requests.back().method ==
            QStringLiteral("get_render_authoring_context"));

    const std::string first_digest(64, 'a');
    const Json first_context{
        {"source_reference", "passes/main.json"},
        {"source_digest",
         {{"algorithm", "sha256"}, {"hex", first_digest}}},
        {"published_generation", 12},
        {"config_kind", "preset"},
        {"pipeline_preset",
         {{"reference", "engine://pipelines/hybrid_v1.json"},
          {"name", "hybrid_v1"},
          {"version", 1}}},
        {"graphs",
         Json::array(
             {{{"name", "save_graph"},
               {"passes",
                Json::array(
                    {{{"name", "base"},
                      {"declaration",
                       {{"name", "base"},
                        {"type", "fullscreen"},
                        {"input", Json::array({"input"})},
                        {"output",
                         {{"color", Json::array({"output"})},
                          {"depth", nullptr}}},
                        {"shader",
                         {{"vertex", "engine://fullscreen"},
                          {"fragment", "engine://fullscreen"}}}}},
                      {"provenance", {{"source", "engine"}}}}})},
               {"anchor_candidates",
                Json::array(
                    {{{"position", 0}, {"insert", "before:base"}},
                     {{"position", 1}, {"insert", "after:base"}}})}}})},
        {"managed_fragments", Json::array()},
    };
    authoring.succeed(authoring.requests.back().id, first_context);
    REQUIRE(required<QLabel>(
                form, "pelican.fullscreenPass.authoringStatus")
                .text()
                .contains(QStringLiteral("preset")));

    const Json plan = minimalFramePlan(
        "save_graph",
        Json::array({resource("input"), resource("output")}));
    form.receiveResult(QByteArray::fromStdString(plan.dump()));
    driveMinimalDraft(form, "authored_probe", "engine://fullscreen",
                      "input", "output");
    auto &save = required<QPushButton>(
        form, "pelican.fullscreenPass.save");
    REQUIRE(save.isEnabled());
    save.click();
    REQUIRE(authoring.requests.back().method ==
            QStringLiteral("add_authored_pass"));
    REQUIRE(authoring.requests.back()
                .params.value(QStringLiteral("base_source_digest"))
                .toString() == QString::fromStdString(first_digest));
    REQUIRE(authoring.requests.back()
                .params.value(QStringLiteral("graph"))
                .toString() == QStringLiteral("save_graph"));
    REQUIRE(authoring.requests.back()
                .params.value(QStringLiteral("insert"))
                .toString() == QStringLiteral("after:base"));
    REQUIRE(authoring.requests.back()
                .params.value(QStringLiteral("pass"))
                .toObject()
                .value(QStringLiteral("name"))
                .toString() == QStringLiteral("authored_probe"));

    const auto add_id = authoring.requests.back().id;
    authoring.succeed(
        add_id,
        {{"ticket", "render-authored-pass-1"},
         {"status", "accepted"}});
    QApplication::processEvents();
    REQUIRE(authoring.requests.back().method ==
            QStringLiteral("get_edit_result"));
    const auto result_id = authoring.requests.back().id;
    authoring.succeed(
        result_id,
        {{"ticket", "render-authored-pass-1"},
         {"status", "committed"},
         {"committed", true},
         {"published_generation", 13}});
    REQUIRE(authoring.requests.back().method ==
            QStringLiteral("get_render_authoring_context"));
    REQUIRE(refresh.requests.size() == 1);

    const std::string second_digest(64, 'b');
    Json second_context = first_context;
    second_context["source_digest"]["hex"] = second_digest;
    second_context["published_generation"] = 13;
    second_context["managed_fragments"] = Json::array(
        {{{"reference",
           "project://passes/authoring/pass-probe.json"},
          {"digest",
           {{"algorithm", "sha256"},
            {"hex", std::string(64, 'c')}}},
          {"pass_names", Json::array({"authored_probe"})}}});
    authoring.succeed(authoring.requests.back().id, second_context);
    auto &fragment = required<QComboBox>(
        form, "pelican.fullscreenPass.removeFragment");
    REQUIRE(fragment.count() == 1);
    fragment.setCurrentIndex(0);
    auto &remove = required<QPushButton>(
        form, "pelican.fullscreenPass.remove");
    REQUIRE(remove.isEnabled());
    remove.click();
    REQUIRE(authoring.requests.back().method ==
            QStringLiteral("remove_authored_pass"));
    REQUIRE(authoring.requests.back()
                .params.value(QStringLiteral("base_source_digest"))
                .toString() == QString::fromStdString(second_digest));
    REQUIRE(authoring.requests.back()
                .params.value(QStringLiteral("fragment_reference"))
                .toString() ==
            QStringLiteral(
                "project://passes/authoring/pass-probe.json"));
}

TEST_CASE(
    "WP355 fullscreen form saves additive raster state distinctly from omission",
    "[devstudio][fullscreen-pass][wp355][raster-state][save][resolve]") {
    (void)application();
    const Json plan = minimalFramePlan(
        "blend_graph",
        Json::array({resource("swapchain", "frame_target", "engine")}));
    const Json context{
        {"source_digest", {{"hex", std::string(64, 'c')}}},
        {"config_kind", "direct"},
        {"graphs",
         Json::array({
             {{"name", "blend_graph"},
              {"passes", Json::array()},
              {"anchor_candidates",
               Json::array({{{"position", 0},
                              {"insert", "append"}}})}},
         })},
        {"managed_fragments", Json::array()},
    };
    const Json additive_state{
        {"cull", "none"},
        {"color_attachments",
         Json::array({{{"blend", "additive"},
                       {"write_mask", "rg"}}})},
    };

    RefreshHarness refresh;
    AuthoringHarness authoring;
    FullscreenPassWidget form{refresh.driver(), authoring.driver()};
    REQUIRE(authoring.requests.size() == 1);
    authoring.succeed(authoring.requests.back().id, context);
    form.receiveResult(QByteArray::fromStdString(plan.dump()));
    driveShapeDraft(form, {}, {"swapchain"}, std::nullopt);
    auto &raster_state = required<QPlainTextEdit>(
        form, "pelican.fullscreenPass.rasterState");
    auto &save = required<QPushButton>(
        form, "pelican.fullscreenPass.save");
    const auto saved_pass = [&]() {
        REQUIRE(save.isEnabled());
        save.click();
        REQUIRE(authoring.requests.back().method ==
                QStringLiteral("add_authored_pass"));
        const QJsonDocument document{
            authoring.requests.back()
                .params.value(QStringLiteral("pass"))
                .toObject()};
        return Json::parse(
            document.toJson(QJsonDocument::Compact).toStdString());
    };
    const auto finish_save = [&](std::string ticket) {
        authoring.succeed(
            authoring.requests.back().id,
            {{"ticket", ticket}, {"status", "accepted"}});
        QApplication::processEvents();
        REQUIRE(authoring.requests.back().method ==
                QStringLiteral("get_edit_result"));
        authoring.succeed(
            authoring.requests.back().id,
            {{"ticket", ticket},
             {"status", "committed"},
             {"committed", true},
             {"published_generation", 2}});
        REQUIRE(authoring.requests.back().method ==
                QStringLiteral("get_render_authoring_context"));
        authoring.succeed(authoring.requests.back().id, context);
    };

    raster_state.setPlainText(
        QString::fromStdString(additive_state.dump()));
    const Json additive_pass = saved_pass();
    finish_save("wp355-additive");
    raster_state.clear();
    const Json omitted_pass = saved_pass();
    REQUIRE(additive_pass.at("raster_state") == additive_state);
    REQUIRE_FALSE(omitted_pass.contains("raster_state"));

    const auto resolve_saved = [](const Json &pass) {
        const Json config{
            {"render_targets", Json::array()},
            {"rendering_passes",
             Json::array({
                 {{"name", "blend_graph"},
                  {"passes", Json::array({pass})}},
             })},
        };
        return Pelican::resolveRenderPipeline(
                   Pelican::RenderPipelineRequest{
                       config, "WP355 saved fullscreen form"},
                   Pelican::RenderEnvironmentCapabilities{})
            .normalized_config;
    };
    const Json additive_resolved = resolve_saved(additive_pass);
    const Json omitted_resolved = resolve_saved(omitted_pass);
    REQUIRE(additive_resolved != omitted_resolved);
    REQUIRE(findPass(additive_resolved, "blend_graph", "shape_case")
                .at("raster_state") == additive_state);
    REQUIRE_FALSE(
        findPass(omitted_resolved, "blend_graph", "shape_case")
            .contains("raster_state"));
}

TEST_CASE(
    "WP331 production MainWindow contains the RPC render feature editor",
    "[devstudio][render-features][wp331][production-wiring]") {
    (void)application();
    MainWindow window;
    auto *child = window.findChild<QWidget *>(
        QStringLiteral("pelican.renderFeatures"));
    REQUIRE(child != nullptr);
    REQUIRE(dynamic_cast<RenderFeaturesWidget *>(child) != nullptr);
    REQUIRE(child->findChild<QListWidget *>(
                QStringLiteral("pelican.renderFeatures.current")) !=
            nullptr);
    REQUIRE(child->findChild<QTreeWidget *>(
                QStringLiteral("pelican.renderFeatures.catalog")) !=
            nullptr);
    REQUIRE(child->findChild<QPushButton *>(
                QStringLiteral("pelican.renderFeatures.add")) !=
            nullptr);
    REQUIRE(child->findChild<QPushButton *>(
                QStringLiteral("pelican.renderFeatures.remove")) !=
            nullptr);
    REQUIRE(child->findChild<QPushButton *>(
                QStringLiteral("pelican.renderFeatures.apply")) !=
            nullptr);

    const auto studio_source = readText(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "src" / "devstudio" / "view" /
        "renderfeatureswidget.cpp");
    REQUIRE(studio_source.find("engine://features/") ==
            std::string::npos);
    REQUIRE(studio_source.find("sky_ambient") ==
            std::string::npos);
    REQUIRE(studio_source.find("gpu_timing") ==
            std::string::npos);
}

TEST_CASE(
    "WP332 render feature catalog displays the engine availability reason verbatim",
    "[devstudio][render-features][wp332][availability]") {
    (void)application();
    MainWindow window;
    auto *form = dynamic_cast<RenderFeaturesWidget *>(
        window.findChild<QWidget *>(
            QStringLiteral("pelican.renderFeatures")));
    REQUIRE(form != nullptr);

    const QString engine_reason =
        QStringLiteral(
            "engine-policy: capability pelican.test.unavailable@1");
    const Json response{
        {"features",
         Json::array({
             {{"name", "blocked_probe"},
              {"reference", "engine://features/blocked_probe.json"},
              {"requires_runtime_module", false},
              {"hot_add_supported", false},
              {"available", false},
              {"unavailable_reason",
               engine_reason.toStdString()}},
             {{"name", "available_probe"},
              {"reference", "engine://features/available_probe.json"},
              {"requires_runtime_module", false},
              {"hot_add_supported", true},
              {"available", true}},
         })},
    };
    form->receiveCatalogResult(
        QByteArray::fromStdString(response.dump()));

    auto *catalog = form->findChild<QTreeWidget *>(
        QStringLiteral("pelican.renderFeatures.catalog"));
    REQUIRE(catalog != nullptr);
    REQUIRE(catalog->topLevelItemCount() == 2);
    const auto find_item = [&](const QString &name) {
        for (int index = 0;
             index < catalog->topLevelItemCount(); ++index) {
            auto *item = catalog->topLevelItem(index);
            if (item->text(0) == name) return item;
        }
        return static_cast<QTreeWidgetItem *>(nullptr);
    };
    const auto *blocked =
        find_item(QStringLiteral("blocked_probe"));
    const auto *available =
        find_item(QStringLiteral("available_probe"));
    REQUIRE(blocked != nullptr);
    REQUIRE(available != nullptr);
    REQUIRE(blocked->text(1) == engine_reason);
    REQUIRE(blocked->toolTip(0) == engine_reason);
    REQUIRE(blocked->toolTip(1) == engine_reason);
    REQUIRE(available->text(1) == QStringLiteral("Available"));

    const auto studio_source = readText(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "src" / "devstudio" / "view" /
        "renderfeatureswidget.cpp");
    REQUIRE(studio_source.find(engine_reason.toStdString()) ==
            std::string::npos);
    REQUIRE(studio_source.find(
                "requires dynamic runtime module creation") ==
            std::string::npos);
}

TEST_CASE(
    "WP334b frame-plan-only test capability cannot reach authored-pass save",
    "[devstudio][fullscreen-pass][wp334b][capability][no-side-effects]") {
    (void)application();

    RefreshHarness refresh;
    FullscreenPassWidget form{refresh.driver()};
    REQUIRE(refresh.ready_subscriptions == 1);
    REQUIRE(refresh.result_subscriptions == 1);
    REQUIRE(refresh.failure_subscriptions == 1);

    const Json plan = minimalFramePlan(
        "side_effect_graph",
        Json::array({resource("input"), resource("output")}));
    auto &refresh_button = required<QPushButton>(
        form, "pelican.fullscreenPass.refresh");
    refresh_button.click();
    REQUIRE(refresh.requests.size() == 1);
    refresh.succeed(refresh.requests.back(),
                    QByteArray::fromStdString(plan.dump()));
    REQUIRE(comboNames(required<QComboBox>(
                form, "pelican.fullscreenPass.inputTarget")) ==
            resourceNames(plan));

    driveMinimalDraft(form, "draft", "engine://fullscreen", "input",
                      "output");
    REQUIRE_FALSE(outputJson(form).empty());

    const auto &scope = required<QLabel>(
        form, "pelican.fullscreenPass.scopeNotice");
    REQUIRE(scope.text().contains(QStringLiteral("engine"),
                                  Qt::CaseInsensitive));
    REQUIRE_FALSE(required<QPushButton>(
                      form, "pelican.fullscreenPass.save")
                      .isEnabled());
    REQUIRE_FALSE(required<QPushButton>(
                      form, "pelican.fullscreenPass.remove")
                      .isEnabled());

    refresh_button.click();
    REQUIRE(refresh.requests.size() == 2);
    refresh.fail(refresh.requests.back(),
                 QStringLiteral("spy failure"));
    REQUIRE(required<QLabel>(
                form, "pelican.fullscreenPass.planStatus")
                .text()
                .contains(QStringLiteral("spy failure")));
}

TEST_CASE(
    "WP334b production project-open waits for engine context and performs no local render-config read",
    "[devstudio][fullscreen-pass][wp334b][project-open][engine-owned]") {
    (void)application();
    const auto source_root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const auto project_root =
        source_root / "projects" / "example";
    const Json config = readJson(
        project_root / "passes" / "main_rendering_config.json");
    const QByteArray frame_plan = QByteArray::fromStdString(readText(
        source_root / "test" / "fixtures" / "devstudio" /
        "example_frame_plan.json"));
    const std::string graph =
        config.at("rendering_passes").at(0).at("name").get<std::string>();
    const Json &declaration = findPass(config, graph, "ssao_pass");

    QTemporaryDir isolation;
    REQUIRE(isolation.isValid());
    const std::filesystem::path isolation_root =
        filesystemPath(isolation.path());
    const auto working_root = isolation_root / "working";
    const auto appdata_root = isolation_root / "appdata";
    const auto localdata_root = isolation_root / "localdata";
    const auto temp_root = isolation_root / "temp";
    std::filesystem::create_directories(working_root);
    std::filesystem::create_directories(appdata_root);
    std::filesystem::create_directories(localdata_root);
    std::filesystem::create_directories(temp_root);

    ScopedEnvironment environment;
    environment.set("APPDATA", displayPath(appdata_root));
    environment.set("LOCALAPPDATA",
                    displayPath(localdata_root));
    environment.set("TEMP", displayPath(temp_root));
    environment.set("TMP", displayPath(temp_root));
    environment.set("XDG_CONFIG_HOME",
                    displayPath(appdata_root));
    environment.set("XDG_DATA_HOME",
                    displayPath(localdata_root));
    environment.set("XDG_CACHE_HOME",
                    displayPath(localdata_root));
    environment.set(
        "PELICAN_STUDIO_PLAYER",
        displayPath(isolation_root / "missing-player.exe"));
    ScopedCurrentPath current_path{working_root};

    const TreeSnapshot project_before = snapshotTree(project_root);
    const TreeSnapshot writable_before = snapshotTree(isolation_root);
    {
        MainWindow window;
        window.openProject(displayPath(project_root));
        QApplication::processEvents();

        auto *form_widget = window.findChild<QWidget *>(
            QStringLiteral("pelican.fullscreenPass"));
        auto *form = dynamic_cast<FullscreenPassWidget *>(form_widget);
        REQUIRE(form != nullptr);
        const auto &authoring_status = required<QLabel>(
            *form, "pelican.fullscreenPass.authoringStatus");
        REQUIRE(authoring_status.text().contains(
            QStringLiteral("unavailable")));
        REQUIRE_FALSE(required<QPushButton>(
                          *form, "pelican.fullscreenPass.save")
                          .isEnabled());

        // The deterministic projection seam supplies bytes explicitly. The
        // production project-open path above did not discover these local
        // declarations by reading project.json or the rendering config.
        form->receiveAuthoringConfig(
            QByteArray::fromStdString(config.dump()));
        form->receiveResult(frame_plan);
        driveFromDeclaration(*form, declaration);
        const Json projection = outputJson(*form);
        REQUIRE(projection == projectToKeys(
                                  declaration,
                                  formOwnedKeysFromAuthority()));
        REQUIRE(reportedOmissions(*form) ==
                actualDifference(declaration, projection));
    }
    QApplication::processEvents();

    REQUIRE((snapshotTree(project_root) == project_before));
    REQUIRE((snapshotTree(isolation_root) == writable_before));
}

TEST_CASE(
    "WP325 Refresh binds drafts to graph and resources but not runtime generation",
    "[devstudio][fullscreen-pass][wp325][refresh][stale]") {
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
    refresh.succeed(
        refresh.requests.back(), QByteArray::fromStdString(first.dump()));
    driveMinimalDraft(form, "fresh_pass", "engine://fullscreen",
                      "first_input", "first_output");
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.targetNames") == "valid");
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.historySupport") ==
            "valid");

    const Json second = minimalFramePlan(
        "refresh_graph",
        Json::array({resource("second_input"), resource("second_output")}),
        11);
    button.click();
    REQUIRE(refresh.requests.size() == 2);
    refresh.succeed(
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
    refresh.succeed(
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

    Json graph_only = generation_only;
    graph_only["graph"] = "other_refresh_graph";
    REQUIRE(resourceNames(graph_only) == resourceNames(generation_only));
    button.click();
    REQUIRE(refresh.requests.size() == 4);
    refresh.succeed(
        refresh.requests.back(),
        QByteArray::fromStdString(graph_only.dump()));
    REQUIRE(required<QLabel>(form, "pelican.fullscreenPass.graph").text() ==
            QStringLiteral("other_refresh_graph"));
    REQUIRE(required<QLineEdit>(form, "pelican.fullscreenPass.name")
                .text()
                .isEmpty());
    REQUIRE(required<QListWidget>(form, "pelican.fullscreenPass.inputs")
                .count() == 0);
    REQUIRE(required<QListWidget>(form, "pelican.fullscreenPass.colors")
                .count() == 0);
    REQUIRE(required<QLabel>(form, "pelican.fullscreenPass.planStatus")
                .text()
                .contains(QStringLiteral("discarded")));
}

TEST_CASE(
    "WP324 generated fullscreen shapes exactly match core acceptance",
    "[devstudio][fullscreen-pass][wp324][shape][exhaustive]") {
    (void)application();
    enum class SwapchainRole { none, input, color, depth };
    const auto &policy = Pelican::defaultPassShapePolicy();

    for (std::size_t color_count = 0; color_count <= 2; ++color_count) {
        for (const bool has_depth : {false, true}) {
            for (const int overlap : {0, 1, 2}) {
                for (const bool duplicate_input : {false, true}) {
                    for (const auto swapchain_role : {
                             SwapchainRole::none, SwapchainRole::input,
                             SwapchainRole::color, SwapchainRole::depth}) {
                        if ((swapchain_role == SwapchainRole::color &&
                             color_count == 0) ||
                            (swapchain_role == SwapchainRole::depth &&
                             !has_depth)) {
                            continue;
                        }

                        std::vector<std::string> colors;
                        if (color_count >= 1) colors.push_back("output_a");
                        if (color_count >= 2) colors.push_back("output_b");
                        if (swapchain_role == SwapchainRole::color) {
                            colors.front() = "swapchain";
                        }
                        std::optional<std::string> depth =
                            has_depth
                                ? std::optional<std::string>{"depth_a"}
                                : std::nullopt;
                        if (swapchain_role == SwapchainRole::depth) {
                            depth = "swapchain";
                        }

                        std::vector<std::string> inputs;
                        if (overlap != 0) {
                            const std::string overlapping =
                                colors.empty() ? "output_a" : colors.front();
                            inputs.push_back(
                                overlap == 2
                                    ? overlapping + "@history"
                                    : overlapping);
                        }
                        if (duplicate_input) {
                            inputs.push_back("input_a");
                            inputs.push_back("input_a");
                        }
                        if (swapchain_role == SwapchainRole::input) {
                            inputs.push_back("swapchain");
                        }

                        const Json authored{
                            {"name", "shape_case"},
                            {"type", "fullscreen"},
                            {"input", inputs},
                            {"output",
                             {{"color", colors},
                              {"depth", depth ? Json(*depth)
                                              : Json(nullptr)}}},
                            {"shader",
                             {{"vertex", "engine://fullscreen"},
                              {"fragment", "engine://fullscreen"}}},
                        };
                        const bool core_accepted =
                            coreAcceptsShape(authored, policy);

                        RefreshHarness refresh;
                        FullscreenPassWidget form{refresh.driver(), policy};
                        form.receiveResult(QByteArray::fromStdString(
                            shapePlan().dump()));
                        driveUncheckedShapeDraft(form, inputs, colors,
                                                 depth);
                        const bool studio_accepted =
                            axisState(
                                form,
                                "pelican.fullscreenPass.axis.passShape") ==
                            "valid";

                        INFO("color_count=" << color_count
                             << " depth=" << has_depth
                             << " overlap=" << overlap
                             << " duplicate=" << duplicate_input
                             << " swapchain_role="
                             << static_cast<int>(swapchain_role));
                        REQUIRE(studio_accepted == core_accepted);
                    }
                }
            }
        }
    }
}

TEST_CASE(
    "WP324 one immutable injected policy controls both core and studio",
    "[devstudio][fullscreen-pass][wp324][authority][negative-contrast]") {
    (void)application();
    const auto &production = Pelican::defaultPassShapePolicy();
    const Pelican::PassShapePolicy requires_input = [&] {
        auto policy = production;
        policy.types[static_cast<std::size_t>(
                         Pelican::RenderPassType::fullscreen)]
            .minimum_inputs = 1;
        return policy;
    }();
    REQUIRE(Pelican::passShapeTypePolicy(
                production, Pelican::RenderPassType::fullscreen)
                .minimum_inputs == 0);
    REQUIRE(Pelican::passShapeTypePolicy(
                requires_input, Pelican::RenderPassType::fullscreen)
                .minimum_inputs == 1);

    const Json authored{
        {"name", "empty_input"},
        {"type", "fullscreen"},
        {"input", Json::array()},
        {"output", {{"color", "output_a"}, {"depth", nullptr}}},
        {"shader",
         {{"vertex", "engine://fullscreen"},
          {"fragment", "engine://fullscreen"}}},
    };
    REQUIRE(coreAcceptsShape(authored, production));
    REQUIRE_FALSE(coreAcceptsShape(authored, requires_input));

    RefreshHarness production_refresh;
    FullscreenPassWidget production_form{production_refresh.driver(),
                                         production};
    production_form.receiveResult(
        QByteArray::fromStdString(shapePlan().dump()));
    driveShapeDraft(production_form, {}, {"output_a"}, std::nullopt);
    REQUIRE(axisState(
                production_form,
                "pelican.fullscreenPass.axis.passShape") == "valid");

    RefreshHarness injected_refresh;
    FullscreenPassWidget injected_form{injected_refresh.driver(),
                                       requires_input};
    injected_form.receiveResult(
        QByteArray::fromStdString(shapePlan().dump()));
    driveShapeDraft(injected_form, {}, {"output_a"}, std::nullopt);
    REQUIRE(axisState(
                injected_form,
                "pelican.fullscreenPass.axis.passShape") == "invalid");
    REQUIRE(shapeViolations(injected_form).contains("input_count"));
}

TEST_CASE(
    "WP327 fullscreen form owns temporary and externally mutable injected policies",
    "[devstudio][fullscreen-pass][wp327][policy-lifetime][negative-contrast]") {
    (void)application();
    const auto rejecting_policy = [] {
        auto policy = Pelican::defaultPassShapePolicy();
        policy.swapchain_color_output_allowed = false;
        return policy;
    };

    RefreshHarness temporary_refresh;
    FullscreenPassWidget temporary_form{
        temporary_refresh.driver(), rejecting_policy()};
    temporary_form.receiveResult(
        QByteArray::fromStdString(shapePlan().dump()));
    driveShapeDraft(temporary_form, {}, {"swapchain"}, std::nullopt);
    REQUIRE(axisState(
                temporary_form,
                "pelican.fullscreenPass.axis.passShape") == "invalid");
    REQUIRE(shapeViolations(temporary_form).contains(
        "swapchain_color_output"));

    auto externally_mutable = rejecting_policy();
    RefreshHarness owned_refresh;
    FullscreenPassWidget owned_form{owned_refresh.driver(),
                                    externally_mutable};
    externally_mutable.swapchain_color_output_allowed = true;
    owned_form.receiveResult(QByteArray::fromStdString(shapePlan().dump()));
    driveShapeDraft(owned_form, {}, {"swapchain"}, std::nullopt);
    REQUIRE(axisState(
                owned_form,
                "pelican.fullscreenPass.axis.passShape") == "invalid");
    REQUIRE(shapeViolations(owned_form).contains(
        "swapchain_color_output"));

    RefreshHarness allowed_refresh;
    FullscreenPassWidget allowed_form{
        allowed_refresh.driver(), Pelican::defaultPassShapePolicy()};
    allowed_form.receiveResult(
        QByteArray::fromStdString(shapePlan().dump()));
    driveShapeDraft(allowed_form, {}, {"swapchain"}, std::nullopt);
    REQUIRE(axisState(
                allowed_form,
                "pelican.fullscreenPass.axis.passShape") == "valid");
}

TEST_CASE(
    "WP324 history and swapchain roles have explicit opposite controls",
    "[devstudio][fullscreen-pass][wp324][history][swapchain]") {
    (void)application();
    const auto &policy = Pelican::defaultPassShapePolicy();
    const auto assess = [&](std::vector<std::string> inputs,
                            std::vector<std::string> colors,
                            std::optional<std::string> depth) {
        const Json authored{
            {"name", "role_case"},
            {"type", "fullscreen"},
            {"input", inputs},
            {"output",
             {{"color", colors},
              {"depth", depth ? Json(*depth) : Json(nullptr)}}},
            {"shader",
             {{"vertex", "engine://fullscreen"},
              {"fragment", "engine://fullscreen"}}},
        };
        RefreshHarness refresh;
        FullscreenPassWidget form{refresh.driver(), policy};
        form.receiveResult(
            QByteArray::fromStdString(shapePlan().dump()));
        driveUncheckedShapeDraft(form, inputs, colors, depth);
        const bool studio = axisState(
                                form,
                                "pelican.fullscreenPass.axis.passShape") ==
                            "valid";
        const bool core = coreAcceptsShape(authored, policy);
        REQUIRE(studio == core);
        return studio;
    };

    REQUIRE_FALSE(assess({"taa_accum"}, {"taa_accum"}, std::nullopt));
    REQUIRE(assess({"taa_accum@history"}, {"taa_accum"}, std::nullopt));

    REQUIRE_FALSE(assess({"swapchain"}, {"output_a"}, std::nullopt));
    REQUIRE_FALSE(assess({}, {"output_a"}, "swapchain"));
    REQUIRE(assess({}, {"swapchain"}, std::nullopt));
}

TEST_CASE(
    "WP327 resolved vrm XR frame plan and injected policy flip core and form together",
    "[devstudio][fullscreen-pass][wp327][vrm-xr][real-file][negative-contrast]") {
    (void)application();
    const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const Json config = readJson(
        root / "projects" / "vrm_xr_demo" / "passes" / "main.json");
    const auto &graph = config.at("rendering_passes").at(0);
    const Json &lighting = findPass(
        config, graph.at("name").get<std::string>(), "lighting_pass");
    const Json plan = resolvedFramePlanFromAuthoredConfig(
        config, "projects/vrm_xr_demo/passes/main.json");

    const Json *resolved_swapchain = nullptr;
    for (const auto &resource : plan.at("resources")) {
        if (resource.at("name") == "swapchain") {
            resolved_swapchain = &resource;
            break;
        }
    }
    REQUIRE(resolved_swapchain != nullptr);
    REQUIRE(resolved_swapchain->at("kind") == "frame_target");
    REQUIRE(plan.at("graph") == graph.at("name"));

    CoreShapeResources core_resources;
    for (const auto &target : config.at("render_targets")) {
        const auto name = target.at("name").get<std::string>();
        core_resources.render_targets.push_back(name);
        if (target.value("history", false)) {
            core_resources.history_targets.insert(name);
        }
    }
    const auto &production = Pelican::defaultPassShapePolicy();
    auto rejects_swapchain_color = production;
    rejects_swapchain_color.swapchain_color_output_allowed = false;
    REQUIRE(production.swapchain_color_output_allowed);
    REQUIRE_FALSE(rejects_swapchain_color.swapchain_color_output_allowed);

    const auto parsed = parseCoreShape(
        lighting, production, core_resources);
    REQUIRE(parsed.output_color.size() == 1);
    REQUIRE(Pelican::isSwapchainRenderTarget(
        parsed.output_color.front().target));
    REQUIRE_THROWS_WITH(
        parseCoreShape(lighting, rejects_swapchain_color, core_resources),
        Catch::Matchers::ContainsSubstring("swapchain_color_output"));

    RefreshHarness production_refresh;
    FullscreenPassWidget production_form{production_refresh.driver(),
                                         production};
    production_form.receiveAuthoringConfig(
        QByteArray::fromStdString(config.dump()));
    production_form.receiveResult(
        QByteArray::fromStdString(plan.dump()));
    driveFromDeclaration(production_form, lighting);

    REQUIRE(outputJson(production_form).at("output").at("color") ==
            Json::array({"swapchain"}));
    REQUIRE(axisState(
                production_form,
                "pelican.fullscreenPass.axis.passShape") == "valid");
    REQUIRE(axisState(
                production_form,
                "pelican.fullscreenPass.axis.targetNames") == "valid");

    RefreshHarness rejecting_refresh;
    FullscreenPassWidget rejecting_form{rejecting_refresh.driver(),
                                        rejects_swapchain_color};
    rejecting_form.receiveAuthoringConfig(
        QByteArray::fromStdString(config.dump()));
    rejecting_form.receiveResult(
        QByteArray::fromStdString(plan.dump()));
    driveFromDeclaration(rejecting_form, lighting);
    REQUIRE(outputJson(rejecting_form).at("output").at("color") ==
            Json::array({"swapchain"}));
    REQUIRE(axisState(
                rejecting_form,
                "pelican.fullscreenPass.axis.targetNames") == "valid");
    REQUIRE(axisState(
                rejecting_form,
                "pelican.fullscreenPass.axis.passShape") == "invalid");
    REQUIRE(shapeViolations(rejecting_form).contains(
        "swapchain_color_output"));
}

TEST_CASE(
    "WP324 paste position comes from authored passes rather than runtime nodes",
    "[devstudio][fullscreen-pass][wp324][position][real-file]") {
    (void)application();
    const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const Json config = readJson(
        root / "projects" / "example" / "passes" /
        "main_rendering_config.json");
    const Json plan = readJson(
        root / "test" / "fixtures" / "devstudio" /
        "example_frame_plan.json");
    const std::size_t authored_count =
        config.at("rendering_passes").at(0).at("passes").size();
    REQUIRE(plan.at("nodes").size() != authored_count);

    RefreshHarness refresh;
    FullscreenPassWidget form{refresh.driver()};
    form.receiveAuthoringConfig(QByteArray::fromStdString(config.dump()));
    form.receiveResult(QByteArray::fromStdString(plan.dump()));
    auto &position = required<QSpinBox>(
        form, "pelican.fullscreenPass.position");
    REQUIRE(position.value() == static_cast<int>(authored_count));
    REQUIRE(position.value() == 20);
    REQUIRE(position.value() != static_cast<int>(plan.at("nodes").size()));
}

TEST_CASE(
    "WP324 resource kind failures stay distinct through model and widget production paths",
    "[devstudio][fullscreen-pass][wp324][resource-kind][fail-fast]") {
    (void)application();
    const std::array invalid_resources{
        std::pair{std::string{"resource_kind_missing"},
                  Json{{"name", "target"}, {"source", "project"}}},
        std::pair{std::string{"resource_kind_null"},
                  Json{{"name", "target"},
                       {"kind", nullptr},
                       {"source", "project"}}},
        std::pair{std::string{"resource_kind_empty"},
                  Json{{"name", "target"},
                       {"kind", ""},
                       {"source", "project"}}},
        std::pair{std::string{"resource_kind_unknown"},
                  Json{{"name", "target"},
                       {"kind", "texture"},
                       {"source", "project"}}},
    };

    for (const auto &[error_name, invalid_resource] : invalid_resources) {
        const Json plan = minimalFramePlan(
            "kind_graph", Json::array({invalid_resource}));
        REQUIRE_THROWS_WITH(
            buildFramePlanModel(plan.dump()),
            Catch::Matchers::ContainsSubstring(error_name));

        RefreshHarness refresh;
        FullscreenPassWidget form{refresh.driver()};
        form.receiveResult(QByteArray::fromStdString(plan.dump()));
        REQUIRE(required<QLabel>(
                    form, "pelican.fullscreenPass.planStatus")
                    .text()
                    .contains(QString::fromStdString(error_name)));
        REQUIRE(required<QComboBox>(
                    form, "pelican.fullscreenPass.inputTarget")
                    .count() == 0);
        REQUIRE(required<QLabel>(
                    form, "pelican.fullscreenPass.graph")
                    .text() == QStringLiteral("(no frame plan)"));
    }

    const Json valid = minimalFramePlan(
        "kind_graph", Json::array({resource("target")}));
    REQUIRE_NOTHROW(buildFramePlanModel(valid.dump()));
    RefreshHarness refresh;
    FullscreenPassWidget form{refresh.driver()};
    form.receiveResult(QByteArray::fromStdString(valid.dump()));
    REQUIRE(comboNames(required<QComboBox>(
                form, "pelican.fullscreenPass.inputTarget")) ==
            StringSet{"target"});
}

TEST_CASE(
    "WP324 collision axis joins authored and runtime names and requires an authored graph",
    "[devstudio][fullscreen-pass][wp324][collision]") {
    (void)application();
    Json plan = minimalFramePlan("collision_graph", Json::array());
    plan["nodes"] = Json::array({
        {{"name", "runtime_only"},
         {"kind", "render"},
         {"declaration_index", 0},
         {"order", 0},
         {"level", 0},
         {"reads", Json::array()},
         {"writes", Json::array()}},
    });
    const Json authored{
        {"rendering_passes",
         Json::array({
             {{"name", "collision_graph"},
              {"passes", Json::array({Json{{"name", "authored_only"}}})}},
         })},
    };

    RefreshHarness refresh;
    FullscreenPassWidget form{refresh.driver()};
    form.receiveAuthoringConfig(QByteArray::fromStdString(authored.dump()));
    form.receiveResult(QByteArray::fromStdString(plan.dump()));
    auto &name = required<QLineEdit>(
        form, "pelican.fullscreenPass.name");

    name.setText(QStringLiteral("authored_only"));
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.nameCollision") ==
            "invalid");
    name.setText(QStringLiteral("runtime_only"));
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.nameCollision") ==
            "invalid");
    name.setText(QStringLiteral("absent_from_both"));
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.nameCollision") ==
            "valid");

    const Json other_graph{
        {"rendering_passes",
         Json::array({
             {{"name", "other_graph"}, {"passes", Json::array()}},
         })},
    };
    form.receiveAuthoringConfig(
        QByteArray::fromStdString(other_graph.dump()));
    name.setText(QStringLiteral("runtime_only"));
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.nameCollision") ==
            "not_checked");
    REQUIRE(axisText(
                form, "pelican.fullscreenPass.axis.nameCollision")
                .contains(QStringLiteral("no graph 'collision_graph'")));
}

TEST_CASE(
    "WP324 refresh failure invalidates plan binding draft and each dependent axis",
    "[devstudio][fullscreen-pass][wp324][refresh][failure]") {
    (void)application();
    RefreshHarness refresh;
    FullscreenPassWidget form{refresh.driver()};
    const Json authored{
        {"rendering_passes",
         Json::array({
             {{"name", "refresh_graph"}, {"passes", Json::array()}},
         })},
    };
    const Json plan = minimalFramePlan(
        "refresh_graph",
        Json::array({resource("input_a"), resource("output_a")}));
    form.receiveAuthoringConfig(QByteArray::fromStdString(authored.dump()));
    form.receiveResult(QByteArray::fromStdString(plan.dump()));
    driveShapeDraft(form, {"input_a"}, {"output_a"}, std::nullopt);
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.passShape") == "valid");
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.nameCollision") ==
            "valid");
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.targetNames") == "valid");

    auto &refresh_button = required<QPushButton>(
        form, "pelican.fullscreenPass.refresh");
    refresh_button.click();
    REQUIRE(refresh.requests.size() == 1);
    refresh.fail(refresh.requests.back(),
                 QStringLiteral("synthetic failure"));

    REQUIRE(required<QLabel>(
                form, "pelican.fullscreenPass.graph")
                .text() == QStringLiteral("(no frame plan)"));
    REQUIRE(required<QComboBox>(
                form, "pelican.fullscreenPass.inputTarget")
                .count() == 0);
    REQUIRE(required<QLineEdit>(
                form, "pelican.fullscreenPass.name")
                .text()
                .isEmpty());
    REQUIRE(required<QListWidget>(
                form, "pelican.fullscreenPass.inputs")
                .count() == 0);
    REQUIRE(required<QListWidget>(
                form, "pelican.fullscreenPass.colors")
                .count() == 0);
    REQUIRE(required<QSpinBox>(
                form, "pelican.fullscreenPass.position")
                .value() == 0);
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.passShape") ==
            "invalid");
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.nameCollision") ==
            "not_checked");
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.targetNames") ==
            "not_checked");
    REQUIRE(axisState(
                form, "pelican.fullscreenPass.axis.historySupport") ==
            "not_checked");
    REQUIRE(axisText(
                form, "pelican.fullscreenPass.axis.historySupport")
                .contains(QStringLiteral("no frame plan")));
    REQUIRE(axisText(
                form, "pelican.fullscreenPass.axis.targetUsage")
                .contains(QStringLiteral("no frame plan")));
    REQUIRE(axisText(
                form, "pelican.fullscreenPass.axis.generationOrder")
                .contains(QStringLiteral("no frame plan")));

    auto &name_after_failure = required<QLineEdit>(
        form, "pelican.fullscreenPass.name");
    name_after_failure.setText(QStringLiteral("new_after_failure"));
    const Json first_rebound_plan = minimalFramePlan(
        "first_rebound_graph",
        Json::array({resource("input_b"), resource("output_b")}));
    form.receiveResult(
        QByteArray::fromStdString(first_rebound_plan.dump()));
    REQUIRE(name_after_failure.text() ==
            QStringLiteral("new_after_failure"));
}

} // namespace PelicanStudio
