#include "embeddedviewport.hpp"
#include "frameplangraphics.hpp"
#include "frameplanwidget.hpp"

#include "../src/core/loader/engineresources.hpp"
#include "../src/core/renderingpass/frameexecutionadapter.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/project/executionplan.hpp"
#include "../src/project/frameresolutionwire.hpp"
#include "../src/project/renderpipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QAction>
#include <QBrush>
#include <QByteArray>
#include <QColor>
#include <QComboBox>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QMenu>
#include <QPainterPath>
#include <QPen>
#include <QLabel>
#include <QSpinBox>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <queue>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;
using StringSet = std::set<std::string, std::less<>>;
using EdgePair = std::pair<std::string, std::string>;
using EdgeSet = std::set<EdgePair>;

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_frameplan_graph_test";
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

std::string loadEngineDocument(std::string_view reference) {
    constexpr std::string_view prefix = "engine://";
    if (!reference.starts_with(prefix)) {
        throw std::runtime_error("expected an engine document reference: " +
                                 std::string{reference});
    }
    return Pelican::engineResourceOrThrow(reference.substr(prefix.size()));
}

Pelican::RenderingTargetPlanDeviceFacts planningDeviceFacts() {
    return Pelican::RenderingTargetPlanDeviceFacts{
        .multiview = true,
        .max_multiview_view_count = 8,
        .query_attachment_samples =
            [](const Pelican::RenderTargetDefinition &) {
                return std::vector<std::uint32_t>{1};
            },
        .query_image_format_capability =
            [](const Pelican::RenderTargetDefinition &) {
                return Pelican::RenderingImageFormatCapability{
                    .image_usage_supported = true,
                    .supported_samples = {1},
                    .max_mip_levels = 16,
                    .max_array_layers = 8,
                    .transient_attachment_supported = true,
                    .local_read_attachment_supported = true,
                };
            },
        .transient_attachments = true,
        .dynamic_rendering_local_read = true,
    };
}

Pelican::ResolvedResourceExtent resolveRuntimeExtent(
    const Pelican::VulkanTargetPlan &plan,
    Pelican::ResolvedResourceExtent output_extent,
    std::string_view resource) {
    if (resource == "swapchain") {
        return output_extent;
    }
    const auto found = std::ranges::find(
        plan.resources, resource,
        &Pelican::VulkanPhysicalResourcePlan::logical_resource);
    if (found == plan.resources.end() || !found->extent) {
        throw std::runtime_error(
            "runtime test resolver has no extent contract for " +
            std::string{resource});
    }
    return Pelican::resolveResourceExtent(
        *found->extent, output_extent);
}

void publishRuntimeResolution(
    Json &wire, const Pelican::VulkanTargetPlan &plan,
    Pelican::ResolvedResourceExtent output_extent) {
    wire["runtime_resolution"] =
        Pelican::frameRuntimeResolutionWireToJson(
            Pelican::makeFrameRuntimeResolutionWire(
                plan, [&](std::string_view resource) {
                    return resolveRuntimeExtent(
                        plan, output_extent, resource);
                }));
}

Json resolvedFramePlan(Pelican::ResolvedRenderPipeline resolved,
                       std::string_view label,
                       bool publish_physical_plan = true) {
    // Runtime resolution publishes the concrete display extent before frame
    // planning. Reproduce that CPU-only boundary without duplicating Studio's
    // separate extent arithmetic (WP315).
    for (auto &target : resolved.normalized_config.at("render_targets")) {
        if (target.value("format_class", std::string{}) == "display") {
            target["width"] = 160;
            target["height"] = 90;
        }
    }
    const Pelican::CompiledRenderPipeline compiled =
        Pelican::compileRenderPipeline(resolved);
    const auto graphs = Pelican::parseFrameGraphDefinitionsFromConfigJson(
        resolved.normalized_config);
    if (graphs.size() != 1) {
        throw std::runtime_error(std::string{label} +
                                 " must resolve to exactly one frame graph");
    }
    const Pelican::FramePlan plan = Pelican::planFrameGraph(graphs.front());
    Json wire = Pelican::framePlanToJson(plan, &compiled);
    const Pelican::FrameExecutionPlan execution =
        Pelican::compileFrameExecutionPlan(
            graphs.front(), plan,
            Pelican::ExecutionEndpoint{
                .id = "device:0",
                .endpoint_class = Pelican::ExecutionEndpointClass::device,
                .backend = "vulkan",
            });
    wire["execution_plan"] = Pelican::frameExecutionPlanToJson(execution);
    if (!publish_physical_plan) {
        return wire;
    }
    const auto targets = Pelican::parseRenderTargetDefinitionsFromJson(
        resolved.normalized_config);
    const auto physical = Pelican::compileRenderingTargetPlans(
        graphs, targets, compiled.sample_count_policy,
        vk::Format::eB8G8R8A8Srgb, planningDeviceFacts(), std::nullopt,
        std::nullopt, compiled.target_planning, compiled.vulkan_plan_pins,
        compiled.vulkan_physical_fragments);
    if (physical.plans.size() != 1) {
        throw std::runtime_error(std::string{label} +
                                 " must compile exactly one physical target "
                                 "plan");
    }
    wire["physical_target_plan"] =
        Pelican::vulkanTargetPlanToJson(*physical.plans.front());
    publishRuntimeResolution(
        wire, *physical.plans.front(),
        Pelican::ResolvedResourceExtent{160, 90});
    return wire;
}

Json animgraphFramePlan(
    bool shadow_directional_enabled,
    Pelican::RenderPipelineGraphVariant variant =
        Pelican::RenderPipelineGraphVariant::flat) {
    Json authored = readJson(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                             "projects" / "animgraph_demo" / "passes" /
                             "main.json");
#if PELICAN_RUNTIME_SHADER_COMPILER
    if (!shadow_directional_enabled) {
        Json retained = Json::array();
        for (const auto &feature : authored.at("features")) {
            if (feature != "engine://features/shadow_directional.json") {
                retained.push_back(feature);
            }
        }
        authored["features"] = std::move(retained);
    }
#else
    (void)shadow_directional_enabled;
    authored["features"] = Json::array();
#endif

    auto resolved = Pelican::resolveRenderPipeline(
        Pelican::RenderPipelineRequest{
            .authored_config = std::move(authored),
            .source_name = "projects/animgraph_demo/passes/main.json",
        },
        Pelican::RenderEnvironmentCapabilities{
            .runtime_shader_compiler_enabled =
                PELICAN_RUNTIME_SHADER_COMPILER != 0,
            .graph_variant = variant,
        },
        Pelican::RenderPipelineResolveDependencies{
            .load_feature_json = loadEngineDocument,
            .load_pipeline_json = loadEngineDocument,
        });
    return resolvedFramePlan(
        std::move(resolved), "animgraph_demo",
        variant != Pelican::RenderPipelineGraphVariant::preview);
}

Json exampleFramePlan(bool ui_enabled) {
    Json authored = readJson(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                             "projects" / "example" / "passes" /
                             "main_rendering_config.json");
#if PELICAN_RUNTIME_SHADER_COMPILER
    if (!ui_enabled) {
        authored["features"] = Json::array();
    }
#else
    (void)ui_enabled;
    authored["features"] = Json::array();
#endif
    auto resolved = Pelican::resolveRenderPipeline(
        Pelican::RenderPipelineRequest{
            .authored_config = std::move(authored),
            .source_name =
                "projects/example/passes/main_rendering_config.json",
        },
        Pelican::RenderEnvironmentCapabilities{
            .runtime_shader_compiler_enabled =
                PELICAN_RUNTIME_SHADER_COMPILER != 0,
            .graph_variant = Pelican::RenderPipelineGraphVariant::flat,
        },
        Pelican::RenderPipelineResolveDependencies{
            .load_feature_json = loadEngineDocument,
            .load_pipeline_json = loadEngineDocument,
        });
    return resolvedFramePlan(std::move(resolved), "example");
}

Json independentOpportunityFramePlan(
    Pelican::PlanningProfileKind profile_kind) {
    const Json config{
        {"render_targets",
         Json::array(
             {{{"name", "alpha_output"},
               {"extent_scale", 1.0},
               {"width", 32},
               {"height", 32},
               {"format", "R8G8B8A8_UNORM"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
              {{"name", "beta_output"},
               {"extent_scale", 1.0},
               {"width", 32},
               {"height", 32},
               {"format", "R8G8B8A8_UNORM"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}})},
        {"rendering_passes",
         Json::array(
             {{{"name", "independent_opportunities"},
               {"passes",
                Json::array(
                    {{{"name", "alpha"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", "alpha_output"}, {"depth", nullptr}}}},
                     {{"name", "beta"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", "beta_output"},
                        {"depth", nullptr}}}}})}}})},
    };
    const auto graphs =
        Pelican::parseFrameGraphDefinitionsFromConfigJson(config);
    const auto targets =
        Pelican::parseRenderTargetDefinitionsFromJson(config);
    const auto compilation = Pelican::compileRenderingTargetPlans(
        graphs, targets, Pelican::compileSampleCountPolicy(config),
        vk::Format::eB8G8R8A8Unorm,
        planningDeviceFacts(),
        std::nullopt, std::nullopt,
        Pelican::TargetPlanningPolicy{
            .profile = Pelican::PlanningProfile{.kind = profile_kind},
        });
    if (graphs.size() != 1 || compilation.plans.size() != 1) {
        throw std::runtime_error(
            "independent opportunity producer must compile one graph and one "
            "physical plan");
    }

    const Pelican::FramePlan plan = Pelican::planFrameGraph(graphs.front());
    Json wire = Pelican::framePlanToJson(plan);
    const Pelican::FrameExecutionPlan execution =
        Pelican::compileFrameExecutionPlan(
            graphs.front(), plan,
            Pelican::ExecutionEndpoint{
                .id = "device:0",
                .endpoint_class = Pelican::ExecutionEndpointClass::device,
                .backend = "vulkan",
            });
    wire["execution_plan"] = Pelican::frameExecutionPlanToJson(execution);
    wire["physical_target_plan"] =
        Pelican::vulkanTargetPlanToJson(*compilation.plans.front());
    publishRuntimeResolution(
        wire, *compilation.plans.front(),
        Pelican::ResolvedResourceExtent{32, 32});
    return wire;
}

Json fractionalScaleFramePlan() {
    const Json config{
        {"render_targets",
         Json::array(
             {{{"name", "scaled_scene"},
               {"extent_scale", 0.7},
               {"format", "R8G8B8A8_UNORM"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
              {{"name", "custom_output"},
               {"extent_scale", 1.0},
               {"width", 10},
               {"height", 10},
               {"format", "R8G8B8A8_UNORM"},
               {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}})},
        {"rendering_passes",
         Json::array(
             {{{"name", "fractional_scale"},
               {"passes",
                Json::array(
                    {{{"name", "scene"},
                      {"type", "fullscreen"},
                      {"resolution_domain", "scene"},
                      {"output",
                       {{"color", "scaled_scene"}, {"depth", nullptr}}}},
                     {{"name", "present"},
                      {"type", "fullscreen"},
                      {"resolution_domain", "output"},
                      {"input", Json::array({"scaled_scene"})},
                      {"output",
                       {{"color", "custom_output"},
                        {"depth", nullptr}}}}})}}})},
    };
    const auto graphs =
        Pelican::parseFrameGraphDefinitionsFromConfigJson(config);
    const auto targets =
        Pelican::parseRenderTargetDefinitionsFromJson(config);
    const auto compilation = Pelican::compileRenderingTargetPlans(
        graphs, targets, Pelican::compileSampleCountPolicy(config),
        vk::Format::eB8G8R8A8Unorm, planningDeviceFacts());
    if (graphs.size() != 1 || compilation.plans.size() != 1) {
        throw std::runtime_error(
            "fractional scale producer must compile one graph and one "
            "physical plan");
    }

    const Pelican::FramePlan plan =
        Pelican::planFrameGraph(graphs.front());
    Json wire = Pelican::framePlanToJson(plan);
    wire["execution_plan"] = Pelican::frameExecutionPlanToJson(
        Pelican::compileFrameExecutionPlan(
            graphs.front(), plan,
            Pelican::ExecutionEndpoint{
                .id = "device:0",
                .endpoint_class =
                    Pelican::ExecutionEndpointClass::device,
                .backend = "vulkan",
            }));
    wire["physical_target_plan"] =
        Pelican::vulkanTargetPlanToJson(*compilation.plans.front());
    publishRuntimeResolution(
        wire, *compilation.plans.front(),
        Pelican::ResolvedResourceExtent{10, 10});
    return wire;
}

QGraphicsScene &scene(FramePlanWidget &widget) {
    auto *view = widget.findChild<QGraphicsView *>(
        QStringLiteral("pelican.framePlanLogicalView"));
    if (view == nullptr || view->scene() == nullptr) {
        throw std::runtime_error("logical frame-plan scene was not installed");
    }
    return *view->scene();
}

QComboBox &targetSelector(FramePlanWidget &widget) {
    auto *selector = widget.findChild<QComboBox *>(
        QStringLiteral("pelican.framePlanTarget"));
    if (selector == nullptr) {
        throw std::runtime_error("frame-plan target control was not installed");
    }
    return *selector;
}

QSpinBox &subtreeDepth(FramePlanWidget &widget) {
    auto *spin = widget.findChild<QSpinBox *>(
        QStringLiteral("pelican.framePlanDepth"));
    if (spin == nullptr) {
        throw std::runtime_error("frame-plan depth control was not installed");
    }
    return *spin;
}

QGraphicsView &logicalView(FramePlanWidget &widget) {
    auto *view = widget.findChild<QGraphicsView *>(
        QStringLiteral("pelican.framePlanLogicalView"));
    if (view == nullptr) {
        throw std::runtime_error("frame-plan logical view was not installed");
    }
    return *view;
}

QTreeWidget &logicalDetails(FramePlanWidget &widget) {
    auto *details = widget.findChild<QTreeWidget *>(
        QStringLiteral("pelican.framePlanLogicalDetails"));
    if (details == nullptr) {
        throw std::runtime_error(
            "frame-plan logical details pane was not installed");
    }
    return *details;
}

QString joinedValues(const std::vector<std::string> &values) {
    QStringList result;
    for (const auto &value : values) {
        result.push_back(QString::fromStdString(value));
    }
    return result.join(QStringLiteral(", "));
}

QString kind(const QGraphicsItem &item) {
    return item.data(FramePlanItemKindRole).toString();
}

std::vector<QGraphicsItem *> itemsOfKind(QGraphicsScene &value,
                                         const char *expected) {
    std::vector<QGraphicsItem *> result;
    for (QGraphicsItem *item : value.items()) {
        if (kind(*item) == QLatin1String{expected}) {
            result.push_back(item);
        }
    }
    return result;
}

StringSet sceneNodeNames(QGraphicsScene &value) {
    StringSet result;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanNodeItem)) {
        result.insert(item->data(FramePlanNameRole).toString().toStdString());
    }
    return result;
}

struct BundleObservation {
    std::string from;
    std::string to;
    qulonglong order = 0;
    QStringList records;
    QPainterPath path;

    bool operator==(const BundleObservation &) const = default;
};

std::vector<BundleObservation> sceneBundles(QGraphicsScene &value) {
    std::vector<BundleObservation> result;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanEdgeItem)) {
        auto *path = dynamic_cast<QGraphicsPathItem *>(item);
        if (path == nullptr) {
            throw std::runtime_error("frame-plan edge was not a path item");
        }
        result.push_back(BundleObservation{
            item->data(FramePlanFromNameRole).toString().toStdString(),
            item->data(FramePlanToNameRole).toString().toStdString(),
            item->data(FramePlanBundleOrderRole).toULongLong(),
            item->data(FramePlanEdgeRecordsRole).toStringList(), path->path()});
    }
    std::ranges::sort(result, {}, &BundleObservation::order);
    return result;
}

QGraphicsItem *nodeItem(QGraphicsScene &value, std::string_view name) {
    const QString expected = QString::fromUtf8(name.data(),
                                               static_cast<qsizetype>(name.size()));
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanNodeItem)) {
        if (item->data(FramePlanNameRole).toString() == expected) {
            return item;
        }
    }
    return nullptr;
}

QGraphicsItem *edgeItem(QGraphicsScene &value, std::string_view from,
                        std::string_view to) {
    const QString expected_from = QString::fromUtf8(
        from.data(), static_cast<qsizetype>(from.size()));
    const QString expected_to =
        QString::fromUtf8(to.data(), static_cast<qsizetype>(to.size()));
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanEdgeItem)) {
        if (item->data(FramePlanFromNameRole).toString() == expected_from &&
            item->data(FramePlanToNameRole).toString() == expected_to) {
            return item;
        }
    }
    return nullptr;
}

QGraphicsItem *physicalResourceItem(QGraphicsScene &value,
                                    std::string_view name) {
    const QString expected = QString::fromUtf8(
        name.data(), static_cast<qsizetype>(name.size()));
    for (QGraphicsItem *item :
         itemsOfKind(value, FramePlanResourceLifetimeItem)) {
        if (item->data(FramePlanNameRole).toString() == expected) {
            return item;
        }
    }
    return nullptr;
}

QGraphicsItem *physicalItemWithState(QGraphicsScene &value,
                                     const char *item_kind,
                                     std::string_view state) {
    const QString expected = QString::fromUtf8(
        state.data(), static_cast<qsizetype>(state.size()));
    for (QGraphicsItem *item : itemsOfKind(value, item_kind)) {
        if (item->data(FramePlanPhysicalStateRole).toString() == expected) {
            return item;
        }
    }
    return nullptr;
}

std::string selectedPlanningEndpoint(const Json &physical) {
    const auto &selection = physical.at("backend_selection");
    const std::string selected =
        selection.at("selected_candidate").get<std::string>();
    for (const auto &candidate : selection.at("candidates")) {
        if (candidate.at("candidate") == selected) {
            return candidate.at("endpoint").get<std::string>();
        }
    }
    throw std::runtime_error(
        "physical plan selected backend candidate has no endpoint");
}

StringSet strings(const QVariant &value) {
    StringSet result;
    for (const QString &entry : value.toStringList()) {
        result.insert(entry.toStdString());
    }
    return result;
}

StringSet sceneDependencyRecords(QGraphicsScene &value) {
    StringSet result;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanEdgeItem)) {
        const StringSet records = strings(item->data(FramePlanEdgeRecordsRole));
        result.insert(records.begin(), records.end());
    }
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanGroupItem)) {
        const StringSet records =
            strings(item->data(FramePlanInternalEdgeRecordsRole));
        result.insert(records.begin(), records.end());
    }
    return result;
}

EdgeSet sceneEdges(QGraphicsScene &value) {
    EdgeSet result;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanEdgeItem)) {
        result.emplace(item->data(FramePlanFromNameRole).toString().toStdString(),
                       item->data(FramePlanToNameRole).toString().toStdString());
    }
    return result;
}

QGraphicsItem *singleGroupItem(QGraphicsScene &value) {
    const auto groups = itemsOfKind(value, FramePlanGroupItem);
    REQUIRE(groups.size() == 1);
    return groups.front();
}

StringSet boundaryTargets(QGraphicsScene &value) {
    StringSet result;
    for (QGraphicsItem *item :
         itemsOfKind(value, FramePlanBoundaryStubItem)) {
        result.insert(
            item->data(FramePlanBoundaryTargetRole).toString().toStdString());
    }
    return result;
}

std::multiset<std::string> annotatedSceneItems(QGraphicsScene &value) {
    std::multiset<std::string> result;
    for (QGraphicsItem *item : value.items()) {
        const QString item_kind =
            item->data(FramePlanItemKindRole).toString();
        if (item_kind.isEmpty()) {
            continue;
        }
        const QString identity =
            QStringLiteral("%1|%2|%3|%4|%5")
                .arg(item_kind,
                     item->data(FramePlanNameRole).toString(),
                     item->data(FramePlanFromNameRole).toString(),
                     item->data(FramePlanToNameRole).toString(),
                     item->data(FramePlanMembersRole)
                         .toStringList()
                         .join(QLatin1Char(',')));
        result.insert(identity.toStdString());
    }
    return result;
}

EdgeSet transitiveReachability(const StringSet &nodes,
                               const EdgeSet &edges) {
    std::map<std::string, StringSet, std::less<>> adjacency;
    for (const auto &[from, to] : edges) {
        adjacency[from].insert(to);
    }

    EdgeSet result;
    for (const auto &start : nodes) {
        std::queue<std::string> frontier;
        StringSet visited;
        if (const auto found = adjacency.find(start);
            found != adjacency.end()) {
            for (const auto &next : found->second) {
                frontier.push(next);
            }
        }
        while (!frontier.empty()) {
            std::string current = std::move(frontier.front());
            frontier.pop();
            if (!visited.insert(current).second) {
                continue;
            }
            result.emplace(start, current);
            if (const auto found = adjacency.find(current);
                found != adjacency.end()) {
                for (const auto &next : found->second) {
                    frontier.push(next);
                }
            }
        }
    }
    return result;
}

QString edgeLabelText(QGraphicsScene &value, std::string_view from,
                      std::string_view to) {
    QGraphicsItem *box = nullptr;
    const QString expected_from = QString::fromUtf8(
        from.data(), static_cast<qsizetype>(from.size()));
    const QString expected_to = QString::fromUtf8(
        to.data(), static_cast<qsizetype>(to.size()));
    for (QGraphicsItem *candidate :
         itemsOfKind(value, FramePlanEdgeLabelItem)) {
        if (candidate->data(FramePlanFromNameRole).toString() ==
                expected_from &&
            candidate->data(FramePlanToNameRole).toString() ==
                expected_to) {
            REQUIRE(box == nullptr);
            box = candidate;
        }
    }
    REQUIRE(box != nullptr);
    QStringList labels;
    for (QGraphicsItem *child : box->childItems()) {
        if (auto *text = dynamic_cast<QGraphicsSimpleTextItem *>(child)) {
            labels.push_back(text->text());
        }
    }
    REQUIRE(labels.size() == 1);
    return labels.front();
}

FramePlanNode groupingNode(std::string name, std::size_t order,
                           std::string feature = {}) {
    FramePlanNode node;
    node.name = std::move(name);
    node.kind = "raster";
    node.source =
        feature.empty() ? "project" : "feature:" + feature;
    node.provider_feature = std::move(feature);
    node.order = order;
    node.reads = {"focus"};
    return node;
}

FramePlanModel convexGroupingModel() {
    FramePlanModel model;
    model.graph = "grouping_graph";
    model.execution_plan.state =
        FramePlanExecutionPlanState::available;
    model.execution_plan.graph = model.graph;

    FramePlanResource focus;
    focus.name = "focus";
    model.resources.push_back(std::move(focus));
    model.nodes = {
        groupingNode("upstream", 0),
        groupingNode("capture_a", 1, "cube_capture"),
        groupingNode("capture_b", 2, "cube_capture"),
        groupingNode("capture_c", 3, "cube_capture"),
        groupingNode("downstream", 4),
        // Deliberately occupies the preferred synthetic group name. The
        // rendered group must choose a distinct endpoint identity.
        groupingNode("__pelican_group__:cube_capture", 5),
    };
    model.dependencies = {
        {"upstream", "capture_a", "pelican.dependency.input_a@1",
         "input_a"},
        {"upstream", "capture_b", "pelican.dependency.input_b@1",
         "input_b"},
        {"capture_a", "capture_b", "pelican.dependency.internal_a@1",
         "inside_a"},
        {"capture_b", "capture_c", "pelican.dependency.internal_b@1",
         "inside_b"},
        {"capture_b", "downstream", "pelican.dependency.output_b@1",
         "output_b"},
        {"capture_c", "downstream", "pelican.dependency.output_c@1",
         "output_c"},
    };
    return model;
}

FramePlanModel nonConvexGroupingModel() {
    FramePlanModel model;
    model.graph = "non_convex_grouping_graph";
    model.execution_plan.state =
        FramePlanExecutionPlanState::available;
    model.execution_plan.graph = model.graph;

    FramePlanResource focus;
    focus.name = "focus";
    model.resources.push_back(std::move(focus));
    model.nodes = {
        groupingNode("inside_a", 0, "non_convex"),
        groupingNode("outside", 1),
        groupingNode("inside_b", 2, "non_convex"),
        groupingNode("project_peer", 3),
        groupingNode("single_feature_node", 4, "single"),
    };
    model.dependencies = {
        {"inside_a", "outside", "pelican.dependency.leave@1", "leave"},
        {"outside", "inside_b", "pelican.dependency.return@1", "return"},
    };
    return model;
}

void triggerContextAction(FramePlanGraphicsScene &scene,
                          QGraphicsItem &item,
                          const QString &action_text) {
    bool triggered = false;
    QTimer chooser;
    chooser.setInterval(0);
    QObject::connect(&chooser, &QTimer::timeout, &scene, [&] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *menu = qobject_cast<QMenu *>(widget);
            if (menu == nullptr || !menu->isVisible()) {
                continue;
            }
            for (QAction *action : menu->actions()) {
                if (action->text().contains(action_text)) {
                    triggered = true;
                    action->trigger();
                    menu->close();
                    return;
                }
            }
            menu->close();
        }
    });
    chooser.start();

    QGraphicsSceneContextMenuEvent event{
        QEvent::GraphicsSceneContextMenu};
    event.setScenePos(item.sceneBoundingRect().center());
    event.setScreenPos(QPoint{100, 100});
    QApplication::sendEvent(&scene, &event);
    chooser.stop();
    QApplication::processEvents();
    REQUIRE(triggered);
}

void doubleClickSceneItem(FramePlanGraphicsScene &scene,
                          QGraphicsItem &item) {
    QGraphicsSceneMouseEvent event{
        QEvent::GraphicsSceneMouseDoubleClick};
    event.setScenePos(item.sceneBoundingRect().center());
    event.setButton(Qt::LeftButton);
    event.setButtons(Qt::LeftButton);
    QApplication::sendEvent(&scene, &event);
    QApplication::processEvents();
}

StringSet wireNodeNames(const Json &wire) {
    StringSet result;
    for (const auto &node : wire.at("nodes")) {
        result.insert(node.at("name").get<std::string>());
    }
    return result;
}

EdgeSet wireEdges(const Json &wire) {
    EdgeSet result;
    for (const auto &dependency :
         wire.at("execution_plan").at("dependencies")) {
        result.emplace(dependency.at("from").get<std::string>(),
                       dependency.at("to").get<std::string>());
    }
    return result;
}

template <typename Set>
Set removedFrom(const Set &enabled, const Set &disabled) {
    Set result;
    std::set_difference(enabled.begin(), enabled.end(), disabled.begin(),
                        disabled.end(), std::inserter(result, result.end()));
    return result;
}

std::string edgeSetText(const EdgeSet &edges) {
    std::ostringstream output;
    for (const auto &[from, to] : edges) {
        output << from << " -> " << to << '\n';
    }
    return output.str();
}

struct ExpectedNode {
    std::string name;
    std::string source;
    bool anchor = false;
};

struct ExpectedDependency {
    std::string from;
    std::string to;
    std::string reason;
    std::string resource;
};

const std::vector<ExpectedNode> &expectedExpandedNodes() {
    static const std::vector<ExpectedNode> expected{
        {"gbuffer_pass", "project"},
        {"ssao_pass", "project"},
        {"ssao_blur_pass", "project"},
        {"lighting_pass", "project"},
        {"__snapshot_opaque_color", "project"},
        {"__snapshot_opaque_depth", "project"},
        {"forward_transparent", "project"},
        {"__anchor_sprite", "engine", true},
        {"__anchor_post_main", "engine", true},
        {"__anchor_tonemap", "engine", true},
        {"__anchor_post_ldr", "engine", true},
        {"HighLuminanceExtraction", "project"},
        {"HorizontalBlur_0", "project"},
        {"VerticalBlur_0", "project"},
        {"HorizontalBlur_1", "project"},
        {"VerticalBlur_1", "project"},
        {"HorizontalBlur_2", "project"},
        {"VerticalBlur_2", "project"},
        {"HorizontalBlur_3", "project"},
        {"VerticalBlur_3", "project"},
        {"UpsampleBlend_3", "project"},
        {"UpsampleBlend_2", "project"},
        {"UpsampleBlend_1", "project"},
        {"FinalBloomComposite", "project"},
        {"__anchor_pelican_ui", "engine", true},
        {"pelican_ui", "feature:ui"},
        {"__anchor_debug_draw", "engine", true},
        {"__anchor_debug_text", "engine", true},
        {"__anchor_imgui", "engine", true},
        {"output_transform", "engine"},
    };
    return expected;
}

const StringSet &expectedBloomMembers() {
    static const StringSet expected{
        "FinalBloomComposite",      "HighLuminanceExtraction",
        "HorizontalBlur_0",         "HorizontalBlur_1",
        "HorizontalBlur_2",         "HorizontalBlur_3",
        "UpsampleBlend_1",          "UpsampleBlend_2",
        "UpsampleBlend_3",          "VerticalBlur_0",
        "VerticalBlur_1",           "VerticalBlur_2",
        "VerticalBlur_3",
    };
    return expected;
}

const std::vector<ExpectedDependency> &expectedExpandedDependencies() {
    static const auto expected = [] {
        constexpr std::string_view explicit_after =
            "pelican.dependency.explicit_after@1";
        constexpr std::string_view read_after_write =
            "pelican.dependency.read_after_write@1";
        constexpr std::string_view snapshot_after =
            "pelican.dependency.snapshot_after@1";
        std::vector<ExpectedDependency> result;
        result.reserve(100);
        const auto add = [&](std::string_view from, std::string_view to,
                             std::string_view reason,
                             std::string_view resource = {}) {
            result.push_back(ExpectedDependency{
                std::string{from}, std::string{to}, std::string{reason},
                std::string{resource}});
        };

        add("__anchor_debug_draw", "__anchor_debug_text", explicit_after);
        add("__anchor_debug_text", "__anchor_imgui", explicit_after);
        add("__anchor_imgui", "output_transform", explicit_after);
        add("__anchor_pelican_ui", "__anchor_debug_draw", explicit_after);
        add("__anchor_pelican_ui", "pelican_ui", explicit_after);
        add("__anchor_post_main", "__anchor_tonemap", explicit_after);
        add("__anchor_sprite", "__anchor_post_main", explicit_after);
        add("__anchor_tonemap", "__anchor_post_ldr", explicit_after);

        add("__snapshot_opaque_color", "__anchor_sprite", explicit_after);
        add("__snapshot_opaque_color", "__snapshot_opaque_depth",
            explicit_after);
        add("__snapshot_opaque_color", "forward_transparent",
            read_after_write, "opaque_color");
        add("__snapshot_opaque_depth", "__anchor_sprite", explicit_after);
        add("__snapshot_opaque_depth", "forward_transparent",
            explicit_after);
        add("__snapshot_opaque_depth", "forward_transparent",
            read_after_write, "opaque_depth");
        add("forward_transparent", "__anchor_sprite", explicit_after);
        add("forward_transparent", "FinalBloomComposite", read_after_write,
            "lit_color");
        add("forward_transparent", "HighLuminanceExtraction",
            read_after_write, "lit_color");

        add("gbuffer_pass", "__anchor_sprite", explicit_after);
        add("gbuffer_pass", "__snapshot_opaque_depth", read_after_write,
            "offscreen_depth");
        add("gbuffer_pass", "forward_transparent", read_after_write,
            "offscreen_depth");
        for (const std::string_view resource :
             {std::string_view{"g_emissive"},
              std::string_view{"gbuffer_albedo"},
              std::string_view{"gbuffer_material"},
              std::string_view{"gbuffer_normal"},
              std::string_view{"gbuffer_worldpos"}}) {
            add("gbuffer_pass", "lighting_pass", read_after_write, resource);
        }
        add("gbuffer_pass", "ssao_pass", explicit_after);
        add("gbuffer_pass", "ssao_pass", read_after_write,
            "gbuffer_normal");
        add("gbuffer_pass", "ssao_pass", read_after_write,
            "gbuffer_worldpos");

        add("lighting_pass", "__anchor_sprite", explicit_after);
        add("lighting_pass", "__snapshot_opaque_color", explicit_after);
        add("lighting_pass", "__snapshot_opaque_color", read_after_write,
            "lit_color");
        add("lighting_pass", "__snapshot_opaque_color", snapshot_after);
        add("lighting_pass", "__snapshot_opaque_depth", snapshot_after);
        add("lighting_pass", "forward_transparent", read_after_write,
            "lit_color");
        add("pelican_ui", "__anchor_debug_draw", explicit_after);
        add("pelican_ui", "output_transform", read_after_write, "display");
        add("ssao_blur_pass", "__anchor_sprite", explicit_after);
        add("ssao_blur_pass", "lighting_pass", explicit_after);
        add("ssao_blur_pass", "lighting_pass", read_after_write,
            "ssao_blur");
        add("ssao_pass", "__anchor_sprite", explicit_after);
        add("ssao_pass", "ssao_blur_pass", explicit_after);
        add("ssao_pass", "ssao_blur_pass", read_after_write,
            "ssao_output");

        constexpr std::array bloom_members{
            std::string_view{"FinalBloomComposite"},
            std::string_view{"HighLuminanceExtraction"},
            std::string_view{"HorizontalBlur_0"},
            std::string_view{"HorizontalBlur_1"},
            std::string_view{"HorizontalBlur_2"},
            std::string_view{"HorizontalBlur_3"},
            std::string_view{"UpsampleBlend_1"},
            std::string_view{"UpsampleBlend_2"},
            std::string_view{"UpsampleBlend_3"},
            std::string_view{"VerticalBlur_0"},
            std::string_view{"VerticalBlur_1"},
            std::string_view{"VerticalBlur_2"},
            std::string_view{"VerticalBlur_3"},
        };
        for (const auto member : bloom_members) {
            add("__anchor_post_ldr", member, explicit_after);
        }
        add("__anchor_post_ldr", "__anchor_pelican_ui", explicit_after);

        add("HighLuminanceExtraction", "HorizontalBlur_0",
            explicit_after);
        add("HighLuminanceExtraction", "HorizontalBlur_0",
            read_after_write, "Bloom_Threshold_RT");
        add("HighLuminanceExtraction", "__anchor_pelican_ui",
            explicit_after);
        for (int index = 0; index != 3; ++index) {
            const std::string horizontal =
                "HorizontalBlur_" + std::to_string(index);
            const std::string vertical =
                "VerticalBlur_" + std::to_string(index);
            const std::string upsample =
                "UpsampleBlend_" + std::to_string(index + 1);
            const std::string resource =
                "Bloom_Downsample_H_" + std::to_string(index) + "_RT";
            add(horizontal, vertical, explicit_after);
            add(horizontal, vertical, read_after_write, resource);
            add(horizontal, upsample, read_after_write, resource);
            add(horizontal, "__anchor_pelican_ui", explicit_after);
        }
        add("HorizontalBlur_3", "VerticalBlur_3", explicit_after);
        add("HorizontalBlur_3", "VerticalBlur_3", read_after_write,
            "Bloom_Downsample_H_3_RT");
        add("HorizontalBlur_3", "__anchor_pelican_ui", explicit_after);

        for (int index = 0; index != 3; ++index) {
            const std::string vertical =
                "VerticalBlur_" + std::to_string(index);
            const std::string horizontal =
                "HorizontalBlur_" + std::to_string(index + 1);
            const std::string upsample =
                "UpsampleBlend_" + std::to_string(index + 1);
            const std::string resource =
                "Bloom_Upsample_V_" + std::to_string(index) + "_RT";
            add(vertical, horizontal, explicit_after);
            add(vertical, horizontal, read_after_write, resource);
            add(vertical, upsample, read_after_write, resource);
            add(vertical, "__anchor_pelican_ui", explicit_after);
        }
        add("VerticalBlur_3", "UpsampleBlend_3", explicit_after);
        add("VerticalBlur_3", "UpsampleBlend_3", read_after_write,
            "Bloom_Upsample_V_3_RT");
        add("VerticalBlur_3", "__anchor_pelican_ui", explicit_after);

        for (int index = 3; index != 0; --index) {
            const std::string from =
                "UpsampleBlend_" + std::to_string(index);
            const std::string to =
                index == 1
                    ? std::string{"FinalBloomComposite"}
                    : "UpsampleBlend_" + std::to_string(index - 1);
            const std::string resource =
                "Bloom_Downsample_H_" + std::to_string(index - 1) +
                "_RT";
            add(from, to, explicit_after);
            add(from, to, read_after_write, resource);
            add(from, "__anchor_pelican_ui", explicit_after);
        }
        add("FinalBloomComposite", "__anchor_pelican_ui", explicit_after);
        add("FinalBloomComposite", "pelican_ui", read_after_write,
            "display");

        if (result.size() != 100) {
            throw std::logic_error(
                "independent WP316 dependency expectation must contain 100 records");
        }
        return result;
    }();
    return expected;
}

std::string expectedRecordIdentity(const ExpectedDependency &dependency) {
    constexpr char separator = '\x1f';
    return dependency.from + separator + dependency.to + separator +
           dependency.reason + separator + dependency.resource;
}

QStringList expectedStrings(const std::vector<std::string> &values) {
    QStringList result;
    for (const auto &value : values) {
        result.push_back(QString::fromStdString(value));
    }
    return result;
}

using ExpectedBundles =
    std::map<EdgePair, std::vector<ExpectedDependency>, std::less<>>;

ExpectedBundles expectedExpandedBundles() {
    auto dependencies = expectedExpandedDependencies();
    std::ranges::sort(
        dependencies, {}, [](const ExpectedDependency &dependency) {
            return std::tie(dependency.from, dependency.to,
                            dependency.reason, dependency.resource);
        });
    ExpectedBundles result;
    for (const auto &dependency : dependencies) {
        result[{dependency.from, dependency.to}].push_back(dependency);
    }
    return result;
}

QString expectedDependencyLabel(
    const std::vector<ExpectedDependency> &dependencies) {
    std::set<std::string, std::less<>> resources;
    std::set<std::string, std::less<>> reasons;
    for (const auto &dependency : dependencies) {
        if (!dependency.resource.empty()) {
            resources.insert(dependency.resource);
        }
        reasons.insert(dependency.reason);
    }
    QStringList parts;
    for (const auto &resource : resources) {
        parts.push_back(QString::fromStdString(resource));
    }
    if (parts.empty()) {
        for (const auto &reason : reasons) {
            const auto marker = reason.rfind('.');
            const auto version = reason.rfind('@');
            parts.push_back(QString::fromStdString(reason.substr(
                marker == std::string::npos ? 0 : marker + 1,
                version == std::string::npos
                    ? std::string::npos
                    : version - (marker == std::string::npos ? 0
                                                              : marker + 1))));
        }
    }
    QString result = parts.join(QStringLiteral(", "));
    if (dependencies.size() > 1) {
        result += QStringLiteral("  x%1").arg(dependencies.size());
    }
    return result;
}

std::vector<QGraphicsItem *> itemsWithEndpoints(QGraphicsScene &value,
                                                 const char *item_kind,
                                                 std::string_view from,
                                                 std::string_view to) {
    std::vector<QGraphicsItem *> result;
    const QString expected_from = QString::fromUtf8(
        from.data(), static_cast<qsizetype>(from.size()));
    const QString expected_to =
        QString::fromUtf8(to.data(), static_cast<qsizetype>(to.size()));
    for (QGraphicsItem *item : itemsOfKind(value, item_kind)) {
        if (item->data(FramePlanFromNameRole).toString() == expected_from &&
            item->data(FramePlanToNameRole).toString() == expected_to) {
            result.push_back(item);
        }
    }
    return result;
}

QGraphicsItem *singleItemWithEndpoints(QGraphicsScene &value,
                                       const char *item_kind,
                                       std::string_view from,
                                       std::string_view to) {
    const auto matches = itemsWithEndpoints(value, item_kind, from, to);
    REQUIRE(matches.size() == 1);
    return matches.front();
}

QGraphicsItem *nodeLabelItem(QGraphicsScene &value, std::string_view name) {
    const QString expected = QString::fromUtf8(
        name.data(), static_cast<qsizetype>(name.size()));
    std::vector<QGraphicsItem *> matches;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanNodeLabelItem)) {
        if (item->data(FramePlanNameRole).toString() == expected) {
            matches.push_back(item);
        }
    }
    REQUIRE(matches.size() == 1);
    return matches.front();
}

QColor expectedNodeColor(std::string_view source) {
    if (source == "project") {
        return QColor{QStringLiteral("#3978a8")};
    }
    if (source.starts_with("feature:")) {
        return QColor{QStringLiteral("#c47b25")};
    }
    return QColor{QStringLiteral("#66717e")};
}

QPainterPath expectedNodePath(bool anchor) {
    QPainterPath path;
    if (anchor) {
        QPolygonF diamond;
        diamond << QPointF{18.0, 16.0} << QPointF{34.0, 32.0}
                << QPointF{18.0, 48.0} << QPointF{2.0, 32.0};
        path.addPolygon(diamond);
        path.closeSubpath();
    } else {
        path.addRoundedRect(QRectF{0.0, 0.0, 240.0, 64.0}, 9.0, 9.0);
    }
    return path;
}

std::map<std::string, QPointF, std::less<>> expandedNodePositions() {
    std::map<std::string, QPointF, std::less<>> result;
    qreal x = 0.0;
    for (const auto &node : expectedExpandedNodes()) {
        result.emplace(node.name, QPointF{x, 0.0});
        x += 320.0;
    }
    return result;
}

std::map<std::string, QPointF, std::less<>>
sceneNodePositions(QGraphicsScene &value) {
    std::map<std::string, QPointF, std::less<>> result;
    for (QGraphicsItem *item : itemsOfKind(value, FramePlanNodeItem)) {
        result.emplace(item->data(FramePlanNameRole).toString().toStdString(),
                       item->scenePos());
    }
    return result;
}

void requireExpectedExpandedScene(QGraphicsScene &value) {
    constexpr auto graph = "main_render";
    const auto &expected_nodes = expectedExpandedNodes();
    const auto expected_positions = expandedNodePositions();
    const auto expected_bundles = expectedExpandedBundles();

    REQUIRE(itemsOfKind(value, FramePlanNodeItem).size() ==
            expected_nodes.size());
    REQUIRE(itemsOfKind(value, FramePlanNodeLabelItem).size() ==
            expected_nodes.size());
    for (const auto &expected : expected_nodes) {
        QGraphicsItem *item = nodeItem(value, expected.name);
        REQUIRE(item != nullptr);
        auto *path_item = dynamic_cast<QGraphicsPathItem *>(item);
        REQUIRE(path_item != nullptr);
        REQUIRE(item->isVisible());
        REQUIRE(item->scenePos() == expected_positions.at(expected.name));
        REQUIRE(item->data(FramePlanGraphRole).toString() ==
                QLatin1String{graph});
        REQUIRE(item->data(FramePlanNameRole).toString() ==
                QString::fromStdString(expected.name));
        REQUIRE(item->data(FramePlanSourceRole).toString() ==
                QString::fromStdString(expected.source));
        const QColor fill = expectedNodeColor(expected.source);
        REQUIRE(item->data(FramePlanColorRole).toString() ==
                fill.name(QColor::HexRgb));
        REQUIRE(item->data(FramePlanAnchorRole).toBool() == expected.anchor);
        REQUIRE(item->data(FramePlanMembersRole).toStringList() ==
                QStringList{QString::fromStdString(expected.name)});
        REQUIRE(item->data(FramePlanInternalEdgeRecordsRole)
                    .toStringList()
                    .empty());
        REQUIRE(path_item->path() == expectedNodePath(expected.anchor));
        REQUIRE(path_item->brush() == QBrush{fill});
        QPen expected_pen{expected.anchor
                              ? QColor{QStringLiteral("#d8e3ec")}
                              : QColor{QStringLiteral("#edf2f6")}};
        expected_pen.setWidthF(1.3);
        if (expected.anchor) {
            expected_pen.setStyle(Qt::DashLine);
        }
        REQUIRE(path_item->pen() == expected_pen);

        QGraphicsItem *label_item = nodeLabelItem(value, expected.name);
        auto *label =
            dynamic_cast<QGraphicsSimpleTextItem *>(label_item);
        REQUIRE(label != nullptr);
        REQUIRE(label->parentItem() == item);
        REQUIRE(label->isVisible());
        REQUIRE(label->text() == QString::fromStdString(expected.name));
        REQUIRE(label->brush() ==
                QBrush{QColor{QStringLiteral("#f7f9fb")}});
        REQUIRE(label->font().bold());
        REQUIRE(label->data(FramePlanGraphRole).toString() ==
                QLatin1String{graph});
        REQUIRE(label->data(FramePlanNameRole).toString() ==
                QString::fromStdString(expected.name));
    }

    REQUIRE(itemsOfKind(value, FramePlanEdgeItem).size() ==
            expected_bundles.size());
    REQUIRE(itemsOfKind(value, FramePlanEdgeArrowItem).size() ==
            expected_bundles.size());
    REQUIRE(itemsOfKind(value, FramePlanEdgeLabelItem).size() ==
            expected_bundles.size());
    const QPen edge_pen = [] {
        QPen result{QColor{QStringLiteral("#748394")}};
        result.setWidthF(1.35);
        return result;
    }();
    const QPen arrow_pen{QColor{QStringLiteral("#748394")}};
    const QBrush arrow_brush{QColor{QStringLiteral("#748394")}};
    const QPen label_pen{QColor{QStringLiteral("#9aa7b4")}};
    const QBrush label_brush{QColor{QStringLiteral("#f5f7f9")}};
    std::size_t lane = 0;
    StringSet expected_records;
    for (const auto &[endpoints, dependencies] : expected_bundles) {
        const auto &[from, to] = endpoints;
        INFO("expected logical edge: " << from << " -> " << to);
        QGraphicsItem *item = singleItemWithEndpoints(
            value, FramePlanEdgeItem, from, to);
        auto *path_item = dynamic_cast<QGraphicsPathItem *>(item);
        REQUIRE(path_item != nullptr);
        REQUIRE(item->isVisible());
        const QString identity = QString::fromStdString(from + "->" + to);
        REQUIRE(item->data(FramePlanGraphRole).toString() ==
                QLatin1String{graph});
        REQUIRE(item->data(FramePlanNameRole).toString() == identity);
        REQUIRE(item->data(FramePlanFromNameRole).toString() ==
                QString::fromStdString(from));
        REQUIRE(item->data(FramePlanToNameRole).toString() ==
                QString::fromStdString(to));

        std::vector<std::string> identities;
        std::set<std::string, std::less<>> resources;
        for (const auto &dependency : dependencies) {
            identities.push_back(expectedRecordIdentity(dependency));
            expected_records.insert(identities.back());
            if (!dependency.resource.empty()) {
                resources.insert(dependency.resource);
            }
        }
        const std::vector<std::string> resource_values{resources.begin(),
                                                       resources.end()};
        REQUIRE(item->data(FramePlanEdgeRecordsRole).toStringList() ==
                expectedStrings(identities));
        REQUIRE(item->data(FramePlanResourcesRole).toStringList() ==
                expectedStrings(resource_values));

        const QPointF from_position = expected_positions.at(from);
        const QPointF to_position = expected_positions.at(to);
        const QPointF start{from_position.x() + 240.0, 32.0};
        const QPointF tip{to_position.x(), 32.0};
        const qreal left_turn = start.x() + 18.0;
        const qreal right_turn = tip.x() - 18.0;
        const qreal lane_y = 134.0 + static_cast<qreal>(lane) * 34.0;
        QPainterPath expected_path{start};
        expected_path.lineTo(left_turn, start.y());
        expected_path.lineTo(left_turn, lane_y);
        expected_path.lineTo(right_turn, lane_y);
        expected_path.lineTo(right_turn, tip.y());
        expected_path.lineTo(tip);
        REQUIRE(path_item->path() == expected_path);
        REQUIRE(path_item->pen() == edge_pen);
        REQUIRE(path_item->brush().style() == Qt::NoBrush);

        QGraphicsItem *arrow_item = singleItemWithEndpoints(
            value, FramePlanEdgeArrowItem, from, to);
        auto *arrow = dynamic_cast<QGraphicsPolygonItem *>(arrow_item);
        REQUIRE(arrow != nullptr);
        REQUIRE(arrow->isVisible());
        QPolygonF expected_arrow;
        expected_arrow << tip << QPointF{tip.x() - 10.0, tip.y() - 5.0}
                       << QPointF{tip.x() - 10.0, tip.y() + 5.0};
        REQUIRE(arrow->polygon() == expected_arrow);
        REQUIRE(arrow->pen() == arrow_pen);
        REQUIRE(arrow->brush() == arrow_brush);
        REQUIRE(arrow->data(FramePlanGraphRole).toString() ==
                QLatin1String{graph});
        REQUIRE(arrow->data(FramePlanNameRole).toString() == identity);
        REQUIRE(arrow->data(FramePlanFromNameRole).toString() ==
                QString::fromStdString(from));
        REQUIRE(arrow->data(FramePlanToNameRole).toString() ==
                QString::fromStdString(to));

        QGraphicsItem *label_item = singleItemWithEndpoints(
            value, FramePlanEdgeLabelItem, from, to);
        auto *label_box = dynamic_cast<QGraphicsRectItem *>(label_item);
        REQUIRE(label_box != nullptr);
        REQUIRE(label_box->isVisible());
        REQUIRE(label_box->rect().topLeft() == QPointF{});
        REQUIRE(label_box->rect().height() == 24.0);
        REQUIRE(label_box->scenePos().y() == lane_y - 12.0);
        REQUIRE(label_box->scenePos().x() +
                    label_box->rect().width() / 2.0 ==
                (left_turn + right_turn) / 2.0);
        REQUIRE(label_box->pen() == label_pen);
        REQUIRE(label_box->brush() == label_brush);
        REQUIRE(label_box->data(FramePlanGraphRole).toString() ==
                QLatin1String{graph});
        REQUIRE(label_box->data(FramePlanNameRole).toString() == identity);
        REQUIRE(label_box->data(FramePlanFromNameRole).toString() ==
                QString::fromStdString(from));
        REQUIRE(label_box->data(FramePlanToNameRole).toString() ==
                QString::fromStdString(to));
        REQUIRE(label_box->data(FramePlanEdgeRecordsRole).toStringList() ==
                expectedStrings(identities));
        REQUIRE(label_box->data(FramePlanResourcesRole).toStringList() ==
                expectedStrings(resource_values));
        REQUIRE(label_box->childItems().size() == 1);
        auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(
            label_box->childItems().front());
        REQUIRE(label != nullptr);
        REQUIRE(label->isVisible());
        REQUIRE(label->text() == expectedDependencyLabel(dependencies));
        REQUIRE(label->brush() ==
                QBrush{QColor{QStringLiteral("#263441")}});
        ++lane;
    }
    REQUIRE(sceneDependencyRecords(value) == expected_records);
}

void requireReadableLabels(QGraphicsScene &value) {
    // Compare reserved layout regions, not glyph bounds. Glyph metrics vary
    // between native Windows and the offscreen QPA even for the same family.
    std::vector<QRectF> reserved_regions;
    for (QGraphicsItem *item : value.items()) {
        const QString item_kind = kind(*item);
        if (item_kind == QLatin1String{FramePlanNodeLabelItem} ||
            item_kind == QLatin1String{FramePlanGroupLabelItem}) {
            auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(item);
            REQUIRE(label != nullptr);
            REQUIRE_FALSE(label->text().isEmpty());
            REQUIRE(label->parentItem() != nullptr);
            const bool group =
                item_kind == QLatin1String{FramePlanGroupLabelItem};
            reserved_regions.emplace_back(
                label->parentItem()->scenePos(),
                QSizeF{group ? 260.0 : 240.0, group ? 76.0 : 64.0});
        } else if (item_kind == QLatin1String{FramePlanEdgeLabelItem}) {
            auto *label_box = dynamic_cast<QGraphicsRectItem *>(item);
            REQUIRE(label_box != nullptr);
            REQUIRE(label_box->childItems().size() == 1);
            auto *label = dynamic_cast<QGraphicsSimpleTextItem *>(
                label_box->childItems().front());
            REQUIRE(label != nullptr);
            REQUIRE_FALSE(label->text().isEmpty());
            // The production box width follows QFontMetricsF.  Use a fixed,
            // conservative per-code-unit envelope around its lane center so
            // QPA font metrics cannot change the overlap conclusion.
            const QPointF center =
                label_box->mapToScene(label_box->rect().center());
            const qreal fixed_width = std::max<qreal>(
                56.0, 14.0 + 12.0 * static_cast<qreal>(label->text().size()));
            QRectF fixed_region{0.0, 0.0, fixed_width, 24.0};
            fixed_region.moveCenter(center);
            reserved_regions.push_back(fixed_region);
        }
    }
    REQUIRE_FALSE(reserved_regions.empty());
    const qreal minimum =
        value.property("pelicanMinimumLabelSpacing").toReal();
    REQUIRE(minimum >= 8.0);
    for (std::size_t left = 0; left < reserved_regions.size(); ++left) {
        for (std::size_t right = left + 1;
             right < reserved_regions.size(); ++right) {
            const QRectF padded = reserved_regions[left].adjusted(
                -minimum, -minimum, minimum, minimum);
            REQUIRE_FALSE(padded.intersects(reserved_regions[right]));
        }
    }
}

void requireNoReference(QGraphicsScene &value, std::string_view node,
                        std::string_view resource) {
    const QString node_text = QString::fromStdString(std::string{node});
    const QString resource_text = QString::fromStdString(std::string{resource});
    for (QGraphicsItem *item : value.items()) {
        REQUIRE(item->data(FramePlanNameRole).toString() != node_text);
        REQUIRE(item->data(FramePlanFromNameRole).toString() != node_text);
        REQUIRE(item->data(FramePlanToNameRole).toString() != node_text);
        REQUIRE_FALSE(item->data(FramePlanMembersRole)
                          .toStringList()
                          .contains(node_text));
        REQUIRE_FALSE(item->data(FramePlanResourcesRole)
                          .toStringList()
                          .contains(resource_text));
        for (const QString &record :
             item->data(FramePlanEdgeRecordsRole).toStringList()) {
            REQUIRE_FALSE(record.contains(node_text));
            REQUIRE_FALSE(record.contains(resource_text));
        }
        for (const QString &record :
             item->data(FramePlanInternalEdgeRecordsRole).toStringList()) {
            REQUIRE_FALSE(record.contains(node_text));
            REQUIRE_FALSE(record.contains(resource_text));
        }
    }
}

} // namespace

TEST_CASE(
    "WP341 feature group collapses to a convex quotient and supports both scoped exits",
    "[devstudio][frame-plan][grouping][wp341][negative-contrast]") {
    (void)application();
    const FramePlanModel model = convexGroupingModel();
    const FramePlanNodeKey target{model.graph, "focus"};
    const StringSet members{"capture_a", "capture_b", "capture_c"};

    FramePlanGraphicsScene logical;
    logical.populate(model, target, 1);

    // Required negative control: the exact same scene starts expanded.
    const StringSet expanded_node_names = sceneNodeNames(logical);
    const EdgeSet expanded_edges = sceneEdges(logical);
    const StringSet expanded_records = sceneDependencyRecords(logical);
    const auto expanded_positions = sceneNodePositions(logical);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanGroupLabelItem).empty());
    REQUIRE(std::ranges::count_if(
                members,
                [&](const std::string &member) {
                    return nodeItem(logical, member) != nullptr;
                }) == members.size());
    QGraphicsItem *collapse_member =
        nodeItem(logical, "capture_a");
    REQUIRE(collapse_member != nullptr);
    const QString group_id =
        collapse_member->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(group_id.isEmpty());
    REQUIRE(collapse_member
                ->data(FramePlanGroupCollapsibleRole)
                .toBool());

    // Exercise the user-facing context-menu entry, not only the state method.
    triggerContextAction(logical, *collapse_member,
                         QStringLiteral("Collapse"));

    // Positive half of the same negative-control test: every member item is
    // gone and exactly one group item has replaced all of them.
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).size() == 1);
    REQUIRE(itemsOfKind(logical, FramePlanGroupLabelItem).size() == 1);
    for (const auto &member : members) {
        REQUIRE(nodeItem(logical, member) == nullptr);
    }
    QGraphicsItem *group = singleGroupItem(logical);
    REQUIRE(group->data(FramePlanGroupIdRole).toString() == group_id);
    REQUIRE(strings(group->data(FramePlanMembersRole)) == members);
    REQUIRE(logical.property("pelicanCollapsedGroups").toStringList() ==
            QStringList{group_id});

    const std::string group_name =
        group->data(FramePlanNameRole).toString().toStdString();
    REQUIRE(group_name != "__pelican_group__:cube_capture");
    REQUIRE(expanded_node_names.contains(
        "__pelican_group__:cube_capture"));
    REQUIRE_FALSE(expanded_node_names.contains(group_name));

    qreal minimum_member_column = std::numeric_limits<qreal>::max();
    for (const auto &member : members) {
        minimum_member_column =
            std::min(minimum_member_column,
                     expanded_positions.at(member).x());
    }
    REQUIRE(group->scenePos().x() == minimum_member_column);

    const auto quotient_name = [&](const std::string &name) {
        return members.contains(name) ? group_name : name;
    };
    EdgeSet expected_quotient_edges;
    for (const auto &[from, to] : expanded_edges) {
        const std::string mapped_from = quotient_name(from);
        const std::string mapped_to = quotient_name(to);
        if (mapped_from != mapped_to) {
            expected_quotient_edges.emplace(mapped_from, mapped_to);
        }
    }
    REQUIRE(sceneEdges(logical) == expected_quotient_edges);

    const EdgeSet expanded_reachability =
        transitiveReachability(expanded_node_names, expanded_edges);
    EdgeSet expected_quotient_reachability;
    for (const auto &[from, to] : expanded_reachability) {
        const std::string mapped_from = quotient_name(from);
        const std::string mapped_to = quotient_name(to);
        if (mapped_from != mapped_to) {
            expected_quotient_reachability.emplace(mapped_from,
                                                   mapped_to);
        }
    }
    StringSet quotient_nodes;
    for (const auto &name : expanded_node_names) {
        quotient_nodes.insert(quotient_name(name));
    }
    REQUIRE(transitiveReachability(quotient_nodes, sceneEdges(logical)) ==
            expected_quotient_reachability);

    // The two internal records are retained on the group but create no edge.
    REQUIRE(group->data(FramePlanInternalEdgeRecordsRole)
                .toStringList()
                .size() == 2);
    REQUIRE(sceneDependencyRecords(logical) == expanded_records);
    for (QGraphicsItem *edge :
         itemsOfKind(logical, FramePlanEdgeItem)) {
        REQUIRE_FALSE(members.contains(
            edge->data(FramePlanFromNameRole).toString().toStdString()));
        REQUIRE_FALSE(members.contains(
            edge->data(FramePlanToNameRole).toString().toStdString()));
    }

    const auto incoming =
        itemsWithEndpoints(logical, FramePlanEdgeItem, "upstream",
                           group_name);
    REQUIRE(incoming.size() == 1);
    REQUIRE(incoming.front()
                ->data(FramePlanEdgeRecordsRole)
                .toStringList()
                .size() == 2);
    REQUIRE(edgeLabelText(logical, "upstream", group_name)
                .contains(QStringLiteral("x2")));
    const auto outgoing =
        itemsWithEndpoints(logical, FramePlanEdgeItem, group_name,
                           "downstream");
    REQUIRE(outgoing.size() == 1);
    REQUIRE(outgoing.front()
                ->data(FramePlanEdgeRecordsRole)
                .toStringList()
                .size() == 2);
    REQUIRE(edgeLabelText(logical, group_name, "downstream")
                .contains(QStringLiteral("x2")));

    const auto collapsed_outer_items = annotatedSceneItems(logical);

    // Double-click enters the group. Only member nodes remain; the two
    // distinct actual external endpoints become two boundary stubs.
    doubleClickSceneItem(logical, *group);
    REQUIRE(sceneNodeNames(logical) == members);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(boundaryTargets(logical) ==
            StringSet{"downstream", "upstream"});
    REQUIRE(itemsOfKind(logical, FramePlanBoundaryStubItem).size() == 2);
    REQUIRE(logical.property("pelicanBoundaryStubCount").toULongLong() ==
            2);
    REQUIRE(logical.property("pelicanCurrentGroupScope").toString() ==
            group_id);
    REQUIRE(itemsOfKind(logical, FramePlanBreadcrumbItem).size() == 1);

    // The breadcrumb is a real clickable scene control.
    QGraphicsView view{&logical};
    view.resize(1000, 700);
    view.show();
    QGraphicsItem *breadcrumb =
        itemsOfKind(logical, FramePlanBreadcrumbItem).front();
    view.centerOn(breadcrumb);
    QApplication::processEvents();
    QTest::mouseClick(
        view.viewport(), Qt::LeftButton, Qt::NoModifier,
        view.mapFromScene(breadcrumb->sceneBoundingRect().center()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanCurrentGroupScope")
                .toString()
                .isEmpty());
    REQUIRE(annotatedSceneItems(logical) == collapsed_outer_items);

    // Enter again, then exercise the independent Escape exit.
    group = singleGroupItem(logical);
    doubleClickSceneItem(logical, *group);
    REQUIRE(sceneNodeNames(logical) == members);
    view.viewport()->setFocus();
    QTest::keyClick(view.viewport(), Qt::Key_Escape);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanCurrentGroupScope")
                .toString()
                .isEmpty());
    REQUIRE(annotatedSceneItems(logical) == collapsed_outer_items);

    // The context-menu expand operation restores the exact expanded graph.
    group = singleGroupItem(logical);
    triggerContextAction(logical, *group,
                         QStringLiteral("Expand"));
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(sceneNodeNames(logical) == expanded_node_names);
    REQUIRE(sceneEdges(logical) == expanded_edges);
    REQUIRE(logical.property("pelicanCollapsedGroups")
                .toStringList()
                .isEmpty());
}

TEST_CASE(
    "WP341 collapsed state survives updates, drops orphans, and reset clears dragged positions",
    "[devstudio][frame-plan][grouping][state][drag][wp341]") {
    (void)application();
    const FramePlanModel model = convexGroupingModel();
    const FramePlanNodeKey target{model.graph, "focus"};

    FramePlanGraphicsScene logical;
    logical.populate(model, target, 1);
    QGraphicsItem *member = nodeItem(logical, "capture_a");
    REQUIRE(member != nullptr);
    const QString group_id =
        member->data(FramePlanGroupIdRole).toString();
    REQUIRE(logical.collapseGroup(group_id));
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).size() == 1);

    logical.populate(model, target, 1);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).size() == 1);
    REQUIRE(logical.property("pelicanCollapsedGroups").toStringList() ==
            QStringList{group_id});

    FramePlanModel without_group = model;
    for (auto &node : without_group.nodes) {
        if (node.provider_feature == "cube_capture") {
            node.provider_feature.clear();
            node.source = "project";
        }
    }
    logical.populate(without_group, target, 1);
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(logical.property("pelicanCollapsedGroups")
                .toStringList()
                .isEmpty());
    REQUIRE(nodeItem(logical, "capture_a") != nullptr);
    REQUIRE(nodeItem(logical, "capture_b") != nullptr);
    REQUIRE(nodeItem(logical, "capture_c") != nullptr);

    FramePlanGraphicsScene positions;
    positions.populate(model, target, 1);
    QGraphicsItem *upstream = nodeItem(positions, "upstream");
    REQUIRE(upstream != nullptr);
    const QPointF default_position = upstream->scenePos();
    upstream->setPos(default_position + QPointF{177.0, 93.0});
    REQUIRE(upstream->scenePos() != default_position);
    positions.resetGraph();
    positions.populate(model, target, 1);
    upstream = nodeItem(positions, "upstream");
    REQUIRE(upstream != nullptr);
    REQUIRE(upstream->scenePos() == default_position);
}

TEST_CASE(
    "WP341 non-convex provider feature stays expanded with an explicit UI reason",
    "[devstudio][frame-plan][grouping][convexity][wp341][negative-contrast]") {
    (void)application();
    const FramePlanModel model = nonConvexGroupingModel();
    const FramePlanNodeKey target{model.graph, "focus"};

    FramePlanGraphicsScene logical;
    logical.populate(model, target, 1);
    QGraphicsItem *inside_a = nodeItem(logical, "inside_a");
    QGraphicsItem *inside_b = nodeItem(logical, "inside_b");
    REQUIRE(inside_a != nullptr);
    REQUIRE(inside_b != nullptr);
    const QString group_id =
        inside_a->data(FramePlanGroupIdRole).toString();
    REQUIRE_FALSE(group_id.isEmpty());
    REQUIRE(inside_b->data(FramePlanGroupIdRole).toString() ==
            group_id);
    REQUIRE_FALSE(
        inside_a->data(FramePlanGroupCollapsibleRole).toBool());

    // Same source is not a grouping fallback, and an N=1 feature is not a
    // collapsible group.
    REQUIRE(nodeItem(logical, "outside")
                ->data(FramePlanGroupIdRole)
                .toString()
                .isEmpty());
    REQUIRE(nodeItem(logical, "project_peer")
                ->data(FramePlanGroupIdRole)
                .toString()
                .isEmpty());
    REQUIRE(nodeItem(logical, "single_feature_node")
                ->data(FramePlanGroupIdRole)
                .toString()
                .isEmpty());

    REQUIRE_FALSE(logical.collapseGroup(group_id));
    REQUIRE(itemsOfKind(logical, FramePlanGroupItem).empty());
    REQUIRE(nodeItem(logical, "inside_a") != nullptr);
    REQUIRE(nodeItem(logical, "inside_b") != nullptr);
    REQUIRE(logical.property("pelicanCollapsedGroups")
                .toStringList()
                .isEmpty());

    const auto warnings =
        itemsOfKind(logical, FramePlanGroupWarningItem);
    REQUIRE(warnings.size() == 1);
    const QString reason =
        warnings.front()->data(FramePlanReasonRole).toString();
    REQUIRE(reason.contains(QStringLiteral("not convex")));
    REQUIRE(reason.contains(QStringLiteral("outside")));
    REQUIRE(warnings.front()->isVisible());
    REQUIRE(warnings.front()->toolTip() == reason);
    REQUIRE(logical.property("pelicanGroupFeedback").toString() ==
            reason);
    REQUIRE(logical.property("pelicanNonCollapsibleGroups")
                .toStringList()
                .contains(group_id));
    bool visible_reason = false;
    for (QGraphicsItem *child : warnings.front()->childItems()) {
        if (const auto *text =
                dynamic_cast<QGraphicsSimpleTextItem *>(child)) {
            visible_reason = text->text() == reason;
        }
    }
    REQUIRE(visible_reason);
}

TEST_CASE(
    "WP341 removed layout constants and no longer writes empty internal-edge roles",
    "[devstudio][frame-plan][grouping][source-audit][wp341]") {
    const std::string source = readText(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "src" /
        "devstudio" / "view" / "frameplangraphics.cpp");
    REQUIRE(source.find("ColumnsPerRow") == std::string::npos);
    REQUIRE(source.find("WrapRowHeight") == std::string::npos);
    REQUIRE(source.find(
                "FramePlanInternalEdgeRecordsRole, QStringList{}") ==
            std::string::npos);
}

TEST_CASE(
    "Studio consumes producer resolved fractional extents and a non-display output source",
    "[devstudio][frame-plan][wp315][resolution][negative-contrast]") {
    const Json wire = fractionalScaleFramePlan();
    const auto runtime =
        Pelican::frameRuntimeResolutionWireFromJson(
            wire.at("runtime_resolution"));
    REQUIRE(runtime.render_source_resource == "scaled_scene");
    REQUIRE(runtime.output_source_resource == "custom_output");
    REQUIRE(runtime.output_source_resource != "display");
    REQUIRE((runtime.render_extent ==
             Pelican::ResolvedResourceExtent{7, 7}));
    REQUIRE((runtime.output_extent ==
             Pelican::ResolvedResourceExtent{10, 10}));

    const FramePlanModel model =
        buildFramePlanModel(wire.dump());
    REQUIRE(model.physical_plan.available());
    REQUIRE(model.physical_plan.output_width ==
            runtime.output_extent.width);
    REQUIRE(model.physical_plan.output_height ==
            runtime.output_extent.height);
    const auto scaled = std::ranges::find(
        model.resources, std::string{"scaled_scene"},
        &FramePlanResource::name);
    REQUIRE(scaled != model.resources.end());
    REQUIRE(scaled->width == runtime.render_extent.width);
    REQUIRE(scaled->height == runtime.render_extent.height);

    Json without_runtime_resolution = wire;
    without_runtime_resolution.erase("runtime_resolution");
    const FramePlanModel unavailable =
        buildFramePlanModel(without_runtime_resolution.dump());
    REQUIRE_FALSE(unavailable.physical_plan.available());
    REQUIRE(unavailable.physical_plan.unavailable_reason_code ==
            "physical_plan_runtime_resolution_missing");
    const auto unresolved = std::ranges::find(
        unavailable.resources, std::string{"scaled_scene"},
        &FramePlanResource::name);
    REQUIRE(unresolved != unavailable.resources.end());
    REQUIRE_FALSE(unresolved->width.has_value());
    REQUIRE_FALSE(unresolved->height.has_value());
}

TEST_CASE(
    "WP317 missing execution plan is a named logical error rather than a valid zero-dependency graph",
    "[devstudio][frame-plan][logical-graph][wp317][negative-contrast]") {
    (void)application();
    const Json zero_dependencies = independentOpportunityFramePlan(
        Pelican::PlanningProfileKind::conservative_debug);
    REQUIRE(zero_dependencies.at("execution_plan")
                .at("dependencies")
                .empty());
    REQUIRE_FALSE(zero_dependencies.at("nodes").empty());

    Json missing = zero_dependencies;
    missing.erase("execution_plan");
    REQUIRE(missing.at("nodes") == zero_dependencies.at("nodes"));
    REQUIRE(missing.at("barriers") == zero_dependencies.at("barriers"));

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(missing.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);

    REQUIRE(logical.property("pelicanExecutionPlanState").toString() ==
            QStringLiteral("unavailable"));
    REQUIRE(logical.property("pelicanExecutionPlanReasonCode").toString() ==
            QStringLiteral("execution_plan_missing"));
    REQUIRE(logical.property("pelicanDependencyRecordCount").toULongLong() ==
            0);
    REQUIRE(itemsOfKind(logical, FramePlanNodeItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanEdgeItem).empty());
    const auto unavailable =
        itemsOfKind(logical, FramePlanLogicalUnavailableItem);
    REQUIRE(unavailable.size() == 1);
    REQUIRE(unavailable.front()
                ->data(FramePlanLogicalStateRole)
                .toString() == QStringLiteral("unavailable"));
    REQUIRE(unavailable.front()
                ->data(FramePlanReasonCodeRole)
                .toString() == QStringLiteral("execution_plan_missing"));
    REQUIRE(unavailable.front()->toolTip().contains(
        QStringLiteral("execution_plan was not published")));
    auto *unavailable_panel =
        dynamic_cast<QGraphicsRectItem *>(unavailable.front());
    REQUIRE(unavailable_panel != nullptr);
    const QRectF reserved_error_region =
        unavailable_panel->sceneBoundingRect();
    REQUIRE(reserved_error_region.width() >= 600.0);
    REQUIRE(reserved_error_region.height() >= 120.0);
    REQUIRE(logical.sceneRect().contains(reserved_error_region));

    // Same production entry and widget; the only difference is the valid,
    // compiler-published execution plan whose dependency array is truly empty.
    widget.receiveResult(
        QByteArray::fromStdString(zero_dependencies.dump()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanExecutionPlanState").toString() ==
            QStringLiteral("available"));
    REQUIRE(logical.property("pelicanExecutionPlanReasonCode")
                .toString()
                .isEmpty());
    REQUIRE(logical.property("pelicanDependencyRecordCount").toULongLong() ==
            0);
    REQUIRE(itemsOfKind(logical, FramePlanLogicalUnavailableItem).empty());
    const auto valid_nodes = itemsOfKind(logical, FramePlanNodeItem);
    REQUIRE(valid_nodes.size() == 1);
    REQUIRE(logical.property("pelicanSubtreeDepth").toInt() == 1);
    REQUIRE(logical.property("pelicanVisibleItemCount").toULongLong() == 1);
    REQUIRE(itemsOfKind(logical, FramePlanEdgeItem).empty());
    for (const QGraphicsItem *node : valid_nodes) {
        REQUIRE(logical.sceneRect().contains(node->sceneBoundingRect()));
    }
}

TEST_CASE(
    "WP329 logical node selection shows model details and clearing selection removes them",
    "[devstudio][frame-plan][logical-graph][selection][wp329][negative-contrast]") {
    (void)application();
    const std::string captured = readText(PELICAN_TEST_FRAME_PLAN_FIXTURE);
    const FramePlanModel model = buildFramePlanModel(captured);
    REQUIRE_FALSE(model.nodes.empty());

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(captured));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    QTreeWidget &details = logicalDetails(widget);

    const auto visible_nodes = itemsOfKind(logical, FramePlanNodeItem);
    REQUIRE_FALSE(visible_nodes.empty());
    QGraphicsItem *selected_item = visible_nodes.front();
    const std::string selected_name =
        selected_item->data(FramePlanNameRole).toString().toStdString();
    const auto expected = std::ranges::find(
        model.nodes, selected_name, &FramePlanNode::name);
    REQUIRE(expected != model.nodes.end());

    selected_item->setSelected(true);
    QApplication::processEvents();
    REQUIRE(details.topLevelItemCount() == 1);
    QTreeWidgetItem *selected_details = details.topLevelItem(0);
    const QStringList selected_observation{
        selected_details->text(1), selected_details->text(2),
        selected_details->text(3), selected_details->text(4)};
    const QStringList expected_observation{
        QString::fromStdString(expected->name),
        QString::fromStdString(expected->kind), joinedValues(expected->reads),
        joinedValues(expected->writes)};
    REQUIRE(selected_observation == expected_observation);

    logical.clearSelection();
    QApplication::processEvents();
    REQUIRE(details.topLevelItemCount() == 1);
    QTreeWidgetItem *no_selection = details.topLevelItem(0);
    const QStringList no_selection_observation{
        no_selection->text(1), no_selection->text(2), no_selection->text(3),
        no_selection->text(4)};
    REQUIRE(no_selection_observation != selected_observation);
    REQUIRE_FALSE(no_selection->text(1).isEmpty());
    REQUIRE(no_selection->text(1) != QString::fromStdString(expected->name));
    REQUIRE(no_selection->text(2).isEmpty());
    REQUIRE(no_selection->text(3).isEmpty());
    REQUIRE(no_selection->text(4).isEmpty());
}

TEST_CASE(
    "WP318 every example target stays within five direct nodes and named depths are exact",
    "[devstudio][frame-plan][target-subtree][wp318]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    QComboBox &selector = targetSelector(widget);
    QSpinBox &depth = subtreeDepth(widget);

    REQUIRE(selector.count() == 22);
    REQUIRE(depth.value() == 1);
    REQUIRE(logical.property("pelicanNodeRecordCount").toULongLong() == 30);

    for (int index = 0; index < selector.count(); ++index) {
        selector.setCurrentIndex(index);
        QApplication::processEvents();
        CAPTURE(selector.currentText().toStdString());
        REQUIRE(itemsOfKind(logical, FramePlanNodeItem).size() <= 5);
        REQUIRE(logical.property("pelicanVisibleItemCount").toULongLong() <=
                5);
        REQUIRE(logical.property("pelicanTarget").toString() ==
                selector.currentText());
        REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).size() ==
                1);
    }

    const std::map<std::string, StringSet, std::less<>> direct{
        {"gbuffer_albedo", {"gbuffer_pass", "lighting_pass"}},
        {"ssao_blur", {"lighting_pass", "ssao_blur_pass"}},
        {"lit_color",
         {"FinalBloomComposite", "HighLuminanceExtraction",
          "__snapshot_opaque_color", "forward_transparent",
          "lighting_pass"}},
        {"Bloom_Threshold_RT",
         {"HighLuminanceExtraction", "HorizontalBlur_0"}},
        {"display",
         {"FinalBloomComposite", "output_transform", "pelican_ui"}},
    };
    for (const auto &[target, expected] : direct) {
        selector.setCurrentText(QString::fromStdString(target));
        QApplication::processEvents();
        REQUIRE(sceneNodeNames(logical) == expected);
    }

    selector.setCurrentText(QStringLiteral("Bloom_Threshold_RT"));
    const std::array expected_by_depth{
        StringSet{},
        StringSet{"HighLuminanceExtraction", "HorizontalBlur_0"},
        StringSet{"FinalBloomComposite", "HighLuminanceExtraction",
                  "HorizontalBlur_0", "UpsampleBlend_1", "VerticalBlur_0",
                  "__snapshot_opaque_color", "forward_transparent",
                  "lighting_pass"},
    };
    for (int value = 0; value <= 2; ++value) {
        depth.setValue(value);
        QApplication::processEvents();
        CAPTURE(value);
        REQUIRE(sceneNodeNames(logical) == expected_by_depth[value]);
        REQUIRE(logical.property("pelicanSubtreeDepth").toInt() == value);
    }
    REQUIRE(expected_by_depth[2].size() == 8);
    REQUIRE(expected_by_depth[2].size() < wire.at("nodes").size());
}

TEST_CASE(
    "WP318 subtree coordinates and bundle order ignore every input array order",
    "[devstudio][frame-plan][target-subtree][determinism][wp318]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");
    FramePlanModel ordered = buildFramePlanModel(wire.dump());
    FramePlanModel reversed = ordered;
    std::reverse(reversed.nodes.begin(), reversed.nodes.end());
    std::reverse(reversed.dependencies.begin(), reversed.dependencies.end());
    std::reverse(reversed.resources.begin(), reversed.resources.end());
    std::reverse(reversed.physical_plan.alias_groups.begin(),
                 reversed.physical_plan.alias_groups.end());
    std::reverse(reversed.physical_plan.alias_candidates.begin(),
                 reversed.physical_plan.alias_candidates.end());
    for (auto &node : reversed.nodes) {
        std::reverse(node.reads.begin(), node.reads.end());
        std::reverse(node.writes.begin(), node.writes.end());
    }

    const FramePlanNodeKey target{ordered.graph, "Bloom_Threshold_RT"};
    FramePlanGraphicsScene first;
    FramePlanGraphicsScene second;
    first.populate(ordered, target, 2);
    second.populate(reversed, target, 2);
    REQUIRE(sceneNodePositions(first) == sceneNodePositions(second));
    REQUIRE(sceneBundles(first) == sceneBundles(second));
}

TEST_CASE(
    "WP318 moving a node reroutes its curve arrow and label and positions live only in one scene",
    "[devstudio][frame-plan][drag][curve][session][wp318]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");
    const QByteArray captured = QByteArray::fromStdString(wire.dump());

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(captured);
    targetSelector(widget).setCurrentText(
        QStringLiteral("Bloom_Threshold_RT"));
    subtreeDepth(widget).setValue(2);
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);

    QGraphicsItem *node = nodeItem(logical, "HorizontalBlur_0");
    auto *edge = dynamic_cast<QGraphicsPathItem *>(
        edgeItem(logical, "HighLuminanceExtraction", "HorizontalBlur_0"));
    REQUIRE(node != nullptr);
    REQUIRE(edge != nullptr);
    REQUIRE(node->flags().testFlag(QGraphicsItem::ItemIsMovable));
    REQUIRE(node->flags().testFlag(QGraphicsItem::ItemSendsGeometryChanges));
    REQUIRE(edge->data(FramePlanCurveRole).toBool());
    bool has_curve = false;
    for (int index = 0; index < edge->path().elementCount(); ++index) {
        has_curve = has_curve ||
                    edge->path().elementAt(index).type ==
                        QPainterPath::CurveToElement;
    }
    REQUIRE(has_curve);

    QGraphicsItem *arrow = nullptr;
    QGraphicsItem *label = nullptr;
    for (QGraphicsItem *item : logical.items()) {
        if (item->data(FramePlanFromNameRole).toString() !=
                QStringLiteral("HighLuminanceExtraction") ||
            item->data(FramePlanToNameRole).toString() !=
                QStringLiteral("HorizontalBlur_0")) {
            continue;
        }
        if (kind(*item) == QLatin1String{FramePlanEdgeArrowItem}) {
            arrow = item;
        } else if (kind(*item) == QLatin1String{FramePlanEdgeLabelItem}) {
            label = item;
        }
    }
    auto *arrow_polygon = dynamic_cast<QGraphicsPolygonItem *>(arrow);
    REQUIRE(arrow_polygon != nullptr);
    REQUIRE(label != nullptr);
    QGraphicsItem *node_label = nodeLabelItem(logical, "HorizontalBlur_0");
    REQUIRE(node_label != nullptr);

    const QPointF default_position = node->pos();
    const QPainterPath old_path = edge->path();
    const QPolygonF old_arrow = arrow_polygon->polygon();
    const QPointF old_node_label = node_label->scenePos();
    const QPointF moved_position = default_position + QPointF{73.0, 81.0};
    node->setPos(moved_position);
    QApplication::processEvents();
    REQUIRE(edge->path() != old_path);
    REQUIRE(arrow_polygon->polygon() != old_arrow);
    REQUIRE(QLineF{arrow_polygon->mapToScene(arrow_polygon->polygon().first()),
                   edge->mapToScene(edge->path().currentPosition())}
                .length() < 0.01);
    REQUIRE(QLineF{label->sceneBoundingRect().center(),
                   edge->mapToScene(edge->path().pointAtPercent(0.5))}
                .length() < 0.01);
    REQUIRE(node_label->scenePos() != old_node_label);

    subtreeDepth(widget).setValue(0);
    subtreeDepth(widget).setValue(2);
    QApplication::processEvents();
    REQUIRE(nodeItem(logical, "HorizontalBlur_0")->pos() ==
            moved_position);

    EmbeddedViewport fresh_viewport;
    FramePlanWidget fresh{&fresh_viewport};
    fresh.receiveResult(captured);
    targetSelector(fresh).setCurrentText(
        QStringLiteral("Bloom_Threshold_RT"));
    subtreeDepth(fresh).setValue(2);
    QApplication::processEvents();
    REQUIRE(nodeItem(scene(fresh), "HorizontalBlur_0")->pos() ==
            default_position);
    REQUIRE(nodeItem(scene(fresh), "HorizontalBlur_0")->pos() !=
            moved_position);
}

TEST_CASE(
    "WP320 dragging through the real view keeps movement and scene bounds finite",
    "[devstudio][frame-plan][drag][mouse][wp320]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.resize(520, 420);
    widget.show();
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    targetSelector(widget).setCurrentText(
        QStringLiteral("Bloom_Threshold_RT"));
    subtreeDepth(widget).setValue(2);
    QApplication::processEvents();

    QGraphicsScene &logical = scene(widget);
    QGraphicsView &view = logicalView(widget);
    const auto nodes = itemsOfKind(logical, FramePlanNodeItem);
    REQUIRE_FALSE(nodes.empty());
    QGraphicsItem *dragged = *std::ranges::max_element(
        nodes, {}, [](const QGraphicsItem *item) {
            return item->sceneBoundingRect().right();
        });
    REQUIRE(dragged != nullptr);
    REQUIRE(dragged->flags().testFlag(QGraphicsItem::ItemIsMovable));

    view.centerOn(dragged);
    QApplication::processEvents();
    QWidget *const viewport_widget = view.viewport();
    REQUIRE(viewport_widget != nullptr);
    REQUIRE(viewport_widget->isVisible());
    const QPoint press_position =
        view.mapFromScene(dragged->mapToScene(QPointF{18.0, 18.0}));
    const std::array move_offsets{
        QPoint{15, 10}, QPoint{30, 20}, QPoint{45, 30},
        QPoint{60, 40}, QPoint{75, 50}, QPoint{90, 60},
        QPoint{105, 70}, QPoint{120, 80},
    };
    REQUIRE(viewport_widget->rect().contains(press_position));
    REQUIRE(viewport_widget->rect().contains(press_position +
                                             move_offsets.back()));

    const auto finite_rect = [](const QRectF &rect) {
        return std::isfinite(rect.x()) && std::isfinite(rect.y()) &&
               std::isfinite(rect.width()) && std::isfinite(rect.height());
    };
    const QRectF before_scene_rect = logical.sceneRect();
    REQUIRE(finite_rect(before_scene_rect));
    const auto before_positions = sceneNodePositions(logical);
    const QPointF before_position = dragged->pos();
    const QPointF expected_delta =
        view.mapToScene(press_position + move_offsets.back()) -
        view.mapToScene(press_position);
    const std::string dragged_name =
        dragged->data(FramePlanNameRole).toString().toStdString();

    QTest::mousePress(viewport_widget, Qt::LeftButton, Qt::NoModifier,
                      press_position);
    for (const QPoint &offset : move_offsets) {
        QTest::mouseMove(viewport_widget, press_position + offset, 1);
    }
    QTest::mouseRelease(viewport_widget, Qt::LeftButton, Qt::NoModifier,
                        press_position + move_offsets.back());
    QApplication::processEvents();

    const QPointF actual_delta = dragged->pos() - before_position;
    REQUIRE(QLineF{actual_delta, expected_delta}.length() < 0.01);
    const auto after_positions = sceneNodePositions(logical);
    REQUIRE(after_positions.size() == before_positions.size());
    for (const auto &[name, position] : before_positions) {
        CAPTURE(name);
        if (name == dragged_name) {
            REQUIRE(after_positions.at(name) != position);
        } else {
            REQUIRE(after_positions.at(name) == position);
        }
    }

    const QRectF settled_scene_rect = logical.sceneRect();
    REQUIRE(finite_rect(settled_scene_rect));
    REQUIRE(settled_scene_rect.contains(dragged->sceneBoundingRect()));
    REQUIRE(settled_scene_rect ==
            logical.itemsBoundingRect().adjusted(-30.0, -30.0, 30.0, 30.0));
    for (int index = 0; index < 4; ++index) {
        QApplication::processEvents();
        REQUIRE(logical.sceneRect() == settled_scene_rect);
    }
}

TEST_CASE(
    "WP318 mouse anchored wheel zoom clamps with visible boundary feedback",
    "[devstudio][frame-plan][zoom][wp318]") {
    (void)application();
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(
        readText(PELICAN_TEST_FRAME_PLAN_FIXTURE)));
    QApplication::processEvents();
    QGraphicsView &view = logicalView(widget);
    REQUIRE(view.transformationAnchor() == QGraphicsView::AnchorUnderMouse);

    const auto wheel = [&](int delta) {
        QWheelEvent event{QPointF{12.0, 12.0}, QPointF{12.0, 12.0},
                          QPoint{}, QPoint{0, delta}, Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false};
        QApplication::sendEvent(view.viewport(), &event);
    };
    for (int index = 0; index < 20; ++index) {
        wheel(120);
    }
    REQUIRE(std::abs(view.transform().m11() - 4.0) < 0.000001);
    REQUIRE(view.property("pelicanZoomBoundary").toString() ==
            QStringLiteral("maximum"));
    auto *zoom_status = widget.findChild<QLabel *>(
        QStringLiteral("pelican.framePlanZoomStatus"));
    REQUIRE(zoom_status != nullptr);
    REQUIRE(zoom_status->text().contains(QStringLiteral("maximum")));

    for (int index = 0; index < 40; ++index) {
        wheel(-120);
    }
    REQUIRE(std::abs(view.transform().m11() - 0.25) < 0.000001);
    REQUIRE(view.property("pelicanZoomBoundary").toString() ==
            QStringLiteral("minimum"));
    REQUIRE(zoom_status->text().contains(QStringLiteral("minimum")));
}

TEST_CASE(
    "WP318 selected target preserves every WP307 physical fact and scopes both alias outcomes",
    "[devstudio][frame-plan][physical-overlay][target][wp318]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");
    const Json &physical = wire.at("physical_target_plan");
    const Json &published_resources = physical.at("resources");
    const std::string endpoint = selectedPlanningEndpoint(physical);

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    QComboBox &selector = targetSelector(widget);

    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QString::fromStdString(endpoint));
    REQUIRE(published_resources.size() == 22);

    std::size_t arbitrary_materialized_count = 0;
    std::size_t same_pixel_count = 0;
    for (const auto &published : published_resources) {
        const std::string name =
            published.at("logical_resource").get<std::string>();
        selector.setCurrentText(QString::fromStdString(name));
        QApplication::processEvents();
        QGraphicsItem *row = physicalResourceItem(logical, name);
        INFO("physical resource overlay: " << name);
        REQUIRE(row != nullptr);
        REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).size() ==
                1);
        REQUIRE(logical.property("pelicanPhysicalResourceCount")
                    .toULongLong() == 1);
        REQUIRE(logical.property("pelicanSelectedTarget").toString() ==
                QString::fromStdString(name));
        REQUIRE(row->data(FramePlanProfileRole).toString() ==
                QStringLiteral("optimized"));
        REQUIRE(row->data(FramePlanEndpointRole).toString() ==
                QString::fromStdString(endpoint));
        REQUIRE(row->data(FramePlanReasonRole).toString().toStdString() ==
                published.at("reason").get<std::string>());
        REQUIRE(row->data(FramePlanWidestReadRole).toString().toStdString() ==
                published.at("widest_read").get<std::string>());
        REQUIRE(row->data(FramePlanAliasableRole).toBool() ==
                published.at("aliasable").get<bool>());
        REQUIRE(
            row->data(FramePlanRepresentationRole).toString().toStdString() ==
            published.at("representation").get<std::string>());

        const Json &lifetime = published.at("lifetime");
        const bool used = lifetime.at("used").get<bool>();
        REQUIRE(row->data(FramePlanLifetimeUsedRole).toBool() == used);
        if (used) {
            REQUIRE(row->data(FramePlanLifetimeFirstRole).toULongLong() ==
                    lifetime.at("first_use").get<std::size_t>());
            REQUIRE(row->data(FramePlanLifetimeLastRole).toULongLong() ==
                    lifetime.at("last_use").get<std::size_t>());
        } else {
            REQUIRE_FALSE(row->data(FramePlanLifetimeFirstRole).isValid());
            REQUIRE_FALSE(row->data(FramePlanLifetimeLastRole).isValid());
        }

        arbitrary_materialized_count +=
            published.at("reason") ==
            "arbitrary read requires a materialized resource";
        same_pixel_count += published.at("widest_read") == "same_pixel";
        REQUIRE(logical.property("pelicanSelectedResourceReason")
                    .toString()
                    .toStdString() ==
                published.at("reason").get<std::string>());
        REQUIRE(logical.property("pelicanSelectedResourceWidestRead")
                    .toString()
                    .toStdString() ==
                published.at("widest_read").get<std::string>());
        REQUIRE(logical.property("pelicanSelectedResourceAliasable")
                    .toBool() == published.at("aliasable").get<bool>());
        REQUIRE(logical.property("pelicanSelectedResourceRepresentation")
                    .toString()
                    .toStdString() ==
                published.at("representation").get<std::string>());
        const QString expected_lifetime =
            used ? QStringLiteral("[%1, %2]")
                       .arg(static_cast<qulonglong>(
                           lifetime.at("first_use").get<std::size_t>()))
                       .arg(static_cast<qulonglong>(
                           lifetime.at("last_use").get<std::size_t>()))
                 : QStringLiteral("unused");
        REQUIRE(logical.property("pelicanSelectedResourceLifetime")
                    .toString() == expected_lifetime);
        REQUIRE(logical.sceneRect().contains(row->sceneBoundingRect()));

        QString visible_facts;
        for (QGraphicsItem *child : row->childItems()) {
            if (auto *text =
                    dynamic_cast<QGraphicsSimpleTextItem *>(child);
                text != nullptr && text->isVisible()) {
                visible_facts += text->text();
            }
        }
        REQUIRE(visible_facts.contains(QString::fromStdString(name)));
        REQUIRE(visible_facts.contains(QString::fromStdString(
            published.at("reason").get<std::string>())));
        REQUIRE(visible_facts.contains(QString::fromStdString(
            published.at("widest_read").get<std::string>())));
        REQUIRE(visible_facts.contains(QString::fromStdString(
            published.at("representation").get<std::string>())));
        REQUIRE(visible_facts.contains(
            published.at("aliasable").get<bool>()
                ? QStringLiteral("aliasable: yes")
                : QStringLiteral("aliasable: no")));
        REQUIRE(visible_facts.contains(expected_lifetime));
    }
    REQUIRE(arbitrary_materialized_count == 16);
    REQUIRE(same_pixel_count == 1);

    selector.setCurrentText(QStringLiteral("Bloom_Threshold_RT"));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanAdoptedAliasCount").toULongLong() == 1);
    REQUIRE(logical.property("pelicanNotAdoptedAliasCount").toULongLong() ==
            1);
    const QGraphicsItem *adopted = physicalItemWithState(
        logical, FramePlanAliasOverlayItem, "adopted");
    const QGraphicsItem *not_adopted = physicalItemWithState(
        logical, FramePlanAliasOverlayItem, "not_adopted");
    REQUIRE(adopted != nullptr);
    REQUIRE(not_adopted != nullptr);
    REQUIRE(strings(adopted->data(FramePlanMembersRole)) ==
            StringSet{"Bloom_Threshold_RT", "g_emissive"});
    REQUIRE(strings(not_adopted->data(FramePlanMembersRole)) ==
            StringSet{"Bloom_Threshold_RT", "gbuffer_albedo"});
    REQUIRE_FALSE(not_adopted->data(FramePlanReasonRole).isValid());
    REQUIRE(not_adopted->toolTip().contains(
        QStringLiteral("not adopted")));
    REQUIRE(logical.property("pelicanSelectedResourceReason").toString() ==
            QStringLiteral("arbitrary read requires a materialized resource"));
    REQUIRE(logical.property("pelicanSelectedResourceWidestRead").toString() ==
            QStringLiteral("arbitrary"));
    REQUIRE(logical.property("pelicanSelectedResourceAliasable").toBool());
    REQUIRE(logical.property("pelicanSelectedResourceRepresentation")
                .toString() == QStringLiteral("materialized_image"));
    REQUIRE(logical.property("pelicanSelectedResourceLifetime").toString() ==
            QStringLiteral("[11, 12]"));
}

TEST_CASE(
    "WP318 selected target updates its profile and endpoint through the production widget boundary",
    "[devstudio][frame-plan][physical-overlay][profile][wp318]") {
    (void)application();
    const Json optimized = independentOpportunityFramePlan(
        Pelican::PlanningProfileKind::optimized);
    const Json conservative = independentOpportunityFramePlan(
        Pelican::PlanningProfileKind::conservative_debug);

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(optimized.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
    REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).size() == 1);
    QGraphicsItem *optimized_row =
        itemsOfKind(logical, FramePlanResourceLifetimeItem).front();
    REQUIRE(optimized_row->data(FramePlanProfileRole).toString() ==
            QStringLiteral("optimized"));
    const QString selected =
        logical.property("pelicanSelectedTarget").toString();
    REQUIRE_FALSE(selected.isEmpty());

    widget.receiveResult(QByteArray::fromStdString(conservative.dump()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("conservative_debug"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
    REQUIRE(logical.property("pelicanSelectedTarget").toString() == selected);
    REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).size() == 1);
    QGraphicsItem *conservative_row =
        itemsOfKind(logical, FramePlanResourceLifetimeItem).front();
    REQUIRE(conservative_row->data(FramePlanProfileRole).toString() ==
            QStringLiteral("conservative_debug"));
    REQUIRE(conservative_row->data(FramePlanEndpointRole).toString() ==
            QStringLiteral("device:0"));
}

TEST_CASE(
    "WP318 flat compiler feature toggle purges removed subtree nodes in one widget",
    "[devstudio][frame-plan][target-subtree][feature-toggle][wp318]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER, PELICAN_WITH_IMGUI);
#if !PELICAN_RUNTIME_SHADER_COMPILER
    const Json base = exampleFramePlan(false);
    REQUIRE_FALSE(wireNodeNames(base).contains("pelican_ui"));
    EmbeddedViewport off_viewport;
    FramePlanWidget off_widget{&off_viewport};
    off_widget.receiveResult(QByteArray::fromStdString(base.dump()));
    QApplication::processEvents();
    QGraphicsScene &off_logical = scene(off_widget);
    REQUIRE(off_logical.property("pelicanPhysicalResourceCount")
                .toULongLong() == 1);
    REQUIRE(off_logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(off_logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
    return;
#else
    const Json enabled = exampleFramePlan(true);
    const Json disabled = exampleFramePlan(false);
    REQUIRE(wireNodeNames(enabled).contains("pelican_ui"));
    REQUIRE_FALSE(wireNodeNames(disabled).contains("pelican_ui"));

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(enabled.dump()));
    targetSelector(widget).setCurrentText(QStringLiteral("display"));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    QGraphicsItem *ui = nodeItem(logical, "pelican_ui");
    REQUIRE(ui != nullptr);
    ui->setSelected(true);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanSelectedNode").toString() ==
            QStringLiteral("pelican_ui"));
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            1);

    widget.receiveResult(QByteArray::fromStdString(disabled.dump()));
    QApplication::processEvents();
    REQUIRE(nodeItem(logical, "pelican_ui") == nullptr);
    REQUIRE(logical.property("pelicanSelectedNode").toString().isEmpty());
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            1);
    REQUIRE(logical.property("pelicanSelectedTarget").toString() ==
            QStringLiteral("display"));
    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
#endif
}

TEST_CASE(
    "WP318 flat preview and XR variants expose the specified logical and physical target states",
    "[devstudio][frame-plan][target-subtree][variant][wp318]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER, PELICAN_WITH_IMGUI,
            PELICAN_WITH_OPENXR);
    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};

    const Json flat = animgraphFramePlan(
        true, Pelican::RenderPipelineGraphVariant::flat);
    widget.receiveResult(QByteArray::fromStdString(flat.dump()));
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);
    REQUIRE_FALSE(itemsOfKind(logical, FramePlanNodeItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanNodeItem).size() <= 5);
    REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).size() == 1);
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            1);

    const Json preview = animgraphFramePlan(
        true, Pelican::RenderPipelineGraphVariant::preview);
    REQUIRE_FALSE(preview.contains("physical_target_plan"));
    widget.receiveResult(QByteArray::fromStdString(preview.dump()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanGraph").toString() ==
            QString::fromStdString(preview.at("graph").get<std::string>()));
    REQUIRE_FALSE(itemsOfKind(logical, FramePlanNodeItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanNodeItem).size() <= 5);
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            0);
    const auto preview_context =
        itemsOfKind(logical, FramePlanPhysicalContextItem);
    REQUIRE(preview_context.size() == 1);
    REQUIRE(preview_context.front()
                ->data(FramePlanPhysicalStateRole)
                .toString() == QStringLiteral("unavailable"));
    REQUIRE(preview_context.front()->toolTip().contains(
        QStringLiteral("physical_plan_missing")));

#if PELICAN_WITH_OPENXR
    const Json xr = animgraphFramePlan(
        true, Pelican::RenderPipelineGraphVariant::xr);
    widget.receiveResult(QByteArray::fromStdString(xr.dump()));
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanGraph").toString() ==
            QString::fromStdString(xr.at("graph").get<std::string>()));
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            1);
    REQUIRE_FALSE(itemsOfKind(logical, FramePlanNodeItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanNodeItem).size() <= 5);
    REQUIRE(logical.property("pelicanPlanningProfile").toString() ==
            QStringLiteral("optimized"));
    REQUIRE(logical.property("pelicanPlanningEndpoint").toString() ==
            QStringLiteral("device:0"));
#else
    try {
        (void)animgraphFramePlan(
            true, Pelican::RenderPipelineGraphVariant::xr);
        FAIL("XR production variant unexpectedly compiled without OpenXR");
    } catch (const std::runtime_error &error) {
        REQUIRE(std::string_view{error.what()}.find(
                    "XR graph variant is unavailable in this build") !=
                std::string_view::npos);
    }
#endif
}

TEST_CASE(
    "WP318 compiler feature removal purges target subtree selection and physical details in one widget",
    "[devstudio][frame-plan][target-subtree][purge][wp318]") {
    (void)application();
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER, PELICAN_WITH_IMGUI);
    const Json enabled = animgraphFramePlan(
        true, Pelican::RenderPipelineGraphVariant::flat);
    const Json disabled = animgraphFramePlan(
        false, Pelican::RenderPipelineGraphVariant::flat);

#if !PELICAN_RUNTIME_SHADER_COMPILER
    REQUIRE(wireNodeNames(enabled) == wireNodeNames(disabled));
    REQUIRE_FALSE(wireNodeNames(enabled).contains("shadow_depth"));
    EmbeddedViewport off_viewport;
    FramePlanWidget off_widget{&off_viewport};
    off_widget.receiveResult(QByteArray::fromStdString(disabled.dump()));
    QApplication::processEvents();
    REQUIRE(targetSelector(off_widget).findText(
                QStringLiteral("shadow_map")) == -1);
    REQUIRE(scene(off_widget)
                .property("pelicanPhysicalResourceCount")
                .toULongLong() == 1);
    return;
#else
    REQUIRE(wireNodeNames(enabled).contains("shadow_depth"));
    REQUIRE_FALSE(wireNodeNames(disabled).contains("shadow_depth"));

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.receiveResult(QByteArray::fromStdString(enabled.dump()));
    QComboBox &selector = targetSelector(widget);
    REQUIRE(selector.findText(QStringLiteral("shadow_map")) >= 0);
    selector.setCurrentText(QStringLiteral("shadow_map"));
    subtreeDepth(widget).setValue(1);
    QApplication::processEvents();
    QGraphicsScene &logical = scene(widget);

    REQUIRE(logical.property("pelicanSelectedTarget").toString() ==
            QStringLiteral("shadow_map"));
    REQUIRE_FALSE(sceneNodeNames(logical).empty());
    REQUIRE(sceneNodeNames(logical).contains("shadow_depth"));
    REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).size() == 1);
    REQUIRE_FALSE(logical.property("pelicanSelectedResourceReason")
                      .toString()
                      .isEmpty());
    QGraphicsItem *shadow = nodeItem(logical, "shadow_depth");
    REQUIRE(shadow != nullptr);
    shadow->setSelected(true);
    QApplication::processEvents();
    REQUIRE(logical.property("pelicanSelectedNode").toString() ==
            QStringLiteral("shadow_depth"));

    widget.receiveResult(QByteArray::fromStdString(disabled.dump()));
    QApplication::processEvents();

    REQUIRE(selector.findText(QStringLiteral("shadow_map")) == -1);
    REQUIRE(selector.currentIndex() == -1);
    REQUIRE(logical.property("pelicanSelectedTarget").toString().isEmpty());
    REQUIRE(logical.property("pelicanSelectedResource").toString().isEmpty());
    REQUIRE(logical.property("pelicanSelectedNode").toString().isEmpty());
    REQUIRE(sceneNodeNames(logical).empty());
    REQUIRE(itemsOfKind(logical, FramePlanResourceLifetimeItem).empty());
    REQUIRE(itemsOfKind(logical, FramePlanAliasOverlayItem).empty());
    REQUIRE(logical.property("pelicanPhysicalResourceCount").toULongLong() ==
            0);
    REQUIRE(logical.property("pelicanSelectedResourceReason")
                .toString()
                .isEmpty());
    REQUIRE(logical.property("pelicanSelectedResourceWidestRead")
                .toString()
                .isEmpty());
    REQUIRE(logical.property("pelicanSelectedResourceRepresentation")
                .toString()
                .isEmpty());
    REQUIRE(logical.property("pelicanSelectedResourceLifetime")
                .toString()
                .isEmpty());
#endif
}


TEST_CASE(
    "WP320 stress: interleaved target, depth, zoom and repeated drags stay stable",
    "[devstudio][frame-plan][drag][stress][wp320]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.show();
    widget.resize(1200, 800);
    QApplication::processEvents();
    widget.receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();

    QGraphicsScene &logical = scene(widget);
    QGraphicsView &view = logicalView(widget);
    QComboBox &selector = targetSelector(widget);
    QSpinBox &depth = subtreeDepth(widget);
    QWidget *const vp = view.viewport();
    REQUIRE(vp != nullptr);

    const auto finite_rect = [](const QRectF &r) {
        return std::isfinite(r.x()) && std::isfinite(r.y()) &&
               std::isfinite(r.width()) && std::isfinite(r.height());
    };

    for (int target = 0; target < selector.count(); ++target) {
        selector.setCurrentIndex(target);
        QApplication::processEvents();
        for (int d = 1; d <= 3; ++d) {
            depth.setValue(d);
            QApplication::processEvents();

            // zoom in and out around the view centre
            for (int w = 0; w < 3; ++w) {
                QWheelEvent in{QPointF{vp->rect().center()},
                               vp->mapToGlobal(vp->rect().center()),
                               QPoint{0, 0}, QPoint{0, 120},
                               Qt::NoButton, Qt::NoModifier,
                               Qt::NoScrollPhase, false};
                QApplication::sendEvent(vp, &in);
            }
            QApplication::processEvents();

            const auto nodes = itemsOfKind(logical, FramePlanNodeItem);
            for (QGraphicsItem *node : nodes) {
                const QPoint press =
                    view.mapFromScene(node->sceneBoundingRect().center());
                if (!vp->rect().contains(press)) continue;
                QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, press);
                // long drag with many intermediate moves
                for (int step = 1; step <= 40; ++step) {
                    const QPoint at = press + QPoint{step * 7, step * 5};
                    if (!vp->rect().contains(at)) break;
                    QTest::mouseMove(vp, at, 1);
                }
                QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier,
                                    press + QPoint{40 * 7, 40 * 5});
                QApplication::processEvents();
                REQUIRE(finite_rect(logical.sceneRect()));
            }

            for (int w = 0; w < 3; ++w) {
                QWheelEvent out{QPointF{vp->rect().center()},
                                vp->mapToGlobal(vp->rect().center()),
                                QPoint{0, 0}, QPoint{0, -120},
                                Qt::NoButton, Qt::NoModifier,
                                Qt::NoScrollPhase, false};
                QApplication::sendEvent(vp, &out);
            }
            QApplication::processEvents();
            REQUIRE(finite_rect(logical.sceneRect()));
        }
    }
    REQUIRE(finite_rect(logical.sceneRect()));
}


TEST_CASE(
    "WP320 a frame plan arriving mid-drag does not destabilise the scene",
    "[devstudio][frame-plan][drag][refresh][wp320]") {
    (void)application();
    Json wire = Json::parse(readText(PELICAN_TEST_FRAME_PLAN_FIXTURE));
    wire["runtime_resolution"] =
        exampleFramePlan(true).at("runtime_resolution");
    const QByteArray captured = QByteArray::fromStdString(wire.dump());

    EmbeddedViewport viewport;
    FramePlanWidget widget{&viewport};
    widget.show();
    widget.resize(1200, 800);
    QApplication::processEvents();
    widget.receiveResult(captured);
    QApplication::processEvents();

    QGraphicsScene &logical = scene(widget);
    QGraphicsView &view = logicalView(widget);
    QWidget *const vp = view.viewport();
    REQUIRE(vp != nullptr);

    const auto finite_rect = [](const QRectF &r) {
        return std::isfinite(r.x()) && std::isfinite(r.y()) &&
               std::isfinite(r.width()) && std::isfinite(r.height());
    };

    const auto nodes = itemsOfKind(logical, FramePlanNodeItem);
    REQUIRE_FALSE(nodes.empty());
    const QPoint press =
        view.mapFromScene(nodes.front()->sceneBoundingRect().center());
    REQUIRE(vp->rect().contains(press));

    QTest::mousePress(vp, Qt::LeftButton, Qt::NoModifier, press);
    for (int step = 1; step <= 30; ++step) {
        QTest::mouseMove(vp, press + QPoint{step * 6, step * 4}, 1);
        // the live studio keeps receiving frame plans while the user drags
        if (step % 5 == 0) {
            widget.receiveResult(captured);
            QApplication::processEvents();
        }
        REQUIRE(finite_rect(logical.sceneRect()));
    }
    QTest::mouseRelease(vp, Qt::LeftButton, Qt::NoModifier,
                        press + QPoint{30 * 6, 30 * 4});
    QApplication::processEvents();
    REQUIRE(finite_rect(logical.sceneRect()));
}


} // namespace PelicanStudio
