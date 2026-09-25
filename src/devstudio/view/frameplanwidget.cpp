#include "frameplanwidget.hpp"

#include "frameplanedit.hpp"
#include "frameplangraphics.hpp"
#include "jsonrpcrequest.hpp"
#include "panelhelp.hpp"
#include "../model/frameplanmodel.hpp"
#include "../viewport/embeddedviewport.hpp"
#include "projectformat.hpp"
#include "projectpathresolver.hpp"
#include "shaderreloadfollower.hpp"
#include "rendertargetform.hpp"
#include "materialpassedit.hpp"
#include "materialpassform.hpp"
#include "renderconfigdigest.hpp"
#include "shadersourceresolver.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QByteArray>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFont>
#include <QGraphicsView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabBar>
#include <QHideEvent>
#include <QShowEvent>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <utility>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {
namespace {

constexpr auto ToolTabIdProperty = "pelicanToolTabId";
constexpr int GpuTimingPollIntervalMs = 1000;

std::size_t gpuTimingCount(const QJsonObject &object,
                           const QString &field) {
    const QJsonValue value = object.value(field);
    const double number = value.toDouble(-1.0);
    if (!value.isDouble() || !std::isfinite(number) || number < 0.0 ||
        std::trunc(number) != number ||
        number > static_cast<double>(
                     std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error(
            QStringLiteral("get_gpu_timing %1 must be a non-negative integer")
                .arg(field)
                .toStdString());
    }
    return static_cast<std::size_t>(number);
}

double gpuTimingDuration(const QJsonObject &object,
                         const QString &field) {
    const QJsonValue value = object.value(field);
    const double number = value.toDouble(-1.0);
    if (!value.isDouble() || !std::isfinite(number) || number < 0.0) {
        throw std::runtime_error(
            QStringLiteral("get_gpu_timing %1 must be a non-negative number")
                .arg(field)
                .toStdString());
    }
    return number;
}

FramePlanGpuTimingSnapshot parseGpuTimingResult(
    const QByteArray &result_json) {
    QJsonParseError parse_error;
    const QJsonDocument document =
        QJsonDocument::fromJson(result_json, &parse_error);
    if (parse_error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        throw std::runtime_error(
            "get_gpu_timing result must be a JSON object");
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("schema")).toString() !=
            QStringLiteral("pelican.gpu_timing_node_averages") ||
        gpuTimingCount(object, QStringLiteral("version")) != 1) {
        throw std::runtime_error(
            "get_gpu_timing result has an unsupported schema or version");
    }
    const QJsonValue enabled = object.value(QStringLiteral("enabled"));
    const QJsonValue supported = object.value(QStringLiteral("supported"));
    const QJsonValue reason = object.value(QStringLiteral("reason"));
    const QJsonValue nodes = object.value(QStringLiteral("nodes"));
    if (!enabled.isBool() || !supported.isBool() || !reason.isString() ||
        reason.toString().isEmpty() || !nodes.isArray()) {
        throw std::runtime_error(
            "get_gpu_timing result has invalid state fields");
    }

    FramePlanGpuTimingSnapshot result;
    result.received = true;
    result.enabled = enabled.toBool();
    result.supported = supported.toBool();
    result.reason = reason.toString().toStdString();
    result.window_size =
        gpuTimingCount(object, QStringLiteral("window_size"));
    result.frame_count =
        gpuTimingCount(object, QStringLiteral("frame_count"));
    if (result.window_size == 0 ||
        result.frame_count > result.window_size) {
        throw std::runtime_error(
            "get_gpu_timing result has an invalid frame window");
    }

    const QJsonArray rows = nodes.toArray();
    result.nodes.reserve(static_cast<std::size_t>(rows.size()));
    for (const QJsonValue &value : rows) {
        if (!value.isObject()) {
            throw std::runtime_error(
                "get_gpu_timing nodes entries must be objects");
        }
        const QJsonObject row = value.toObject();
        const QString node_kind =
            row.value(QStringLiteral("node_kind")).toString();
        const QString node_name =
            row.value(QStringLiteral("node_name")).toString();
        const QJsonValue body_supported =
            row.value(QStringLiteral("body_supported"));
        if (node_kind.isEmpty() || node_name.isEmpty() ||
            !body_supported.isBool()) {
            throw std::runtime_error(
                "get_gpu_timing node identity is invalid");
        }
        const auto view_index =
            gpuTimingCount(row, QStringLiteral("view_index"));
        const auto sample_count =
            gpuTimingCount(row, QStringLiteral("sample_count"));
        if (view_index >
                std::numeric_limits<std::uint32_t>::max() ||
            sample_count == 0 || sample_count > result.frame_count) {
            throw std::runtime_error(
                "get_gpu_timing node counts are invalid");
        }
        result.nodes.push_back(FramePlanGpuTimingRow{
            static_cast<std::uint32_t>(view_index),
            node_kind.toStdString(), node_name.toStdString(),
            gpuTimingDuration(row, QStringLiteral("barriers_ms")),
            gpuTimingDuration(row, QStringLiteral("body_ms")),
            body_supported.toBool(), sample_count});
    }
    return result;
}

const QStringList &defaultToolTabOrder() {
    static const QStringList order{
        QStringLiteral("logical-graph"), QStringLiteral("passes"),
        QStringLiteral("resources"), QStringLiteral("physical-plan"),
        QStringLiteral("decisions"), QStringLiteral("backends"),
        QStringLiteral("barriers"), QStringLiteral("materials"),
        QStringLiteral("raw-json"),
    };
    return order;
}

QString toolTabId(const QWidget *page) {
    return page == nullptr
               ? QString{}
               : page->property(ToolTabIdProperty).toString();
}

void addToolTab(QTabWidget &tabs, QWidget *page, const QString &id,
                const QString &label) {
    page->setProperty(ToolTabIdProperty, id);
    tabs.addTab(page, label);
}

using Json = nlohmann::json;

QString text(const std::string &value) {
    return QString::fromStdString(value);
}

QString joined(const std::vector<std::string> &values) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const auto &value : values) {
        result.push_back(text(value));
    }
    return result.join(QStringLiteral(", "));
}

QString authoredExtent(const FramePlanExtent &extent) {
    return extent.kind == "output_relative"
               ? QStringLiteral("output_relative %1 x %2")
                     .arg(extent.scale_x, 0, 'g', 6)
                     .arg(extent.scale_y, 0, 'g', 6)
               : QStringLiteral("fixed %1 x %2")
                     .arg(static_cast<qulonglong>(extent.width))
                     .arg(static_cast<qulonglong>(extent.height));
}

QString lifetimeLabel(const FramePlanLifetime &lifetime) {
    if (!lifetime.used) {
        return QStringLiteral("unused");
    }
    return QStringLiteral("[%1, %2]")
        .arg(static_cast<qulonglong>(*lifetime.first_use))
        .arg(static_cast<qulonglong>(*lifetime.last_use));
}

QString byteCount(std::size_t bytes) {
    if (bytes >= 1024 * 1024) {
        return QStringLiteral("%1 MiB")
            .arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024) {
        return QStringLiteral("%1 KiB")
            .arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 B").arg(static_cast<qulonglong>(bytes));
}

QTreeWidget *makeTree(const QStringList &headers, QWidget *parent) {
    auto *tree = new QTreeWidget(parent);
    tree->setColumnCount(headers.size());
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(true);
    tree->setUniformRowHeights(true);
    tree->setAlternatingRowColors(true);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->header()->setStretchLastSection(true);
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    return tree;
}

// Q-4. A tree whose first column sizes itself to its contents measures every
// row it holds again on every single insertion, and these trees are filled a
// few hundred rows at a time. Measure once, when the filling is over -- the
// column ends up exactly as wide as it would have been. Together with not
// laying out the raw reply until somebody opens the tab that shows it, a
// `playground_pose` refresh went from 349 ms to 192 ms in a Debug build, of
// which the body of populate() is 277 ms to 129 ms.
class TreeFillGuard {
  public:
    explicit TreeFillGuard(std::initializer_list<QTreeWidget *> trees)
        : trees_{trees} {
        for (QTreeWidget *tree : trees_) {
            tree->setUpdatesEnabled(false);
            tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
        }
    }
    TreeFillGuard(const TreeFillGuard &) = delete;
    TreeFillGuard &operator=(const TreeFillGuard &) = delete;
    ~TreeFillGuard() {
        for (QTreeWidget *tree : trees_) {
            tree->header()->setSectionResizeMode(0,
                                                 QHeaderView::ResizeToContents);
            tree->setUpdatesEnabled(true);
        }
    }

  private:
    std::vector<QTreeWidget *> trees_;
};

QTreeWidgetItem *groupItem(QTreeWidgetItem *parent, const QString &label,
                           std::size_t count = 0) {
    const QString title = count == 0
                              ? label
                              : QStringLiteral("%1 (%2)")
                                    .arg(label)
                                    .arg(static_cast<qulonglong>(count));
    auto *item = new QTreeWidgetItem(parent);
    item->setText(1, title);
    QFont font = item->font(1);
    font.setBold(true);
    item->setFont(1, font);
    return item;
}

void addFact(QTreeWidgetItem *parent, const QString &name,
             const QString &value) {
    if (value.isEmpty()) {
        return;
    }
    auto *item = new QTreeWidgetItem(parent);
    item->setText(1, name);
    item->setText(2, value);
}

bool itemMatches(QTreeWidgetItem *item, const QString &needle,
                 int columns) {
    for (int column = 0; column < columns; ++column) {
        if (item->text(column).contains(needle, Qt::CaseInsensitive)) {
            return true;
        }
    }
    for (int index = 0; index < item->childCount(); ++index) {
        if (itemMatches(item->child(index), needle, columns)) {
            return true;
        }
    }
    return false;
}

void applyFilter(QTreeWidget &tree, const QString &needle) {
    for (int index = 0; index < tree.topLevelItemCount(); ++index) {
        QTreeWidgetItem *item = tree.topLevelItem(index);
        item->setHidden(!needle.isEmpty() &&
                        !itemMatches(item, needle, tree.columnCount()));
    }
}

const FramePlanResource *findResource(const FramePlanModel &model,
                                      const std::string &name) {
    const auto found = std::lower_bound(
        model.resources.begin(), model.resources.end(), name,
        [](const FramePlanResource &resource, const std::string &candidate) {
            return resource.name < candidate;
        });
    return found != model.resources.end() && found->name == name ? &*found
                                                                 : nullptr;
}

QString resourceLabel(const FramePlanModel &model, const std::string &name) {
    const FramePlanResource *resource = findResource(model, name);
    if (resource == nullptr || resource->format == "unknown") {
        return text(name);
    }
    return QStringLiteral("%1  [%2]").arg(text(name), text(resource->format));
}

// The zoom, pan and fit behaviour this view used to define alone now lives
// in NodeCanvasView (nodegraphics), so the common graph canvas behaves the
// same way. The limits and the published pelicanZoom* properties are the
// ones this view shipped.
using FramePlanGraphicsView = PelicanStudio::NodeCanvasView;

} // namespace

struct FramePlanWidget::Impl {
    FramePlanWidget &owner;
    EmbeddedViewport &viewport;
    QLabel *status = nullptr;
    QLineEdit *filter = nullptr;
    QPushButton *refresh = nullptr;
    QComboBox *target = nullptr;
    QSpinBox *depth = nullptr;
    QLabel *zoom_status = nullptr;
    QTimer *gpu_timing_poll = nullptr;
    QTabWidget *tabs = nullptr;
    QSplitter *logical_splitter = nullptr;
    FramePlanGraphicsView *logical = nullptr;
    FramePlanGraphicsScene *logical_scene = nullptr;
    QTreeWidget *logical_details = nullptr;
    QTreeWidget *passes = nullptr;
    QTreeWidget *resources = nullptr;
    QTreeWidget *physical = nullptr;
    QTreeWidget *decisions = nullptr;
    QTreeWidget *backends = nullptr;
    QTreeWidget *barriers = nullptr;
    QTreeWidget *materials = nullptr;
    QPlainTextEdit *raw_json = nullptr;
    // The text box does not hold the reply that is on screen.
    bool raw_json_stale = true;
    std::optional<FramePlanModel> model;
    QString requested_hit_group;
    std::optional<Pelican::ProjectPathResolver> project_resolver;
    FramePlanWidget::ShaderSourceOpenAction shader_source_open_action;
    // F-3: the engine's shader reload status, read after a shader file a node
    // reads is written.
    std::unique_ptr<ShaderReloadFollower> reload_follower;
    // F-5: where a shader reload's log goes besides the marks (the Pass
    // Editor's log under its form); empty text clears it.
    std::function<void(const QString &)> shader_reload_log_action;
    // G-5: who restarts the engine for a committed setting it reads only at
    // start. The window sets it, so a scene with unsaved edits is asked about
    // first; without it the viewport restarts at once.
    std::function<void(const QString &)> engine_restart_action;
    // Why the engine did not restart, kept until the refreshed plan's summary
    // has been shown so that it is not the summary that stays on the row.
    QString restart_declined_notice;
    // The status line a shader reload put up, so a later success takes down
    // that line and nothing else.
    QString shown_reload_notice;
    std::function<void(const QString &, const QString &)> preset_pass_edit_action;
    std::function<void(const QString &)> task_edit_action;
    FramePlanWidget::GraphNodeOpenAction graph_node_open_action;
    std::function<void(const QString &, const QStringList &)> task_connection_action;
    std::function<void(const QString &, const QString &, const QStringList &)> pass_connection_action;
    std::function<void(const QString &, const QStringList &, quint64)> node_order_action;
    qint64 pending_request = 0;
    // A ray-tracing project's first generation stays private until its first
    // submitted frame, and the engine answers RPC before that. While
    // get_frame_plan names that state, ask again instead of showing a failure.
    int unpublished_plan_retries = 0;
    static constexpr int max_unpublished_plan_retries = 40;
    static constexpr int unpublished_plan_retry_ms = 250;
    qint64 pending_gpu_timing_request = 0;
    // Half of what gates the GPU timing clock: whether the engine still has
    // anything to report. The other half is whether anyone is looking at the
    // panel, and either can change without the other.
    bool gpu_timing_reportable = true;

    // --- N-8: editing on the canvas.
    FramePlanEditCapability edit;
    FramePlanLayoutCapability placement;
    std::function<void(const QString &, const QString &)> pass_insert_action;
    std::function<void()> task_insert_action;
    FramePlanAuthoring authoring;
    FramePlanTaskAuthoring task_authoring;
    qint64 pending_authoring_request = 0;
    qint64 pending_task_request = 0;
    qint64 pending_edit_request = 0;
    qint64 pending_ticket_request = 0;
    QString pending_ticket;
    bool edit_signals_connected = false;
    // The graph the placement on screen belongs to. A placement written against
    // a graph that is no longer shown would name passes that are not there.
    std::string placement_graph;
    bool placement_loaded = false;
    // --- F-1: render targets.
    RenderTargetForm *render_target_form = nullptr;
    // The method of the edit in flight, so its rejection is read in its terms.
    std::string submitted_method;
    // The target the edit in flight names; it becomes the canvas target once
    // the edit commits, so the new node is what the user sees next.
    std::string submitted_target;
    std::string pending_target_selection;
    // A double-click that arrived before the context naming the target did,
    // and a New render target... that arrived before any context at all.
    std::string pending_form_target;
    bool pending_new_target_form = false;
    // T-1: the name New render target... opens on (empty: none), the target
    // the form last created, and who is told once it commits.
    std::string new_target_name;
    std::string created_target;
    // T-1: what a band's editor completes to, by field id.
    std::function<QStringList(const QString &)> shader_choices;
    std::function<void(const QString &)> render_target_created_action;
    // --- G-1: material passes. Save may be two changes: the targets first,
    // then -- from the context that names them -- the pass.
    MaterialPassForm *material_pass_form = nullptr;
    bool pending_material_form = false;
    std::optional<MaterialPassFormValues> pending_material_pass;
    // The context in flight when the targets committed predates them; the
    // pass waits for the next one.
    bool material_context_stale = false;
    // The targets Save made for the pass in flight: they stay if it is refused.
    std::vector<std::string> material_targets_made;
    // --- Q-13: the digest every panel that edits the document shares.
    std::shared_ptr<RenderConfigDigest> digest_hub;
    // Another panel moved the document while a read was already on its way:
    // that answer may predate the move, so the read is asked for again.
    bool authoring_reread = false;
    bool task_reread = false;
    // The edit in flight, as the gesture that built it. An external_modification
    // refusal reads the context again and builds it once more from that.
    std::function<FramePlanEditRequest()> submitted_build;
    bool submitted_retried = false;
    bool retry_pending = false;
    // Shown when the rebuilt edit is refused as well: reads the context again.
    QPushButton *reload_authoring = nullptr;

    Impl(FramePlanWidget &widget, EmbeddedViewport &embedded_viewport)
        : owner{widget}, viewport{embedded_viewport} {
        shader_source_open_action = [](const std::filesystem::path &path) {
            return QDesktopServices::openUrl(QUrl::fromLocalFile(
                QString::fromStdWString(path.wstring())));
        };
        reload_follower = std::make_unique<ShaderReloadFollower>(
            ShaderReloadFollower::Hooks{
                .request_status =
                    [this](QString *error) -> qint64 {
                    if (!viewport.rpcReady()) return 0;
                    return viewport.requestRpc(QStringLiteral("get_status"),
                                               QJsonObject{}, error);
                },
                .apply =
                    [this](const ShaderReloadMarks &marks,
                           const ShaderReloadReading &reading,
                           bool after_change) {
                        applyShaderReloadMarks(marks, reading, after_change);
                    },
                // F-5: a write asks the engine to recompile.
                .request_reload =
                    [this](QString *error) -> qint64 {
                    if (!viewport.rpcReady()) return 0;
                    return viewport.requestRpc(QStringLiteral("reload_shaders"),
                                               QJsonObject{}, error);
                },
                .reloaded =
                    [this](const ShaderReloadOutcome &outcome) {
                        applyShaderReloadOutcome(outcome);
                    },
            });
        owner.setFont(studioPanelFont());
        auto *layout = new QVBoxLayout(&owner);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);
        gpu_timing_poll = new QTimer(&owner);
        gpu_timing_poll->setObjectName(
            QStringLiteral("pelican.gpuTimingPollTimer"));
        gpu_timing_poll->setInterval(GpuTimingPollIntervalMs);
        owner.setProperty("pelicanGpuTimingPollIntervalMs",
                          GpuTimingPollIntervalMs);
        QObject::connect(gpu_timing_poll, &QTimer::timeout, &owner,
                         [this] { requestGpuTiming(); });

        auto *toolbar = new QHBoxLayout;
        refresh = new QPushButton(owner.tr("Refresh"), &owner);
        refresh->setObjectName(QStringLiteral("pelican.framePlanRefresh"));
        refresh->setToolTip(owner.tr(
            "Fetch get_frame_plan once. The multi-thousand-line response is "
            "not polled every frame because transfer, parsing, and rebuilding "
            "the trees would stall the editor."));
        filter = new QLineEdit(&owner);
        filter->setObjectName(QStringLiteral("pelican.framePlanFilter"));
        filter->setPlaceholderText(owner.tr("Filter"));
        filter->setToolTip(owner.tr(
            "Filter passes, resources, decisions, backends, or barriers"));
        filter->setClearButtonEnabled(true);
        toolbar->addWidget(refresh);
        toolbar->addWidget(filter, 1);
        toolbar->addWidget(new QLabel(owner.tr("Target"), &owner));
        target = new QComboBox(&owner);
        target->setObjectName(QStringLiteral("pelican.framePlanTarget"));
        target->setInsertPolicy(QComboBox::NoInsert);
        target->setMinimumContentsLength(18);
        target->setToolTip(owner.tr(
            "Whole frame shows every pass, and every declared target no pass "
            "writes as a node of its own. Choose one logical render target "
            "to show only the passes within the selected resource depth."));
        toolbar->addWidget(target);
        toolbar->addWidget(new QLabel(owner.tr("Depth"), &owner));
        depth = new QSpinBox(&owner);
        depth->setObjectName(QStringLiteral("pelican.framePlanDepth"));
        depth->setRange(0, 64);
        depth->setValue(1);
        depth->setToolTip(owner.tr(
            "Depth 0 shows no pass nodes. Depth 1 is exactly the unique "
            "writers and readers of the selected target."));
        toolbar->addWidget(depth);
        zoom_status = new QLabel(owner.tr("Zoom: 100%"), &owner);
        zoom_status->setObjectName(
            QStringLiteral("pelican.framePlanZoomStatus"));
        zoom_status->setFont(studioSmallFont());
        toolbar->addWidget(zoom_status);
        // N-7 rule 1: the gesture is described where the gesture is made.
        auto *order_nodes = new QPushButton(owner.tr("Order..."), &owner);
        order_nodes->setObjectName(QStringLiteral("pelican.framePlanOrderNodes"));
        toolbar->addWidget(order_nodes);
        QObject::connect(order_nodes, &QPushButton::clicked, &owner, [this] {
            if (!model || !node_order_action) return;
            if (!model->runtime_generation || *model->runtime_generation == 0) {
                showStatus(owner.tr("Node ordering needs a published runtime generation"), owner.tr("Node ordering requires a published runtime generation. Refresh the Frame Plan first."));
                return;
            }
            QStringList names;
            for (const auto &node : model->nodes) names.push_back(QString::fromStdString(node.name));
            node_order_action(QString::fromStdString(model->graph), names, *model->runtime_generation);
        });
        auto *edit_task = new QPushButton(owner.tr("Edit task"), &owner);
        edit_task->setObjectName(QStringLiteral("pelican.framePlanEditTask"));
        toolbar->addWidget(edit_task);
        QObject::connect(edit_task, &QPushButton::clicked, &owner, [this] {
            const auto &selected = logical_scene->selectedNode();
            if (!selected || !model || !task_edit_action) return;
            const auto node = std::ranges::find(model->nodes, selected->name, &FramePlanNode::name);
            if (node == model->nodes.end() || node->kind != "compute" || node->authored_name.empty()) {
                showStatus(owner.tr("Select a task with a source declaration to edit."));
                return;
            }
            task_edit_action(QString::fromStdString(node->authored_name));
        });
        auto *edit_pass = new QPushButton(owner.tr("Edit pass"), &owner);
        edit_pass->setObjectName(QStringLiteral("pelican.framePlanEditPresetPass"));
        toolbar->addWidget(edit_pass);
        QObject::connect(edit_pass, &QPushButton::clicked, &owner, [this] {
            const auto &selected = logical_scene->selectedNode();
            if (selected && preset_pass_edit_action)
                preset_pass_edit_action(QString::fromStdString(selected->graph), QString::fromStdString(selected->name));
        });
        auto *new_target = new QPushButton(owner.tr("New render target..."), &owner);
        new_target->setObjectName(
            QStringLiteral("pelican.framePlanNewRenderTarget"));
        toolbar->addWidget(new_target);
        auto *new_material_pass =
            new QPushButton(owner.tr("Add material pass..."), &owner);
        new_material_pass->setObjectName(
            QStringLiteral("pelican.framePlanAddMaterialPass"));
        toolbar->addWidget(new_material_pass);
        toolbar->addWidget(makePanelHelpButton(
            &owner, "pelican.framePlan.help", ManualFramePlanAnchor,
            owner.tr("Shift-drag: connect resources. The snapshot updates "
                     "only when the engine connects or Refresh is pressed; "
                     "it is not polled per frame.")));
        layout->addLayout(toolbar);

        status = new QLabel(&owner);
        status->setObjectName(QStringLiteral("pelican.framePlanStatus"));
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        applyPanelNoticeStyle(status);
        status->hide();
        // Q-13: the status row, and Reload beside it for the one refusal a
        // second try did not get past.
        auto *status_row = new QHBoxLayout;
        status_row->setContentsMargins(0, 0, 0, 0);
        status_row->addWidget(status, 1);
        reload_authoring = new QPushButton(owner.tr("Reload"), &owner);
        reload_authoring->setObjectName(
            QStringLiteral("pelican.framePlan.reloadAuthoring"));
        reload_authoring->setToolTip(owner.tr(
            "Read the rendering document again, then repeat the change."));
        reload_authoring->hide();
        QObject::connect(reload_authoring, &QPushButton::clicked, &owner,
                         [this] { reloadAuthoring(); });
        status_row->addWidget(reload_authoring);
        layout->addLayout(status_row);

        tabs = new QTabWidget(&owner);
        tabs->setObjectName(QStringLiteral("pelican.framePlanTabs"));
        tabs->setMovable(true);
        logical_splitter = new QSplitter(Qt::Horizontal, tabs);
        logical_splitter->setObjectName(
            QStringLiteral("pelican.framePlanLogicalSplitter"));
        logical_scene = new FramePlanGraphicsScene(&owner);
        logical_scene->connectionFeedback = [this](const QString &message) { showStatus(oneLine(message), message); };
        logical_scene->connectionRequested = [this](const FramePlanNodeKey &from, const FramePlanNodeKey &to) {
            if (!model || (!task_connection_action && !pass_connection_action)) {
                showStatus(owner.tr("Resource connection editing is unavailable."));
                return;
            }
            const auto producer = std::find_if(model->nodes.begin(), model->nodes.end(), [&](const auto &node) { return node.name == from.name; });
            const auto consumer = std::find_if(model->nodes.begin(), model->nodes.end(), [&](const auto &node) { return node.name == to.name; });
            if (from.graph != model->graph || to.graph != model->graph ||
                producer == model->nodes.end() || consumer == model->nodes.end() ||
                producer->writes.empty()) {
                showStatus(owner.tr("Drag from a node with resource outputs"), owner.tr("Drag from a node with resource outputs to a task or editable preset pass."));
                return;
            }
            QStringList outputs;
            for (const auto &resource : producer->writes) outputs.push_back(QString::fromStdString(resource));
            if (consumer->kind == "compute" && !consumer->authored_name.empty() && task_connection_action)
                task_connection_action(QString::fromStdString(consumer->authored_name), outputs);
            else if (consumer->kind == "render" && pass_connection_action)
                pass_connection_action(QString::fromStdString(model->graph), QString::fromStdString(consumer->name), outputs);
            else
                showStatus(owner.tr("Select an authored task or editable preset pass"), owner.tr("Select an authored task or editable preset pass as the destination."));
        };
        // N-8: the plain gestures. Shift-drag above is untouched; these are the
        // ones that finish as an authoring RPC rather than as a dialog.
        logical_scene->resourceEdgeRequested =
            [this](const FramePlanNodeKey &consumer,
                   const QStringList &resources, bool connect) {
                changeResourceEdge(consumer, resources, connect);
            };
        logical_scene->nodeRemoveRequested =
            [this](const FramePlanNodeKey &node) { removeNode(node); };
        logical_scene->inlineFieldCommitted =
            [this](const FramePlanNodeKey &node, const QString &field,
                   const QString &value) {
                commitInlineField(node, field, value);
            };
        logical_scene->inlineFieldProvider =
            [this](const FramePlanNodeKey &node) {
                std::vector<FramePlanInlineBand> bands;
                if (!model) return bands;
                for (const auto &field : framePlanInlineFields(
                         authoring, task_authoring, node.graph, node.name,
                         authoredTask(node.name)))
                    bands.push_back(FramePlanInlineBand{
                        text(field.field), text(field.label),
                        text(field.value)});
                return bands;
            };
        logical_scene->inlineFieldChoices =
            [this](const FramePlanNodeKey &, const QString &field) {
                return shader_choices ? shader_choices(field) : QStringList{};
            };
        // Double-click is the way to the detailed editors: everything the
        // bands do not carry lives in the two forms that already exist.
        logical_scene->nodeEditRequested =
            [this](const FramePlanNodeKey &node) {
                const auto task = authoredTask(node.name);
                if (!task.empty()) {
                    if (task_edit_action) task_edit_action(text(task));
                    return;
                }
                if (preset_pass_edit_action)
                    preset_pass_edit_action(text(node.graph), text(node.name));
            };
        logical_scene->nodeAddRequested =
            [this](const FramePlanNodeKey &after, bool task) {
                if (task) {
                    if (task_insert_action) task_insert_action();
                    else showStatus(owner.tr("Adding a compute task is unavailable."));
                    return;
                }
                if (!pass_insert_action) {
                    showStatus(owner.tr("Adding a pass is unavailable."));
                    return;
                }
                pass_insert_action(text(after.graph), text(after.name));
            };
        logical_scene->placementChanged = [this] { writePlacement(); };
        logical = new FramePlanGraphicsView(logical_scene, logical_splitter);
        logical->setObjectName(QStringLiteral("pelican.framePlanLogicalView"));
        logical->setToolTip(owner.tr("Shift-drag: connect resources"));
        logical->setRenderHint(QPainter::Antialiasing, true);
        logical->setDragMode(QGraphicsView::RubberBandDrag);
        logical->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        logical->setResizeAnchor(QGraphicsView::AnchorViewCenter);
        // The canvas ground and its grid are the theme's: NodeCanvasView
        // paints them, so a theme switch does not have to find this line.
        logical->setFocusPolicy(Qt::StrongFocus);
        // "Fit view" is offered by the scene's context menu, but only the view
        // can carry it out.
        logical_scene->fitViewRequested = [this] { logical->fitView(); };
        // N-4: the canvas menu's two placement entries live as long as the
        // panel does and are named, so `action` fires them by name without
        // opening a popup -- which a command that owes a reply cannot do.
        auto *auto_layout = new QAction(QStringLiteral("Auto layout"), &owner);
        auto_layout->setObjectName(
            QStringLiteral("pelican.framePlan.autoLayout"));
        owner.addAction(auto_layout);
        QObject::connect(auto_layout, &QAction::triggered, &owner,
                         [this] { logical_scene->autoLayout(); });
        auto *fit_view = new QAction(QStringLiteral("Fit view"), &owner);
        fit_view->setObjectName(QStringLiteral("pelican.framePlan.fitView"));
        fit_view->setShortcut(QKeySequence{Qt::Key_F});
        // The key itself belongs to the view, which knows that "F" typed into
        // an in-place editor is a letter. The entry carries the binding so the
        // menu can show it; the context is the panel itself, which never holds
        // the keyboard, so the shortcut cannot take a keystroke off an editor.
        fit_view->setShortcutContext(Qt::WidgetShortcut);
        owner.addAction(fit_view);
        QObject::connect(fit_view, &QAction::triggered, &owner,
                         [this] { logical->fitView(); });
        logical_scene->setMenuActions(auto_layout, fit_view);
        // N-8: the two insert entries, named for the same reason the placement
        // ones are -- a command that owes a reply cannot open a popup.
        auto *add_pass = new QAction(
            QStringLiteral("Add fullscreen pass..."), &owner);
        add_pass->setObjectName(
            QStringLiteral("pelican.framePlan.addFullscreenPass"));
        owner.addAction(add_pass);
        QObject::connect(add_pass, &QAction::triggered, &owner, [this] {
            const auto anchor = logical_scene->insertAnchor();
            if (!anchor) {
                showStatus(owner.tr(
                    "Select the node the new pass should follow first."));
                return;
            }
            if (logical_scene->nodeAddRequested)
                logical_scene->nodeAddRequested(*anchor, false);
        });
        auto *add_task = new QAction(
            QStringLiteral("Add compute task..."), &owner);
        add_task->setObjectName(
            QStringLiteral("pelican.framePlan.addComputeTask"));
        owner.addAction(add_task);
        QObject::connect(add_task, &QAction::triggered, &owner, [this] {
            const auto anchor = logical_scene->insertAnchor();
            if (logical_scene->nodeAddRequested)
                logical_scene->nodeAddRequested(
                    anchor ? *anchor
                           : FramePlanNodeKey{model ? model->graph
                                                    : std::string{},
                                              std::string{}},
                    true);
        });
        logical_scene->setInsertActions(add_pass, add_task);
        // Q-12: a task a render.compute node made is edited in the Graph
        // tab. Named like the others; offered on a right-click over such a
        // task only.
        auto *open_in_graph = new QAction(
            QStringLiteral("Open in Graph"), &owner);
        open_in_graph->setObjectName(
            QStringLiteral("pelican.framePlan.openInGraph"));
        owner.addAction(open_in_graph);
        QObject::connect(open_in_graph, &QAction::triggered, &owner,
                         [this] { openInGraph(logical_scene->insertAnchor()); });
        logical_scene->setOpenInGraphAction(open_in_graph);
        logical_scene->openInGraphOffered =
            [this](const FramePlanNodeKey &node) {
                return graphNodeOf(node) != nullptr;
            };
        // F-1: named like the insert entries, and offered on a right-click
        // over open canvas and on the toolbar.
        auto *add_target = new QAction(
            QStringLiteral("New render target..."), &owner);
        add_target->setObjectName(
            QStringLiteral("pelican.framePlan.newRenderTarget"));
        owner.addAction(add_target);
        QObject::connect(add_target, &QAction::triggered, &owner,
                         [this] { openNewRenderTarget(); });
        QObject::connect(new_target, &QPushButton::clicked, add_target,
                         &QAction::trigger);
        logical_scene->setRenderTargetAction(add_target);
        // G-1: named like the others; after the node right-clicked or picked,
        // at the start of the graph otherwise.
        auto *add_material = new QAction(
            QStringLiteral("Add material pass..."), &owner);
        add_material->setObjectName(
            QStringLiteral("pelican.framePlan.addMaterialPass"));
        owner.addAction(add_material);
        QObject::connect(add_material, &QAction::triggered, &owner,
                         [this] { openMaterialPassForm(); });
        QObject::connect(new_material_pass, &QPushButton::clicked,
                         add_material, &QAction::trigger);
        logical_scene->setMaterialPassAction(add_material);
        logical_scene->resourceEditRequested =
            [this](const FramePlanNodeKey &resource) {
                openRenderTargetForm(resource.name);
            };
        logical_scene->resourceRemoveRequested =
            [this](const FramePlanNodeKey &resource) {
                submitBuilt([this, name = resource.name] {
                    return framePlanRenderTargetRemoveRequest(authoring, name);
                });
            };
        auto *leave_group = new QAction(&owner);
        leave_group->setObjectName(
            QStringLiteral("pelican.framePlanLeaveGroup"));
        leave_group->setShortcut(QKeySequence{Qt::Key_Escape});
        leave_group->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        owner.addAction(leave_group);
        QObject::connect(leave_group, &QAction::triggered, &owner,
                         [this] {
                             // Q-4. Escape belongs to whatever is open
                             // innermost. A band being retyped is inside the
                             // connection gesture, which is inside the group;
                             // this shortcut used to take the key off the
                             // editor and leave the group instead, so there
                             // was no way at all to abandon a typed value.
                             if (logical_scene->editingInline()) {
                                 logical_scene->cancelInlineEdit();
                                 return;
                             }
                             if (!logical_scene->cancelConnectionGesture()) logical_scene->leaveGroup();
                         });
        logical->setZoomObserver(
            [this](qreal scale, const QString &boundary) {
                zoom_status->setText(
                    boundary.isEmpty()
                        ? owner.tr("Zoom: %1%").arg(qRound(scale * 100.0))
                        : owner.tr("Zoom: %1% (%2 limit)")
                              .arg(qRound(scale * 100.0))
                              .arg(boundary));
            });
        logical_details = makeTree(
            {owner.tr("Order"), owner.tr("Pass / detail"), owner.tr("Kind"),
             owner.tr("Inputs"), owner.tr("Outputs")},
            logical_splitter);
        logical_details->setObjectName(
            QStringLiteral("pelican.framePlanLogicalDetails"));
        logical_splitter->setStretchFactor(0, 3);
        logical_splitter->setStretchFactor(1, 2);
        logical_splitter->setSizes({600, 360});
        passes = makeTree(
            {owner.tr("Order"), owner.tr("Pass / detail"), owner.tr("Kind"),
             owner.tr("Inputs"), owner.tr("Outputs")},
            tabs);
        resources = makeTree(
            {owner.tr("Resource"), owner.tr("Format"), owner.tr("Writers"),
             owner.tr("Readers")},
            tabs);
        physical = makeTree(
            {owner.tr("Physical item"), owner.tr("Kind / state"),
             owner.tr("Members / value"), owner.tr("Detail")},
            tabs);
        decisions = makeTree(
            {owner.tr("Subject"), owner.tr("Decision id"),
             owner.tr("Selected"), owner.tr("Detail")},
            tabs);
        backends = makeTree(
            {owner.tr("Candidate"), owner.tr("Status"),
             owner.tr("Endpoint / subject"),
             owner.tr("Failure / diagnostic id"), owner.tr("Detail")},
            tabs);
        barriers = makeTree(
            {owner.tr("#"), owner.tr("From"), owner.tr("To"),
             owner.tr("Resource"), owner.tr("Hazard")},
            tabs);
        materials = makeTree(
            {owner.tr("Route"), owner.tr("Target pass"), owner.tr("Phase"),
             owner.tr("Contract"), owner.tr("Shader contract")},
            tabs);
        raw_json = new QPlainTextEdit(tabs);
        raw_json->setObjectName(QStringLiteral("pelican.framePlanRawJson"));
        raw_json->setReadOnly(true);
        raw_json->setLineWrapMode(QPlainTextEdit::NoWrap);
        addToolTab(*tabs, logical_splitter, QStringLiteral("logical-graph"),
                   owner.tr("Logical graph"));
        addToolTab(*tabs, passes, QStringLiteral("passes"),
                   owner.tr("Passes"));
        addToolTab(*tabs, resources, QStringLiteral("resources"),
                   owner.tr("Resources"));
        addToolTab(*tabs, physical, QStringLiteral("physical-plan"),
                   owner.tr("Physical plan"));
        addToolTab(*tabs, decisions, QStringLiteral("decisions"),
                   owner.tr("Decisions"));
        addToolTab(*tabs, backends, QStringLiteral("backends"),
                   owner.tr("Backends"));
        addToolTab(*tabs, barriers, QStringLiteral("barriers"),
                   owner.tr("Barriers"));
        addToolTab(*tabs, materials, QStringLiteral("materials"),
                   owner.tr("Materials"));
        addToolTab(*tabs, raw_json, QStringLiteral("raw-json"),
                   owner.tr("Raw JSON"));
        layout->addWidget(tabs, 1);

        QObject::connect(tabs, &QTabWidget::currentChanged, &owner,
                         [this](int) { refreshRawJson(); });
        QObject::connect(refresh, &QPushButton::clicked, &owner,
                         [this] { requestRefresh(); });
        QObject::connect(logical_scene, &QGraphicsScene::selectionChanged,
                         &owner, [this] { populateLogicalDetails(); },
                         Qt::QueuedConnection);
        QObject::connect(filter, &QLineEdit::textChanged, &owner,
                         [this](const QString &needle) {
                             applyFilter(*passes, needle);
                             applyFilter(*resources, needle);
                             applyFilter(*physical, needle);
                             applyFilter(*decisions, needle);
                             applyFilter(*backends, needle);
                             applyFilter(*barriers, needle);
                             applyFilter(*materials, needle);
                         });
        QObject::connect(target, &QComboBox::currentTextChanged, &owner,
                         [this](const QString &) {
                             if (model) {
                                 populate();
                             }
                         });
        QObject::connect(depth, qOverload<int>(&QSpinBox::valueChanged),
                         &owner, [this](int) {
                             if (model) {
                                 populate();
                             }
                         });
        QObject::connect(&viewport, &EmbeddedViewport::engineRpcBecameAvailable,
                         &owner, [this] {
                             pending_request = 0;
                             unpublished_plan_retries = 0;
                             pending_gpu_timing_request = 0;
                             model.reset();
                             clearTrees();
                             logical_scene->setGpuTiming({});
                             // N-8: a different engine is a different project.
                             // What it says about who may be edited, and where
                             // it drew things last time, are both its own -- so
                             // they are asked for again rather than carried in.
                             authoring = {};
                             task_authoring = {};
                             pending_authoring_request = 0;
                             pending_task_request = 0;
                             pending_edit_request = 0;
                             pending_ticket_request = 0;
                             pending_ticket.clear();
                             submitted_method.clear();
                             submitted_target.clear();
                             pending_target_selection.clear();
                             pending_form_target.clear();
                             pending_new_target_form = false;
                             if (render_target_form) render_target_form->hide();
                             pending_material_form = false;
                             pending_material_pass.reset();
                             material_context_stale = false;
                             material_targets_made.clear();
                             if (material_pass_form) material_pass_form->hide();
                             placement_loaded = false;
                             placement_graph.clear();
                             requestRefresh();
                             gpu_timing_reportable = true;
                             requestGpuTiming();
                             updateGpuTimingPoll();
                         });
        QObject::connect(
            &viewport, &EmbeddedViewport::engineRpcBecameUnavailable, &owner,
            [this](const QString &message) { showUnavailable(message); });
        QObject::connect(
            &viewport, &EmbeddedViewport::engineProcessExitedWithFailure,
            &owner, [this](const QString &fatal_error_line) {
                if (!fatal_error_line.isEmpty()) {
                    showEngineFailure(fatal_error_line);
                }
            });
        QObject::connect(
            &viewport, &EmbeddedViewport::inspectorRpcSucceeded, &owner,
            [this](qint64 request_id, const QByteArray &result_json) {
                receiveRpcResult(request_id, result_json);
            });
        // F-5: a refused reload_shaders is read by its error_code.
        QObject::connect(
            &viewport, &EmbeddedViewport::inspectorRpcRejected, &owner,
            [this](qint64 request_id, const QString &error_code,
                   const QString &message) {
                (void)reload_follower->reject(request_id, error_code, message);
            });
        QObject::connect(
            &viewport, &EmbeddedViewport::inspectorRpcFailed, &owner,
            [this](qint64 request_id, const QString &message) {
                receiveFailure(request_id, message);
            });

        populateLogicalDetails();

        if (viewport.rpcReady()) {
            requestRefresh();
            gpu_timing_reportable = true;
            requestGpuTiming();
            updateGpuTimingPoll();
        } else {
            showUnavailable(owner.tr(
                "pelican_player is not running. Open a project to start the "
                "engine viewport."));
        }
    }

    void clearTrees() {
        logical_scene->resetGraph();
        populateLogicalDetails();
        {
            const QSignalBlocker blocker{target};
            target->clear();
            target->setCurrentIndex(-1);
        }
        target->setEnabled(false);
        passes->clear();
        resources->clear();
        physical->clear();
        decisions->clear();
        backends->clear();
        barriers->clear();
        materials->clear();
        raw_json->clear();
        raw_json_stale = true;
        tabs->setEnabled(false);
    }

    QStringList tabOrder() const {
        QStringList order;
        order.reserve(tabs->count());
        for (int index = 0; index < tabs->count(); ++index) {
            order.push_back(toolTabId(tabs->widget(index)));
        }
        return order;
    }

    bool reorderTabs(const QStringList &order) {
        if (order.size() != tabs->count()) {
            return false;
        }
        const QStringList current_order = tabOrder();
        for (const QString &tab_id : order) {
            if (tab_id.isEmpty() || order.count(tab_id) != 1 ||
                !current_order.contains(tab_id)) {
                return false;
            }
        }

        for (int target_index = 0; target_index < order.size();
             ++target_index) {
            int current_index = target_index;
            while (current_index < tabs->count() &&
                   toolTabId(tabs->widget(current_index)) !=
                       order[target_index]) {
                ++current_index;
            }
            if (current_index >= tabs->count()) {
                return false;
            }
            if (current_index != target_index) {
                tabs->tabBar()->moveTab(current_index, target_index);
            }
        }
        return tabOrder() == order;
    }

    void showUnavailable(const QString &reason) {
        pending_request = 0;
        pending_gpu_timing_request = 0;
        gpu_timing_poll->stop();
        model.reset();
        clearTrees();
        logical_scene->setGpuTiming(FramePlanGpuTimingSnapshot{
            .received = true,
            .reason = reason.toStdString(),
        });
        refresh->setEnabled(false);
        status->setStyleSheet(QStringLiteral("color: #b36b00;"));
        showStatus(owner.tr("Frame plan unavailable: %1").arg(oneLine(reason)),
                   owner.tr("Frame plan unavailable: %1 Start or restart "
                            "pelican_player, then press Refresh.")
                       .arg(reason));
    }

    void showFailure(const QString &message) {
        requested_hit_group.clear();
        refresh->setEnabled(viewport.rpcReady());
        tabs->setEnabled(model.has_value());
        status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        showStatus(owner.tr("Frame plan refresh failed: %1").arg(oneLine(message)),
                   model
                       ? owner.tr("Frame plan refresh failed: %1 The previous "
                                  "snapshot is still displayed.")
                             .arg(message)
                       : owner.tr("Frame plan refresh failed: %1").arg(message));
    }

    void showEngineFailure(const QString &fatal_error_line) {
        pending_request = 0;
        pending_gpu_timing_request = 0;
        gpu_timing_poll->stop();
        model.reset();
        clearTrees();
        refresh->setEnabled(false);
        status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        showStatus(oneLine(fatal_error_line), fatal_error_line);
    }

    // N-7 rule 2: the panel has one status row. It is one line, it carries
    // the long form as its tooltip, and an empty line takes the row with it.
    void showStatus(const QString &line, const QString &detail = {}) {
        setPanelNotice(status, line, detail.isEmpty() ? line : detail);
    }

    // --- F-3. A shader that failed to reload is marked on the node that
    // reads it. The files are the ones the Open button would open.
    void followShaderSources() {
        NodeShaderSources sources;
        if (model && project_resolver) {
            for (const auto &node : model->nodes) {
                for (const auto &stage : node.shader_resolution.stages) {
                    if (!stage.source_open_ref) continue;
                    try {
                        sources[node.name].push_back(
                            Pelican::materializeShaderSourceOpenReference(
                                *project_resolver, *stage.source_open_ref));
                    } catch (const std::exception &) {
                        // Not a file this project holds: nothing to watch.
                    }
                }
            }
        }
        reload_follower->setSources(std::move(sources));
    }

    void applyShaderReloadMarks(const ShaderReloadMarks &marks,
                                const ShaderReloadReading &reading,
                                bool after_change) {
        (void)after_change;
        std::map<std::string, QString> nodes;
        for (const auto &[node, line] : marks.nodes)
            nodes.emplace(node, text(line));
        logical_scene->setShaderErrorMarks(std::move(nodes));
        // The panel's own status line: it is where the marks are. F-5: a
        // shut file watch is no longer news here -- Studio asks the engine
        // itself, and says so only when the engine refuses (see below).
        if (!marks.unmatched.empty()) {
            showReloadNotice(owner.tr("Shader reload failed: %1")
                                 .arg(text(marks.unmatched.front())),
                             text(reading.last_error));
        } else if (reading.last_error.empty()) {
            clearReloadNotice();
        }
    }

    // --- F-5. The answer to reload_shaders, which a write to a watched
    // shader file sends.
    void applyShaderReloadOutcome(const ShaderReloadOutcome &outcome) {
        using Kind = ShaderReloadOutcome::Kind;
        switch (outcome.kind) {
        case Kind::committed:
            clearReloadNotice();
            logical_scene->setShaderErrorMarks({});
            if (shader_reload_log_action) shader_reload_log_action({});
            break;
        case Kind::unchanged:
            break;
        case Kind::failed: {
            // Marked now from the engine's own words; the status read that
            // follows says the same thing from reload status.
            ShaderReloadReading reading;
            reading.reported = true;
            reading.last_error = outcome.message.toStdString();
            applyShaderReloadMarks(
                shaderReloadMarks(reading, reload_follower->sources()),
                reading, true);
            if (shader_reload_log_action)
                shader_reload_log_action(outcome.message);
            break;
        }
        case Kind::unavailable:
            showReloadNotice(
                owner.tr("A shader file changed; this engine does not "
                         "reload shaders (%1).")
                    .arg(unavailableReason(outcome.message)),
                outcome.message);
            break;
        case Kind::error:
            if (!outcome.message.isEmpty())
                showReloadNotice(owner.tr("Could not ask the engine to "
                                          "reload shaders: %1")
                                     .arg(oneLine(outcome.message)),
                                 outcome.message);
            break;
        }
    }

    // "reload_shaders: this engine does not reload shaders (replay)" ->
    // "replay".
    static QString unavailableReason(const QString &message) {
        const auto open = message.lastIndexOf(QLatin1Char('('));
        const auto close = message.lastIndexOf(QLatin1Char(')'));
        if (open >= 0 && close > open)
            return message.mid(open + 1, close - open - 1);
        return message;
    }

    void showReloadNotice(const QString &line, const QString &detail) {
        shown_reload_notice = line;
        showStatus(line, detail);
    }

    // Only a line a shader reload put there: the status row also says what
    // the panel is showing.
    void clearReloadNotice() {
        if (!shown_reload_notice.isEmpty() &&
            status->text() == shown_reload_notice)
            showStatus({});
        shown_reload_notice.clear();
    }

    // --- N-8. Every canvas gesture ends here: one authoring RPC, or one line
    // that names the pass and says why there is no RPC to send.

    // The authored task behind a node, or empty when the node is a pass.
    std::string authoredTask(const std::string &name) const {
        if (!model) return {};
        const auto found = std::ranges::find(model->nodes, name,
                                             &FramePlanNode::name);
        if (found == model->nodes.end() || found->kind != "compute") return {};
        return found->authored_name;
    }

    void connectEditSignals() {
        if (edit_signals_connected) return;
        edit_signals_connected = true;
        if (edit.result)
            edit.result(&owner, [this](qint64 id, const QByteArray &bytes) {
                receiveEditResult(id, bytes);
            });
        if (edit.failure)
            edit.failure(&owner, [this](qint64 id, const QString &message) {
                receiveEditFailure(id, message);
            });
    }

    // A panel nobody is looking at asks the engine nothing. The Frame Plan has
    // always been a snapshot the user refreshes; N-8 added two reads to it, and
    // they follow the same rule -- a tab behind another one must not put
    // traffic on the editor channel, or take work from the panel in front.
    // They are asked for again the moment this one is shown.
    void requestAuthoringContext(bool even_hidden = false) {
        if (!edit.requestAuthoringContext || pending_authoring_request != 0 ||
            (!owner.isVisible() && !even_hidden))
            return;
        QString error;
        pending_authoring_request = edit.requestAuthoringContext(&error);
        if (pending_authoring_request == 0 && !error.isEmpty())
            showStatus(oneLine(error), error);
    }

    void requestTaskAuthoring(bool even_hidden = false) {
        if (!edit.requestTaskAuthoring || pending_task_request != 0 ||
            (!owner.isVisible() && !even_hidden))
            return;
        QString error;
        pending_task_request = edit.requestTaskAuthoring(&error);
        if (pending_task_request == 0 && !error.isEmpty())
            showStatus(oneLine(error), error);
    }

    // --- Q-13. Every read and every commit of the document says which digest
    // it saw; a digest another panel saw moves this panel's context.
    void publishDigest(const std::string &digest) {
        if (digest_hub) digest_hub->publish(digest, &owner);
    }

    // Another panel read or committed the document at `digest`. A context
    // built from any other revision would send a base digest the engine no
    // longer holds, so both reads are asked for again.
    void documentMoved(const std::string &digest) {
        if (authoring.received && authoring.source_digest != digest) {
            if (pending_authoring_request != 0) authoring_reread = true;
            else requestAuthoringContext();
        }
        if (task_authoring.received && task_authoring.source_digest != digest) {
            if (pending_task_request != 0) task_reread = true;
            else requestTaskAuthoring();
        }
    }

    static std::string errorCode(const Json &response) {
        const auto error = response.find("error");
        return error != response.end() && error->is_object()
                   ? error->value("code", std::string{})
                   : std::string{};
    }

    // The engine refused the edit in flight because the document is not the
    // one its context was read from. Once per edit, the context is read again
    // and the edit is built again from it (resumeRetry). The bytes that were
    // refused are never sent a second time: they name the old revision.
    bool retryAfterExternalModification(const Json &response) {
        if (errorCode(response) != "external_modification" || !submitted_build ||
            submitted_retried)
            return false;
        retry_pending = true;
        status->setStyleSheet({});
        showStatus(owner.tr("The document changed since this panel read it; "
                            "reading it again..."));
        if (pending_authoring_request != 0) authoring_reread = true;
        else requestAuthoringContext(true);
        if (edit.requestTaskAuthoring) {
            if (pending_task_request != 0) task_reread = true;
            else requestTaskAuthoring(true);
        }
        return true;
    }

    void resumeRetry() {
        if (!retry_pending || pending_authoring_request != 0 ||
            pending_task_request != 0)
            return;
        retry_pending = false;
        auto build = std::move(submitted_build);
        submitted_build = {};
        if (!build || !authoring.received) {
            offerReload(owner.tr("The document changed outside this panel, and "
                                 "its context could not be read again."));
            return;
        }
        const auto request = build();
        // G-1: the targets Save wanted are there after all; the pass is next.
        if (!request.sendable() && request.refusal.empty() &&
            request.method == "edit_render_targets" && pending_material_pass) {
            submitMaterialPass();
            return;
        }
        if (submit(request)) {
            submitted_build = std::move(build);
            submitted_retried = true;
        } else {
            pending_material_pass.reset();
        }
    }

    // The second refusal: say so, and offer to read the document again.
    void offerReload(const QString &message) {
        pending_material_pass.reset();
        material_context_stale = false;
        status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        showStatus(owner.tr("The document changed outside this panel. Press "
                            "Reload to read it again."),
                   message);
        reload_authoring->show();
    }

    void reloadAuthoring() {
        reload_authoring->hide();
        status->setStyleSheet({});
        showStatus(owner.tr("Reading the render authoring context..."));
        if (pending_authoring_request != 0) authoring_reread = true;
        else requestAuthoringContext(true);
        if (pending_task_request != 0) task_reread = true;
        else requestTaskAuthoring(true);
        requestRefresh();
    }

    // One gesture, kept as the function that builds its request from the
    // context, so a refusal for a moved document can build it once more.
    void submitBuilt(std::function<FramePlanEditRequest()> build) {
        if (!submit(build())) return;
        submitted_build = std::move(build);
        submitted_retried = false;
    }

    bool submit(const FramePlanEditRequest &request) {
        if (!request.sendable()) {
            const QString reason = text(request.refusal.empty()
                                            ? std::string{"Nothing to change."}
                                            : request.refusal);
            showStatus(request.line.empty() ? oneLine(reason)
                                            : text(request.line),
                       reason);
            return false;
        }
        if (pending_edit_request != 0 || !pending_ticket.isEmpty()) {
            showStatus(owner.tr("Wait for the current change to finish."));
            return false;
        }
        const auto method = text(request.method);
        const auto sender =
            request.method == "edit_preset_pass"     ? edit.editPresetPass
            : request.method == "remove_authored_pass" ? edit.removeAuthoredPass
            : request.method == "edit_compute_task"  ? edit.editComputeTask
            : request.method == "remove_compute_task" ? edit.removeComputeTask
            : request.method == "edit_render_targets" ? edit.editRenderTargets
            : request.method == "add_authored_pass"   ? edit.addAuthoredPass
                                                      : nullptr;
        if (!sender) {
            showStatus(owner.tr("Canvas editing is unavailable: %1").arg(method));
            return false;
        }
        QJsonObject params;
        try {
            params = exactJsonRpcObject(request.params);
        } catch (const std::exception &error) {
            const QString message = QString::fromUtf8(error.what());
            showStatus(oneLine(message), message);
            return false;
        }
        QString error;
        pending_edit_request = sender(params, &error);
        if (pending_edit_request == 0) {
            showStatus(oneLine(error), error);
            return false;
        }
        // Q-13: an edit sent without its builder is not rebuilt; submitBuilt
        // puts the builder back once this returns.
        submitted_build = {};
        submitted_retried = false;
        reload_authoring->hide();
        submitted_method = request.method;
        submitted_target = submittedTargetName(request);
        status->setStyleSheet({});
        showStatus(owner.tr("%1: submitting...").arg(method));
        return true;
    }

    void requestTicketResult() {
        if (!edit.requestEditResult || pending_ticket.isEmpty() ||
            pending_ticket_request != 0)
            return;
        QString error;
        pending_ticket_request = edit.requestEditResult(pending_ticket, &error);
        if (pending_ticket_request == 0) {
            pending_ticket.clear();
            showStatus(oneLine(error), error);
        }
    }

    bool receiveEditResult(qint64 request_id, const QByteArray &bytes) {
        if (request_id != 0 && request_id == pending_authoring_request) {
            pending_authoring_request = 0;
            try {
                authoring = framePlanAuthoringFromContext(
                    Json::parse(bytes.constData(),
                                bytes.constData() + bytes.size()));
            } catch (const std::exception &) {
                authoring = {};
            }
            // Q-10: the catalog names each task's document; only the
            // context names the root one.
            task_authoring.root_reference = authoring.source_reference;
            if (authoring.received) publishDigest(authoring.source_digest);
            if (model) populate();
            // Q-13: the document moved while this read was on its way.
            if (std::exchange(authoring_reread, false))
                requestAuthoringContext(true);
            if (retry_pending) {
                resumeRetry();
                return true;
            }
            if (std::exchange(pending_new_target_form, false) &&
                authoring.received)
                openNewRenderTarget();
            if (std::exchange(pending_material_form, false) &&
                authoring.received)
                openMaterialPassForm();
            if (pending_material_pass) {
                if (std::exchange(material_context_stale, false))
                    requestAuthoringContext(true);
                else
                    submitMaterialPass();
            }
            if (!pending_form_target.empty()) {
                const auto name = std::exchange(pending_form_target, {});
                if (authoring.findRenderTarget(name) != nullptr)
                    openRenderTargetForm(name);
                else
                    showStatus(text(name + " is not a render target in the "
                                           "current authoring context."));
            }
            return true;
        }
        if (request_id != 0 && request_id == pending_task_request) {
            pending_task_request = 0;
            try {
                task_authoring = framePlanTaskAuthoringFromCatalog(
                    Json::parse(bytes.constData(),
                                bytes.constData() + bytes.size()));
            } catch (const std::exception &) {
                task_authoring = {};
            }
            task_authoring.root_reference = authoring.source_reference;
            if (task_authoring.received) publishDigest(task_authoring.source_digest);
            if (model) populate();
            if (std::exchange(task_reread, false)) requestTaskAuthoring(true);
            resumeRetry();
            return true;
        }
        if (request_id != 0 && request_id == pending_edit_request) {
            pending_edit_request = 0;
            consumeSubmission(bytes);
            return true;
        }
        if (request_id != 0 && request_id == pending_ticket_request) {
            pending_ticket_request = 0;
            consumeTicket(bytes);
            return true;
        }
        return false;
    }

    static QString responseError(const Json &response) {
        const auto code = response.value("code", std::string{});
        const auto message = response.value("message", std::string{});
        if (!message.empty())
            return text(code.empty() ? message : code + ": " + message);
        return text(response.dump());
    }

    // An edit the engine did not take. Q-13: a document that moved under it
    // is read again and the edit rebuilt once; the second time, Reload.
    void refuse(const Json &response) {
        if (retryAfterExternalModification(response)) return;
        const bool moved = errorCode(response) == "external_modification";
        const QString message = rejectionText(response) + materialFailureNote();
        submitted_build = {};
        if (moved) {
            offerReload(message);
            return;
        }
        status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        showStatus(oneLine(message), message);
    }

    void consumeSubmission(const QByteArray &bytes) {
        try {
            const Json response = Json::parse(
                bytes.constData(), bytes.constData() + bytes.size());
            const auto status_text = response.value("status", std::string{});
            if (status_text != "accepted" || !response.contains("ticket") ||
                !response.at("ticket").is_string()) {
                refuse(response);
                return;
            }
            pending_ticket = text(response.at("ticket").get<std::string>());
            showStatus(owner.tr("accepted; waiting for the engine..."));
            QTimer::singleShot(0, &owner, [this] { requestTicketResult(); });
        } catch (const std::exception &error) {
            const QString message = QString::fromUtf8(error.what());
            status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
            showStatus(oneLine(message), message);
        }
    }

    void consumeTicket(const QByteArray &bytes) {
        try {
            const Json response = Json::parse(
                bytes.constData(), bytes.constData() + bytes.size());
            const auto status_text = response.value("status", std::string{});
            if (status_text == "pending") {
                QTimer::singleShot(25, &owner,
                                   [this] { requestTicketResult(); });
                return;
            }
            pending_ticket.clear();
            if (status_text != "committed" ||
                !response.value("committed", false)) {
                refuse(response);
                return;
            }
            submitted_build = {};
            // Q-13: every other panel learns the revision this commit made.
            publishDigest(response.value("source_digest", Json::object())
                              .value("hex", std::string{}));
            status->setStyleSheet({});
            if (submitted_method == "edit_render_targets" && !pending_material_pass)
                pending_target_selection = submitted_target;
            if (submitted_method == "edit_render_targets" && !pending_material_pass &&
                !created_target.empty() && created_target == submitted_target) {
                const auto made = std::exchange(created_target, {});
                if (render_target_created_action)
                    render_target_created_action(text(made));
            }
            // G-1: the pass is added from the context that names the targets;
            // a reply already in flight was read before they were made.
            if (submitted_method == "edit_render_targets" && pending_material_pass) {
                material_context_stale = pending_authoring_request != 0;
                requestAuthoringContext(true);
            }
            if (submitted_method == "add_authored_pass") material_targets_made.clear();
            // G-4: the change is a setting the engine reads once at start
            // (material_route_default). Restart the engine on the same
            // project; the Frame Plan comes back when it is ready again.
            if (response.value("restart_required", false)) {
                const QString reason = owner.tr(
                    "Restarting the engine to apply the material route setting...");
                status->setStyleSheet({});
                showStatus(reason);
                if (engine_restart_action) engine_restart_action(reason);
                else viewport.restartEngine(reason);
                return;
            }
            // The refresh first, so its own "requesting..." line does not land
            // on top of the answer the user is waiting for. The summary line
            // replaces this one when the new plan arrives, which is the right
            // order: "committed" is what happened, the summary is what is now.
            requestAuthoringContext();
            requestTaskAuthoring();
            requestRefresh();
            status->setStyleSheet({});
            showStatus(owner.tr("Canvas change committed."));
        } catch (const std::exception &error) {
            pending_ticket.clear();
            const QString message = QString::fromUtf8(error.what());
            status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
            showStatus(oneLine(message), message);
        }
    }

    // G-1. What a refused material pass leaves behind: the targets Save made
    // for it, named, so the user knows they are there. A refused target batch
    // made nothing, and the pass it was for is not sent.
    QString materialFailureNote() {
        if (submitted_method == "edit_render_targets" && pending_material_pass) {
            pending_material_pass.reset();
            material_context_stale = false;
            material_targets_made.clear();
            return owner.tr(" The material pass was not added.");
        }
        if (submitted_method != "add_authored_pass" || material_targets_made.empty())
            return {};
        std::string names;
        for (const auto &name : std::exchange(material_targets_made, {}))
            names += (names.empty() ? "" : ", ") + name;
        return owner.tr(" The render targets made for it stay declared: %1.")
            .arg(text(names));
    }

    MaterialPassForm &materialPassForm() {
        if (material_pass_form == nullptr) {
            material_pass_form = new MaterialPassForm(&owner);
            material_pass_form->setSubmit(
                [this](const MaterialPassFormValues &values) -> QString {
                    if (pending_edit_request != 0 || !pending_ticket.isEmpty() ||
                        pending_material_pass)
                        return owner.tr("Wait for the current change to finish.");
                    const auto targets = materialPassTargetsRequest(authoring, values);
                    if (!targets.refusal.empty()) return text(targets.refusal);
                    if (!targets.sendable()) {
                        const auto pass = materialPassAddRequest(authoring, values);
                        if (!pass.sendable()) return text(pass.refusal);
                        material_targets_made.clear();
                        submitBuilt([this, values] {
                            return materialPassAddRequest(authoring, values);
                        });
                        return {};
                    }
                    pending_material_pass = values;
                    material_context_stale = false;
                    // Q-13: built again from a fresh context after a refusal
                    // for a moved document, and so are the names it makes.
                    submitBuilt([this, values] {
                        material_targets_made.clear();
                        for (const auto index : materialPassNewSlots(authoring, values))
                            material_targets_made.push_back(values.targets[index]);
                        return materialPassTargetsRequest(authoring, values);
                    });
                    if (pending_edit_request == 0) {
                        pending_material_pass.reset();
                        material_targets_made.clear();
                    }
                    return {};
                });
        }
        return *material_pass_form;
    }

    void openMaterialPassForm() {
        if (!model) {
            showStatus(owner.tr("The frame plan has not arrived yet."));
            return;
        }
        if (!authoring.received) {
            pending_material_form = true;
            requestAuthoringContext();
            showStatus(owner.tr("Reading the render authoring context..."));
            return;
        }
        const auto &graph = model->graph;
        std::string insert;
        QString where;
        if (const auto anchor = logical_scene->insertAnchor();
            anchor && anchor->graph == graph) {
            if (const auto after = authoring.anchorAfter(graph, anchor->name)) {
                insert = after->second;
                where = owner.tr("The pass goes after %1 in %2.")
                            .arg(text(anchor->name), text(graph));
            }
        }
        if (insert.empty()) {
            const auto anchors = authoring.anchors.find(graph);
            if (anchors != authoring.anchors.end())
                for (const auto &[position, token] : anchors->second)
                    if (position == 0) insert = token;
            where = owner.tr("The pass goes at the start of %1.").arg(text(graph));
        }
        materialPassForm().present(authoring, graph, insert, where);
    }

    // The second half of Save: the context that names the new targets is here.
    void submitMaterialPass() {
        const auto values = *std::exchange(pending_material_pass, std::nullopt);
        const auto request = materialPassAddRequest(authoring, values);
        if (!request.sendable()) {
            submitted_method = "add_authored_pass";
            const QString message = text(request.refusal) + materialFailureNote();
            status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
            showStatus(oneLine(message), message);
            return;
        }
        submitBuilt([this, values] { return materialPassAddRequest(authoring, values); });
    }

    bool receiveEditFailure(qint64 request_id, const QString &message) {
        if (request_id == 0) return false;
        if (request_id == pending_authoring_request) {
            pending_authoring_request = 0;
            authoring = {};
            authoring_reread = false;
            resumeRetry();
            return true;
        }
        if (request_id == pending_task_request) {
            pending_task_request = 0;
            task_authoring = {};
            task_reread = false;
            resumeRetry();
            return true;
        }
        if (request_id != pending_edit_request &&
            request_id != pending_ticket_request)
            return false;
        pending_edit_request = 0;
        pending_ticket_request = 0;
        pending_ticket.clear();
        status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        showStatus(oneLine(message), message);
        return true;
    }

    // One drag, or one Delete on an edge.
    void changeResourceEdge(const FramePlanNodeKey &consumer,
                            const QStringList &resources, bool connect) {
        if (!model || consumer.graph != model->graph) return;
        std::vector<std::string> names;
        for (const QString &resource : resources)
            names.push_back(resource.toStdString());
        const auto task = authoredTask(consumer.name);
        submitBuilt([this, graph = model->graph, pass = consumer.name, task,
                     names, connect] {
            return task.empty()
                       ? framePlanPassResourceRequest(authoring, graph, pass,
                                                      names, connect)
                       : framePlanTaskResourceRequest(task_authoring, task,
                                                      names, connect);
        });
    }

    void commitInlineField(const FramePlanNodeKey &node, const QString &field,
                           const QString &value) {
        if (!model || node.graph != model->graph) return;
        const auto task = authoredTask(node.name);
        if (!task.empty()) {
            submitBuilt([this, task, dispatch = value.toStdString()] {
                return framePlanTaskDispatchRequest(task_authoring, task,
                                                    dispatch);
            });
            return;
        }
        const auto stage = field.startsWith(QStringLiteral("shader."))
                               ? field.mid(7).toStdString()
                               : std::string{};
        if (stage.empty()) return;
        submitBuilt([this, graph = model->graph, pass = node.name, stage,
                     reference = value.toStdString()] {
            return framePlanPassShaderRequest(authoring, graph, pass, stage,
                                              reference);
        });
    }

    // --- F-1: render targets.

    static std::string submittedTargetName(const FramePlanEditRequest &request) {
        if (request.method != "edit_render_targets") return {};
        const auto ops = request.params.find("ops");
        if (ops == request.params.end() || !ops->is_array() || ops->empty())
            return {};
        const auto &op = ops->back();
        const auto target = op.find("target");
        return target != op.end() && target->is_object()
                   ? target->value("name", std::string{})
                   : std::string{};
    }

    // An edit_render_targets rejection is read by its code; everything else
    // keeps the line it always had.
    QString rejectionText(const Json &response) const {
        const auto error = response.find("error");
        if (error != response.end() && error->is_object() &&
            (submitted_method == "edit_render_targets" ||
             error->value("code", std::string{}).starts_with("render_target_")))
            return text(framePlanRenderTargetRejection(*error,
                                                       authoring.preset_name));
        // G-6: a graph variant that refused the edit is named, whatever the
        // edit was.
        if (error != response.end() && error->is_object() &&
            error->contains("variant"))
            return text(renderAuthoringRefusalLine(*error));
        if (error != response.end() && error->is_object() &&
            submitted_method == "add_authored_pass") {
            // G-4: name the materials no pass would draw, and the fix.
            if (const auto notice = materialRouteUnservedNotice(*error);
                !notice.empty())
                return owner.tr("The engine refused the pass: %1").arg(text(notice));
            return owner.tr("The engine refused the pass: %1")
                .arg(text(error->value("message", error->dump())));
        }
        return responseError(response);
    }

    RenderTargetForm &renderTargetForm() {
        if (render_target_form == nullptr) {
            render_target_form = new RenderTargetForm(&owner);
            render_target_form->setSubmit(
                [this](const RenderTargetFormValues &values) -> QString {
                    const bool create = render_target_form->mode() ==
                                        RenderTargetForm::Mode::create;
                    const auto build = [this, create, values,
                                        editing = render_target_form->editing()] {
                        return create
                                   ? framePlanRenderTargetAddRequest(authoring, values)
                                   : framePlanRenderTargetReplaceRequest(
                                         authoring, editing, values);
                    };
                    const auto request = build();
                    if (!request.sendable()) return text(request.refusal);
                    submitBuilt(build);
                    created_target = create ? values.name : std::string{};
                    return {};
                });
        }
        return *render_target_form;
    }

    void openNewRenderTarget() {
        if (!authoring.received) {
            // Opened as soon as the context that names the vocabulary arrives.
            pending_new_target_form = true;
            requestAuthoringContext();
            showStatus(owner.tr(
                "Reading the render authoring context..."));
            return;
        }
        RenderTargetFormValues values;
        values.name = std::exchange(new_target_name, {});
        renderTargetForm().present(RenderTargetForm::Mode::create, values,
                                   authoring.render_target_formats,
                                   authoring.render_target_usages);
    }

    void openRenderTargetForm(const std::string &name) {
        const auto *row = authoring.findRenderTarget(name);
        if (row == nullptr) {
            // The context may simply be older than the plan on screen: ask
            // for it once and open the form when it answers.
            pending_form_target = name;
            requestAuthoringContext();
            return;
        }
        if (row->editable) {
            renderTargetForm().present(
                RenderTargetForm::Mode::edit,
                renderTargetFormValues(row->authored.empty() ? row->declaration
                                                             : row->authored),
                authoring.render_target_formats,
                authoring.render_target_usages);
            return;
        }
        renderTargetForm().present(
            RenderTargetForm::Mode::read_only,
            renderTargetFormValues(row->declaration),
            authoring.render_target_formats, authoring.render_target_usages,
            owner.tr("Read-only: %1.")
                .arg(text(framePlanRenderTargetOrigin(row->source, name,
                                                      authoring.preset_name))));
    }

    // Q-12. The node that made this Frame Plan node's task, when a
    // render.compute node made it.
    const FramePlanGraphNodeRef *graphNodeOf(const FramePlanNodeKey &node) const {
        if (!model || node.graph != model->graph) return nullptr;
        const auto task = authoredTask(node.name);
        if (task.empty()) return nullptr;
        const auto *row = task_authoring.find(task);
        return row != nullptr && row->graph_node ? &*row->graph_node : nullptr;
    }

    void openInGraph(const std::optional<FramePlanNodeKey> &node) {
        const auto *source = node ? graphNodeOf(*node) : nullptr;
        if (source == nullptr) {
            showStatus(owner.tr("Select a task a graph node made first."));
            return;
        }
        if (!graph_node_open_action) {
            showStatus(owner.tr("Opening the Graph tab is unavailable."));
            return;
        }
        if (!graph_node_open_action(text(source->scope), text(source->node),
                                    text(source->instance))) {
            const auto where =
                source->scope == "root" ? std::string{"the root"} : source->scope;
            showStatus(owner.tr("The Graph tab has no node %1 in %2 yet: refresh it.")
                           .arg(text(source->node), text(where)));
        }
    }

    void removeNode(const FramePlanNodeKey &node) {
        if (!model || node.graph != model->graph) return;
        const auto task = authoredTask(node.name);
        submitBuilt([this, graph = model->graph, pass = node.name, task] {
            return task.empty()
                       ? framePlanPassRemoveRequest(authoring, graph, pass)
                       : framePlanTaskRemoveRequest(task_authoring, task);
        });
    }

    // --- The placement. One member of the document's root `layout`, written
    // through the common canvas's debounce so the two halves travel together.

    void loadPlacement() {
        if (!placement.read || !model) return;
        Json frame_plan;
        try {
            frame_plan = placement.read();
        } catch (const std::exception &) {
            return;
        }
        if (!frame_plan.is_object()) return;
        const auto graph = frame_plan.find(model->graph);
        if (graph == frame_plan.end() || !graph->is_object()) return;
        const auto nodes = graph->find("nodes");
        if (nodes == graph->end() || !nodes->is_object()) return;
        std::map<std::string, QPointF> positions;
        for (const auto &[name, place] : nodes->items()) {
            if (!place.is_object() || !place.contains("x") ||
                !place.contains("y") || !place.at("x").is_number() ||
                !place.at("y").is_number())
                continue;
            positions.emplace(name, QPointF{place.at("x").get<double>(),
                                            place.at("y").get<double>()});
        }
        if (positions.empty()) return;
        logical_scene->setPlacement(model->graph, positions);
    }

    void writePlacement() {
        if (!model) return;
        if (!placement.write) {
            showStatus(owner.tr(
                "Node positions are not saved: this panel has no document to "
                "write them to."));
            return;
        }
        Json frame_plan;
        try {
            frame_plan = placement.read ? placement.read() : Json::object();
        } catch (const std::exception &) {
            frame_plan = Json::object();
        }
        if (!frame_plan.is_object()) frame_plan = Json::object();
        auto &graph = frame_plan[model->graph];
        if (!graph.is_object()) graph = Json::object();
        auto nodes = Json::object();
        for (const auto &[name, position] :
             logical_scene->placement(model->graph))
            nodes[name] = {{"x", position.x()}, {"y", position.y()}};
        if (nodes.empty()) graph.erase("nodes");
        else graph["nodes"] = std::move(nodes);
        // A graph the canvas has nothing to say about at all leaves the
        // member rather than writing an empty object into the document.
        if (graph.empty()) frame_plan.erase(model->graph);
        QString reason;
        if (!placement.write(frame_plan, &reason)) {
            const QString message =
                reason.isEmpty()
                    ? owner.tr("Node positions are not saved for this project.")
                    : reason;
            showStatus(oneLine(message), message);
        }
    }

    void requestRefresh() {
        if (pending_request != 0) {
            return;
        }
        if (!viewport.rpcReady()) {
            showUnavailable(owner.tr("the engine RPC connection is not ready."));
            return;
        }

        QString error;
        pending_request = viewport.requestRpc(
            QStringLiteral("get_frame_plan"), QJsonObject{}, &error);
        if (pending_request == 0) {
            showFailure(error);
            return;
        }
        refresh->setEnabled(false);
        status->setStyleSheet({});
        showStatus(owner.tr("Requesting the current frame plan..."));
    }

    void updateGpuTimingPoll() {
        if (gpu_timing_reportable && viewport.rpcReady() &&
            owner.isVisible()) {
            if (!gpu_timing_poll->isActive()) gpu_timing_poll->start();
            return;
        }
        gpu_timing_poll->stop();
    }

    void requestGpuTiming() {
        if (pending_gpu_timing_request != 0 ||
            !viewport.rpcReady()) {
            return;
        }
        QString error;
        pending_gpu_timing_request = viewport.requestRpc(
            QStringLiteral("get_gpu_timing"), QJsonObject{}, &error);
        if (pending_gpu_timing_request == 0) {
            logical_scene->setGpuTiming(FramePlanGpuTimingSnapshot{
                .received = true,
                .reason = error.toStdString(),
            });
        }
    }

    void receiveRpcResult(qint64 request_id, const QByteArray &result_json) {
        if (reload_follower->receive(request_id, result_json)) return;
        if (request_id == pending_gpu_timing_request) {
            pending_gpu_timing_request = 0;
            consumeGpuTimingResult(result_json);
            return;
        }
        if (request_id != pending_request) {
            return;
        }
        pending_request = 0;
        unpublished_plan_retries = 0;
        owner.receiveResult(result_json);
    }

    void consumeGpuTimingResult(const QByteArray &result_json) {
        try {
            const auto snapshot = parseGpuTimingResult(result_json);
            // A project that did not turn GPU timing on cannot start
            // reporting it while it is loaded, so there is nothing left to
            // ask. The poll is armed again when the engine comes back, which
            // is the only moment that answer can become a different one.
            gpu_timing_reportable = snapshot.enabled && snapshot.supported;
            updateGpuTimingPoll();
            logical_scene->setGpuTiming(snapshot);
        } catch (const std::exception &error) {
            logical_scene->setGpuTiming(FramePlanGpuTimingSnapshot{
                .received = true,
                .reason = std::string{"invalid_response: "} +
                          error.what(),
            });
        }
    }

    void consumeResult(const QByteArray &result_json) {
        try {
            const bool had_model = model.has_value();
            const std::string previous_graph =
                model ? model->graph : std::string{};
            const QString previous_target = target->currentText();
            const bool was_whole_frame = wholeFrameSelected();
            FramePlanModel next = buildFramePlanModel(std::string_view{
                result_json.constData(),
                static_cast<std::size_t>(result_json.size())});
            model = std::move(next);
            const bool same_graph =
                had_model && previous_graph == model->graph;
            std::vector<std::string> targets;
            targets.reserve(model->resources.size());
            for (const auto &resource : model->resources) {
                targets.push_back(resource.name);
            }
            std::ranges::sort(targets);
            targets.erase(std::unique(targets.begin(), targets.end()),
                          targets.end());
            {
                const QSignalBlocker blocker{target};
                target->clear();
                // F-7: the whole frame comes first, and is what a graph the
                // panel has not shown before opens on.
                target->addItem(owner.tr("Whole frame"));
                for (const auto &name : targets) {
                    target->addItem(text(name));
                }
                int selected_index = -1;
                if (!had_model || !same_graph || was_whole_frame) {
                    selected_index = WholeFrameIndex;
                } else if (same_graph && !previous_target.isEmpty()) {
                    selected_index = targetIndex(previous_target);
                }
                // F-1: a target the user just created or renamed is the one
                // the canvas shows next -- unless the canvas is showing the
                // whole frame, which draws that target already, beside the
                // passes it can be joined to.
                if (!pending_target_selection.empty()) {
                    const int created =
                        targetIndex(text(pending_target_selection));
                    if (created >= 0 && selected_index != WholeFrameIndex)
                        selected_index = created;
                    pending_target_selection.clear();
                }
                target->setCurrentIndex(selected_index);
            }
            target->setEnabled(true);
            populate();
            // N-8: the placement the document keeps for the graph now on
            // screen, and the context the bands and the refusals need. Loading
            // the placement once per graph is what keeps a refresh from
            // putting the document's older positions back over a live drag.
            if (!placement_loaded || placement_graph != model->graph) {
                placement_graph = model->graph;
                placement_loaded = true;
                loadPlacement();
            }
            requestAuthoringContext();
            requestTaskAuthoring();
            followShaderSources();
            if (!requested_hit_group.isEmpty()) {
                const auto group = std::exchange(requested_hit_group, QString{});
                owner.openTasksForHitGroup(group);
            }
        } catch (const std::exception &error) {
            showFailure(QString::fromUtf8(error.what()));
        }
    }

    void receiveFailure(qint64 request_id, const QString &message) {
        if (reload_follower->fail(request_id, message)) return;
        if (request_id == pending_gpu_timing_request) {
            pending_gpu_timing_request = 0;
            logical_scene->setGpuTiming(FramePlanGpuTimingSnapshot{
                .received = true,
                .reason = message.toStdString(),
            });
            return;
        }
        if (request_id != pending_request) {
            return;
        }
        pending_request = 0;
        if (message.contains(QStringLiteral("Current frame plan is not registered")) &&
            unpublished_plan_retries < max_unpublished_plan_retries &&
            viewport.rpcReady()) {
            ++unpublished_plan_retries;
            status->setStyleSheet({});
            showStatus(owner.tr("Waiting for the engine's first frame..."));
            QTimer::singleShot(unpublished_plan_retry_ms, &owner, [this] {
                if (viewport.rpcReady()) requestRefresh();
            });
            return;
        }
        showFailure(message);
    }

    // F-7: the Target combo's first entry is the whole frame, not a target;
    // the targets follow it in name order.
    static constexpr int WholeFrameIndex = 0;

    bool wholeFrameSelected() const {
        return target->currentIndex() == WholeFrameIndex;
    }

    int targetIndex(const QString &name) const {
        for (int index = WholeFrameIndex + 1; index < target->count(); ++index)
            if (target->itemText(index) == name) return index;
        return -1;
    }

    void populate() {
        const TreeFillGuard filling{passes,    resources, physical,
                                    decisions, backends,  barriers,
                                    materials, logical_details};
        passes->clear();
        resources->clear();
        physical->clear();
        decisions->clear();
        backends->clear();
        barriers->clear();
        materials->clear();
        raw_json->clear();
        raw_json_stale = true;
        const bool whole_frame = wholeFrameSelected();
        const std::optional<FramePlanNodeKey> selected_target =
            target->currentIndex() < 0 || whole_frame
                ? std::nullopt
                : std::optional<FramePlanNodeKey>{FramePlanNodeKey{
                      model->graph, target->currentText().toStdString()}};
        if (whole_frame)
            logical_scene->populateWholeFrame(*model);
        else
            logical_scene->populate(*model, selected_target, depth->value());
        // The graph, and the group the canvas has descended into, are what the
        // Frame Plan calls a scope. A scope the session has not shown before --
        // which includes the first plan after a project is opened -- is fitted;
        // a scope being returned to keeps the zoom and scroll it was left at.
        logical->showScope(
            QStringLiteral("%1\x1f%2")
                .arg(text(model->graph),
                     logical_scene->property("pelicanCurrentGroupScope")
                         .toString()));
        populateLogicalDetails();
        populatePasses();
        populateResources();
        populatePhysicalPlan();
        populateDecisions();
        populateBackends();
        populateBarriers();
        populateMaterials();
        raw_json_stale = true;
        refreshRawJson();
        tabs->setEnabled(true);
        refresh->setEnabled(viewport.rpcReady());
        applyFilter(*passes, filter->text());
        applyFilter(*resources, filter->text());
        applyFilter(*physical, filter->text());
        applyFilter(*decisions, filter->text());
        applyFilter(*backends, filter->text());
        applyFilter(*barriers, filter->text());
        applyFilter(*materials, filter->text());

        const auto compute_count = static_cast<std::size_t>(std::count_if(
            model->nodes.begin(), model->nodes.end(), [](const FramePlanNode &node) {
                return node.kind == "compute";
            }));
        const QString generation = model->runtime_generation
                                       ? owner.tr("generation %1 | ")
                                             .arg(static_cast<qulonglong>(
                                                 *model->runtime_generation))
                                       : QString{};
        const QString physical_summary = model->physical_plan.available()
                                             ? owner.tr("physical: %1/%2, %3 alias groups, %4 candidates")
                                                   .arg(text(model->physical_plan.planning_profile),
                                                        text(model->physical_plan.planning_endpoint))
                                                   .arg(static_cast<qulonglong>(model->physical_plan.alias_groups.size()))
                                                   .arg(static_cast<qulonglong>(model->physical_plan.alias_candidates.size()))
                                             : owner.tr("physical unavailable (%1)")
                                                   .arg(text(model->physical_plan.unavailable_reason));
        const QString execution_summary =
            model->execution_plan.available()
                ? owner.tr("%1 dependencies")
                      .arg(static_cast<qulonglong>(model->dependencies.size()))
                : owner.tr("execution unavailable (%1)")
                      .arg(text(model->execution_plan.unavailable_reason));
        status->setStyleSheet(
            model->physical_plan.available() &&
                    model->execution_plan.available()
                ? QStringLiteral("color: #388e3c;")
                : QStringLiteral("color: #b36b00;"));
        const QString target_summary =
            whole_frame ? owner.tr("whole frame")
            : selected_target
                ? owner.tr("%1 @%2")
                      .arg(text(selected_target->name))
                      .arg(depth->value())
                : owner.tr("no target");
        const QString barrier_summary =
            logical_scene->property("pelicanBarrierCoverage").toString();
        // N-7 rule 2: the line names what was loaded and what is on screen.
        // Generation, physical and execution planning, the response size and
        // the refresh time are facts a reader asks for, not facts that have
        // to be read every time -- so they are the row's tooltip.
        showStatus(
            owner.tr("%1 \u00b7 %2 \u00b7 %3 passes (%4 compute) \u00b7 "
                     "%5 resources \u00b7 %6")
                .arg(text(model->graph), target_summary)
                .arg(static_cast<qulonglong>(model->nodes.size()))
                .arg(static_cast<qulonglong>(compute_count))
                .arg(static_cast<qulonglong>(model->resources.size()))
                .arg(barrier_summary),
            owner.tr("%1 | %2 | %3%4 passes/tasks (%5 compute), %6, "
                     "%7 resources | %8 | %9 | %10 response | "
                     "refreshed %11. Snapshot updates "
                     "only when the engine connects or Refresh is pressed; it "
                     "is not polled per frame.")
                .arg(text(model->graph), target_summary, generation)
                .arg(static_cast<qulonglong>(model->nodes.size()))
                .arg(static_cast<qulonglong>(compute_count))
                .arg(execution_summary)
                .arg(static_cast<qulonglong>(model->resources.size()))
                .arg(barrier_summary)
                .arg(physical_summary, byteCount(model->response_bytes),
                     QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
        // G-5: a restart the user declined outlives the plan it refreshed.
        if (!restart_declined_notice.isEmpty())
            showStatus(std::exchange(restart_declined_notice, {}));
    }

    // Q-4. The reply, laid out for reading, is built only while somebody is
    // reading it. A quarter of a megabyte of pretty-printed JSON used to be
    // built and handed to this text box on every reply, whichever tab was in
    // front -- for a view nobody had opened.
    void refreshRawJson() {
        if (raw_json == nullptr || !model) return;
        if (!owner.isVisible() || tabs->currentWidget() != raw_json) return;
        if (!raw_json_stale) return;
        raw_json_stale = false;
        try {
            raw_json->setPlainText(
                text(Json::parse(model->raw_json).dump(2)));
        } catch (const std::exception &) {
            raw_json->setPlainText(text(model->raw_json));
        }
    }

    void addResourceGroup(QTreeWidgetItem *pass_item, const QString &title,
                          const std::vector<std::string> &names) {
        auto *group = groupItem(pass_item, title, names.size());
        for (const auto &name : names) {
            auto *item = new QTreeWidgetItem(group);
            item->setText(1, resourceLabel(*model, name));
        }
    }

    void openShaderSource(const std::string &logical_ref) {
        try {
            if (!project_resolver) {
                throw std::runtime_error(
                    "shader source open requires an open project");
            }
            const auto path =
                Pelican::materializeShaderSourceOpenReference(
                    *project_resolver, logical_ref);
            if (!shader_source_open_action ||
                !shader_source_open_action(path)) {
                throw std::runtime_error(
                    "the desktop could not open shader source: " +
                    path.string());
            }
            showStatus(owner.tr("Opened shader source %1")
                           .arg(QString::fromStdString(logical_ref)));
            status->setStyleSheet(QStringLiteral("color: #388e3c;"));
        } catch (const std::exception &error) {
            const QString reason = QString::fromUtf8(error.what());
            showStatus(oneLine(reason), reason);
            status->setStyleSheet(QStringLiteral("color: #b00020;"));
        }
    }

    void addShaderResolution(QTreeWidget *tree,
                             QTreeWidgetItem *pass_item,
                             const FramePlanNode &node) {
        const auto &resolution = node.shader_resolution;
        if (resolution.state ==
            FramePlanShaderResolutionState::material_owned) {
            auto *group = groupItem(pass_item, owner.tr("Shaders"));
            addFact(group, owner.tr("State"),
                    QStringLiteral("material_owned"));
            return;
        }
        if (resolution.state ==
            FramePlanShaderResolutionState::not_applicable) {
            auto *group = groupItem(pass_item, owner.tr("Shaders"));
            addFact(group, owner.tr("State"),
                    QStringLiteral("not_applicable"));
            return;
        }

        auto *group = groupItem(
            pass_item, owner.tr("Shaders"),
            resolution.stages.size() +
                resolution.closesthit_slots.size() +
                resolution.anyhit_slots.size() + resolution.intersection_slots.size());
        const auto add_stage = [&](
            const FramePlanShaderStage &stage,
            const QString &stage_name,
            std::optional<std::size_t> hit_slot = std::nullopt) {
            auto *stage_item = new QTreeWidgetItem(group);
            stage_item->setText(1, stage_name);
            stage_item->setText(
                2, QStringLiteral("[%1]").arg(text(stage.origin)));

            QString reference = text(stage.effective_ref);
            if (stage.declared_ref &&
                *stage.declared_ref != stage.effective_ref) {
                reference = owner.tr("declared: %1  →  effective: %2")
                                .arg(text(*stage.declared_ref),
                                     text(stage.effective_ref));
            }
            stage_item->setText(3, reference);

            auto *open = new QPushButton(owner.tr("Open"), tree);
            open->setObjectName(
                QStringLiteral("pelican.shaderSourceOpen"));
            open->setProperty("pelicanNode",
                              QString::fromStdString(node.name));
            open->setProperty("pelicanStage",
                              QString::fromStdString(stage.stage));
            open->setProperty(
                "pelicanStageIndex",
                stage.index ? QVariant::fromValue<qulonglong>(*stage.index)
                            : QVariant{});
            open->setProperty(
                "pelicanHitSlot",
                hit_slot
                    ? QVariant::fromValue<qulonglong>(*hit_slot)
                    : QVariant{});
            if (stage.source_open_ref) {
                const auto logical_ref = *stage.source_open_ref;
                open->setProperty(
                    "pelicanSourceOpenRef",
                    QString::fromStdString(logical_ref));
                open->setToolTip(
                    owner.tr("Open %1")
                        .arg(QString::fromStdString(logical_ref)));
                QObject::connect(
                    open, &QPushButton::clicked, &owner,
                    [this, logical_ref] {
                        openShaderSource(logical_ref);
                    });
            } else {
                open->setEnabled(false);
                const auto reason = stage.source_open_reason.value_or(
                    "source_not_found");
                open->setText(text(reason));
                open->setToolTip(
                    owner.tr("Shader source cannot be opened: %1")
                        .arg(text(reason)));
            }
            tree->setItemWidget(stage_item, 4, open);
        };
        for (const auto &stage : resolution.stages) {
            const QString stage_name =
                stage.index
                    ? QStringLiteral("%1[%2]")
                          .arg(text(stage.stage))
                          .arg(static_cast<qulonglong>(*stage.index))
                    : text(stage.stage);
            add_stage(stage, stage_name);
        }
        const auto slot_name = [](const FramePlanHitShaderSlot &slot) {
            auto label = text(slot.projection.stage) + QStringLiteral("[hit_slot=%1]")
                .arg(static_cast<qulonglong>(slot.hit_slot));
            if (!slot.group_name.empty())
                label += QStringLiteral(" %1 / %2").arg(text(slot.group_name), text(slot.ray_type));
            return label;
        };
        for (const auto &slot : resolution.closesthit_slots) {
            add_stage(
                slot.projection,
                slot_name(slot),
                slot.hit_slot);
        }
        for (const auto &slot : resolution.intersection_slots) {
            add_stage(slot.projection,
                slot_name(slot), slot.hit_slot);
        }
        for (const auto &slot : resolution.anyhit_slots) {
            add_stage(
                slot.projection,
                slot_name(slot),
                slot.hit_slot);
        }
    }

    void populatePasses(QTreeWidget *tree,
                        std::span<const FramePlanNode> nodes) {
        for (const FramePlanNode &node : nodes) {
            auto *item = new QTreeWidgetItem(tree);
            item->setText(0, QString::number(static_cast<qulonglong>(node.order)));
            item->setText(1, text(node.name));
            item->setText(2, text(node.kind));
            item->setText(3, joined(node.reads));
            item->setText(4, joined(node.writes));
            QFont font = item->font(1);
            font.setBold(true);
            item->setFont(1, font);

            auto *facts = groupItem(item, owner.tr("Plan facts"));
            addFact(facts, owner.tr("Source"), text(node.source));
            addFact(facts, owner.tr("Provider feature"),
                    text(node.provider_feature));
            addFact(facts, owner.tr("Provider ref"),
                    text(node.provider_reference));
            addFact(facts, owner.tr("Level"),
                    QString::number(static_cast<qulonglong>(node.level)));
            addFact(facts, owner.tr("Declaration index"),
                    QString::number(
                        static_cast<qulonglong>(node.declaration_index)));
            addFact(facts, owner.tr("View family"), text(node.view_family));
            addFact(facts, owner.tr("Material variant"),
                    text(node.material_variant));
            if (!node.snapshot_after.empty()) {
                addFact(facts, owner.tr("Snapshot after"),
                        text(node.snapshot_after));
                addFact(facts, owner.tr("Snapshot bytes"),
                        byteCount(node.byte_size));
            }
            if (!node.color_load_op.empty()) {
                addFact(facts, owner.tr("Color load / store"),
                        QStringLiteral("%1 / %2")
                            .arg(text(node.color_load_op),
                                 text(node.color_store_op)));
            }
            if (!node.depth_load_op.empty()) {
                addFact(facts, owner.tr("Depth load / store"),
                        QStringLiteral("%1 / %2")
                            .arg(text(node.depth_load_op),
                                 text(node.depth_store_op)));
            }

            addShaderResolution(tree, item, node);
            if (!node.callable_record_source.empty()) {
                auto *records = groupItem(item, owner.tr("Callable record settings"), node.callable_records.size());
                addFact(records, owner.tr("Source"), text(node.callable_record_source));
                if (node.callable_record_source == "runtime_cpu_settings")
                    addFact(records, owner.tr("Observation"), owner.tr("CPU settings at refresh time; not GPU completion readback"));
                for (const auto &record : node.callable_records) {
                    auto *fields = groupItem(records, owner.tr("Record %1").arg(record.index), record.fields.size());
                    for (const auto &field : record.fields) {
                        addFact(fields, QStringLiteral("%1 (%2, offset %3)").arg(text(field.name), text(field.type)).arg(field.offset),
                                text(field.value));
                    }
                }
            }

            addResourceGroup(item, owner.tr("Inputs"), node.reads);
            addResourceGroup(item, owner.tr("History inputs"),
                             node.history_reads);
            addResourceGroup(item, owner.tr("Outputs"), node.writes);

            if (!node.attachments.empty()) {
                auto *attachment_group = groupItem(
                    item, owner.tr("Attachment operations"),
                    node.attachments.size());
                for (const auto &attachment : node.attachments) {
                    auto *attachment_item =
                        new QTreeWidgetItem(attachment_group);
                    attachment_item->setText(1, text(attachment.resource));
                    attachment_item->setText(2, text(attachment.aspect));
                    attachment_item->setText(
                        3, owner.tr("load: %1").arg(text(attachment.load_op)));
                    attachment_item->setText(
                        4, owner.tr("store: %1").arg(text(attachment.store_op)));
                }
            }

            if (!node.incoming_barriers.empty() ||
                !node.outgoing_barriers.empty()) {
                auto *barrier_group = groupItem(
                    item, owner.tr("Barriers"),
                    node.incoming_barriers.size() +
                        node.outgoing_barriers.size());
                for (const std::size_t barrier_index : node.incoming_barriers) {
                    const auto &barrier = model->barriers.at(barrier_index);
                    auto *barrier_item = new QTreeWidgetItem(barrier_group);
                    barrier_item->setText(1, owner.tr("Incoming from %1")
                                                 .arg(text(barrier.from)));
                    barrier_item->setText(2, text(barrier.kind));
                    barrier_item->setText(3, text(barrier.resource));
                }
                for (const std::size_t barrier_index : node.outgoing_barriers) {
                    const auto &barrier = model->barriers.at(barrier_index);
                    auto *barrier_item = new QTreeWidgetItem(barrier_group);
                    barrier_item->setText(1,
                                          owner.tr("Outgoing to %1")
                                              .arg(text(barrier.to)));
                    barrier_item->setText(2, text(barrier.kind));
                    barrier_item->setText(4, text(barrier.resource));
                }
            }

            if (!node.semantic_dialect.empty() || !node.resource_uses.empty()) {
                auto *execution = groupItem(item, owner.tr("Execution"));
                addFact(execution, owner.tr("Dialect"),
                        text(node.semantic_dialect));
                addFact(execution, owner.tr("Implementation"),
                        text(node.selected_implementation));
                addFact(execution, owner.tr("Endpoint"),
                        text(node.selected_endpoint));
                addFact(execution, owner.tr("Capabilities"),
                        joined(node.required_capabilities));
                if (!node.resource_uses.empty()) {
                    auto *uses = groupItem(execution, owner.tr("Resource uses"),
                                           node.resource_uses.size());
                    for (const auto &use : node.resource_uses) {
                        auto *use_item = new QTreeWidgetItem(uses);
                        use_item->setText(1, text(use.resource));
                        use_item->setText(
                            2, QStringLiteral("%1 / %2")
                                   .arg(text(use.access), text(use.intent)));
                        use_item->setText(3, text(use.epoch));
                        use_item->setText(4, text(use.footprint));
                    }
                }
            }

            if (node.material_filter) {
                auto *material_filter =
                    groupItem(item, owner.tr("Material filter"));
                addFact(material_filter, owner.tr("Include"),
                        joined(node.material_filter->include));
                addFact(material_filter, owner.tr("Exclude"),
                        joined(node.material_filter->exclude));
                addFact(material_filter, owner.tr("Unmatched include"),
                        joined(node.material_filter->unmatched_include));
                addFact(material_filter, owner.tr("Unmatched exclude"),
                        joined(node.material_filter->unmatched_exclude));
                addFact(material_filter, owner.tr("Filter id"),
                        text(node.material_filter->filter_id));
                addFact(material_filter, owner.tr("Resolution"),
                        text(node.material_filter->resolution_state));
                if (node.material_filter->resolved_draw_count) {
                    addFact(material_filter, owner.tr("Resolved draws"),
                            QString::number(static_cast<qulonglong>(
                                *node.material_filter->resolved_draw_count)));
                }
            }
        }
    }

    void populateLogicalDetails() {
        logical_details->clear();
        const auto &selected = logical_scene->selectedNode();
        if (!model || !selected || selected->graph != model->graph) {
            auto *message = new QTreeWidgetItem(logical_details);
            message->setText(1, owner.tr("No node selected"));
        } else {
            const auto found = std::ranges::find(
                model->nodes, selected->name, &FramePlanNode::name);
            if (found == model->nodes.end()) {
                auto *message = new QTreeWidgetItem(logical_details);
                message->setText(1, owner.tr("No node selected"));
            } else {
                populatePasses(
                    logical_details,
                    std::span<const FramePlanNode>{&*found, 1});
            }
        }
        logical_details->header()->setSectionResizeMode(
            1, QHeaderView::ResizeToContents);
    }

    void populatePasses() {
        populatePasses(passes, model->nodes);
        passes->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    }

    void populateDecisions() {
        for (const auto &group : model->decision_groups) {
            auto *subject = new QTreeWidgetItem(decisions);
            subject->setText(
                0, QStringLiteral("%1 (%2)")
                       .arg(group.subject.empty() ? owner.tr("No subject")
                                                  : text(group.subject))
                       .arg(static_cast<qulonglong>(group.decisions.size())));
            QFont font = subject->font(0);
            font.setBold(true);
            subject->setFont(0, font);
            for (const auto &decision : group.decisions) {
                auto *item = new QTreeWidgetItem(subject);
                item->setText(1, text(decision.id));
                item->setText(2, text(decision.selected));
                item->setText(3, text(decision.detail));
            }
        }
    }

    void populateBackends() {
        if (!model->backend_diagnostics.empty()) {
            auto *selection = new QTreeWidgetItem(backends);
            selection->setText(0, owner.tr("Selection diagnostics"));
            QFont font = selection->font(0);
            font.setBold(true);
            selection->setFont(0, font);
            for (const auto &diagnostic : model->backend_diagnostics) {
                auto *item = new QTreeWidgetItem(selection);
                item->setText(1, text(diagnostic.severity));
                item->setText(2, text(diagnostic.subject));
                item->setText(3, text(diagnostic.id));
                item->setText(4, text(diagnostic.detail));
            }
        }

        for (const auto &candidate : model->backend_candidates) {
            auto *candidate_item = new QTreeWidgetItem(backends);
            candidate_item->setText(0, text(candidate.candidate));
            candidate_item->setText(
                1, candidate.selected
                       ? owner.tr("Selected")
                       : (candidate.feasible ? owner.tr("Feasible")
                                             : owner.tr("Rejected")));
            candidate_item->setText(2, text(candidate.endpoint));
            QFont font = candidate_item->font(0);
            font.setBold(true);
            candidate_item->setFont(0, font);

            for (const auto &failure : candidate.failures) {
                auto *item = new QTreeWidgetItem(candidate_item);
                item->setText(1, owner.tr("Failure"));
                item->setText(2, text(failure.subject));
                item->setText(3, text(failure.id));
                item->setText(4, text(failure.detail));
            }
            for (const auto &diagnostic : candidate.diagnostics) {
                auto *item = new QTreeWidgetItem(candidate_item);
                item->setText(1, text(diagnostic.severity));
                item->setText(2, text(diagnostic.subject));
                item->setText(3, text(diagnostic.id));
                item->setText(4, text(diagnostic.detail));
            }
        }
    }

    void populateResources() {
        for (const FramePlanResource &resource : model->resources) {
            auto *item = new QTreeWidgetItem(resources);
            item->setText(0, text(resource.name));
            item->setText(1, text(resource.format));
            item->setText(2, joined(resource.writers));
            QStringList readers{joined(resource.readers)};
            if (!resource.history_readers.empty()) {
                readers.push_back(owner.tr("history: %1")
                                      .arg(joined(resource.history_readers)));
            }
            readers.removeAll(QString{});
            item->setText(3, readers.join(QStringLiteral(" | ")));
            QFont font = item->font(0);
            font.setBold(true);
            item->setFont(0, font);

            auto *facts = groupItem(item, owner.tr("Resource facts"));
            addFact(facts, owner.tr("Kind"), text(resource.kind));
            addFact(facts, owner.tr("Source"), text(resource.source));
            addFact(facts, owner.tr("Dimension"), text(resource.dimension));
            addFact(facts, owner.tr("Alias group"),
                    text(resource.alias_group));
            if (resource.width && resource.height) {
                addFact(facts, owner.tr("Extent"),
                        QStringLiteral("%1 x %2")
                            .arg(static_cast<qulonglong>(*resource.width))
                            .arg(static_cast<qulonglong>(*resource.height)));
            }
            if (resource.extent) {
                addFact(facts, owner.tr("Authored extent"),
                        authoredExtent(*resource.extent));
            }
            addFact(facts, owner.tr("Representation"),
                    text(resource.representation));
            addFact(facts, owner.tr("Widest read"),
                    text(resource.widest_read));
            addFact(facts, owner.tr("Aliasable"),
                    resource.representation.empty()
                        ? QString{}
                        : (resource.aliasable ? owner.tr("yes")
                                              : owner.tr("no")));
            if (!resource.representation.empty()) {
                addFact(facts, owner.tr("Lifetime"),
                        lifetimeLabel(resource.lifetime));
            }
            addFact(facts, owner.tr("Reason"), text(resource.reason));
            addFact(facts, owner.tr("Provider feature"),
                    text(resource.provider_feature));
            addFact(facts, owner.tr("Provider ref"),
                    text(resource.provider_reference));
            addFact(facts, owner.tr("Sampling"), text(resource.sampling));
            addFact(facts, owner.tr("View policy"),
                    text(resource.view_policy));
            addFact(facts, owner.tr("Fallback"), text(resource.fallback));
            addFact(facts, owner.tr("Material consumers"),
                    joined(resource.material_consumers));
            addFact(facts, owner.tr("Fullscreen consumers"),
                    joined(resource.fullscreen_consumers));
        }
    }

    void populatePhysicalPlan() {
        auto *state = new QTreeWidgetItem(physical);
        QFont state_font = state->font(0);
        state_font.setBold(true);
        state->setFont(0, state_font);
        state->setText(0, owner.tr("Physical target plan"));
        if (!model->physical_plan.available()) {
            state->setText(1, owner.tr("Unavailable"));
            state->setText(2,
                           text(model->physical_plan.unavailable_reason_code));
            state->setText(3,
                           text(model->physical_plan.unavailable_reason));
        } else {
            state->setText(1, owner.tr("Available"));
            state->setText(2, text(model->physical_plan.planning_profile));
            state->setText(
                3, owner.tr("%1 alias groups, %2 alias candidates")
                       .arg(static_cast<qulonglong>(
                           model->physical_plan.alias_groups.size()))
                       .arg(static_cast<qulonglong>(
                           model->physical_plan.alias_candidates.size())));

            addFact(state, owner.tr("Graph"),
                    text(model->physical_plan.graph));
            addFact(state, owner.tr("Logical fingerprint"),
                    text(model->physical_plan.logical_graph_fingerprint));
            addFact(state, owner.tr("Automatic fingerprint"),
                    text(model->physical_plan.automatic_plan_fingerprint));
            addFact(state, owner.tr("Planning profile"),
                    text(model->physical_plan.planning_profile));
            addFact(state, owner.tr("Planning endpoint"),
                    text(model->physical_plan.planning_endpoint));
            if (model->physical_plan.output_width &&
                model->physical_plan.output_height) {
                addFact(
                    state, owner.tr("Canonical output extent"),
                    QStringLiteral("%1 x %2")
                        .arg(static_cast<qulonglong>(
                            *model->physical_plan.output_width))
                        .arg(static_cast<qulonglong>(
                            *model->physical_plan.output_height)));
            }

            auto *aliases = groupItem(
                state,
                owner.tr("Alias groups (%1)")
                    .arg(static_cast<qulonglong>(
                        model->physical_plan.alias_groups.size())));
            for (const auto &group : model->physical_plan.alias_groups) {
                auto *item = new QTreeWidgetItem(aliases);
                item->setText(0, text(group.id));
                item->setText(1, owner.tr("Adopted"));
                item->setText(2, joined(group.resources));
            }

            const auto add_candidates =
                [&](const QString &label,
                    const std::vector<FramePlanOpportunityPair> &candidates) {
                    auto *group = groupItem(
                        state,
                        QStringLiteral("%1 (%2)")
                            .arg(label)
                            .arg(static_cast<qulonglong>(candidates.size())));
                    for (const auto &candidate : candidates) {
                        auto *item = new QTreeWidgetItem(group);
                        item->setText(0, label);
                        item->setText(1, candidate.adopted
                                                 ? owner.tr("Adopted")
                                                 : owner.tr("Not adopted"));
                        item->setText(2,
                                      QStringLiteral("%1 + %2")
                                          .arg(text(candidate.first),
                                               text(candidate.second)));
                    }
                };
            add_candidates(owner.tr("Alias candidates"),
                           model->physical_plan.alias_candidates);
            add_candidates(owner.tr("Fusion candidates"),
                           model->physical_plan.fusion_candidates);
            add_candidates(owner.tr("Parallel candidates"),
                           model->physical_plan.parallel_candidates);

            auto *scope_group = groupItem(
                state, owner.tr("Scopes"), model->physical_plan.scopes.size());
            for (const auto &scope : model->physical_plan.scopes) {
                auto *item = new QTreeWidgetItem(scope_group);
                item->setText(0, text(scope.id));
                item->setText(1, text(scope.kind));
                item->setText(2, joined(scope.nodes));
                item->setText(
                    3, owner.tr("%1; %2 execution(s); %3 local reads")
                           .arg(text(scope.view_execution))
                           .arg(static_cast<qulonglong>(scope.execution_count))
                           .arg(static_cast<qulonglong>(
                               scope.local_reads.size())));
            }

            auto *lowering_group = groupItem(
                state, owner.tr("Lowering nodes"),
                model->physical_plan.lowering_nodes.size());
            for (const auto &node : model->physical_plan.lowering_nodes) {
                auto *item = new QTreeWidgetItem(lowering_group);
                item->setText(0, text(node.name));
                item->setText(1,
                              QStringLiteral("%1 / %2")
                                  .arg(text(node.kind), text(node.dialect)));
                item->setText(2, joined(node.sources));
                item->setText(3, joined(node.required_physical_features));
            }

            if (model->physical_plan.resolution_plan) {
                const auto &resolution =
                    *model->physical_plan.resolution_plan;
                auto *resolution_group =
                    groupItem(state, owner.tr("Resolution plan"));
                addFact(resolution_group, owner.tr("Render source"),
                        text(resolution.render_source_resource));
                addFact(resolution_group, owner.tr("Render extent"),
                        authoredExtent(resolution.render_extent));
                addFact(resolution_group, owner.tr("Output source"),
                        text(resolution.output_source_resource));
                addFact(resolution_group, owner.tr("Output extent"),
                        authoredExtent(resolution.output_extent));
                addFact(resolution_group, owner.tr("Scene resources"),
                        joined(resolution.scene_resources));
            }

            auto *wire_group = groupItem(
                state, owner.tr("Retained wire sections"),
                model->physical_plan.wire_sections.size());
            for (const auto &section : model->physical_plan.wire_sections) {
                auto *item = new QTreeWidgetItem(wire_group);
                item->setText(0, text(section.name));
                item->setText(2, byteCount(section.json.size()));
            }
        }

        if (!model->execution_plan.available()) {
            auto *execution_state =
                groupItem(state, owner.tr("Execution plan unavailable"));
            execution_state->setText(
                1, text(model->execution_plan.unavailable_reason_code));
            execution_state->setText(
                2, text(model->execution_plan.unavailable_reason));
        } else {
            auto *dependency_group = groupItem(
                state, owner.tr("Execution dependencies"),
                model->dependencies.size());
            for (const auto &dependency : model->dependencies) {
                auto *item = new QTreeWidgetItem(dependency_group);
                item->setText(
                    0, QStringLiteral("%1 -> %2")
                           .arg(text(dependency.from), text(dependency.to)));
                item->setText(1, text(dependency.reason));
                item->setText(2, text(dependency.resource));
            }
        }

        if (model->gpu_resource_arena) {
            auto *arena = groupItem(
                state,
                owner.tr("GPU resource arena (%1)")
                    .arg(static_cast<qulonglong>(
                        model->gpu_resource_arena->resource_count)));
            for (const auto &scope : model->gpu_resource_arena->scopes) {
                auto *scope_item = new QTreeWidgetItem(arena);
                scope_item->setText(0, text(scope.owner_scope));
                scope_item->setText(1, owner.tr("Owner scope"));
                scope_item->setText(
                    2, owner.tr("%1 resources / %2 leases")
                           .arg(static_cast<qulonglong>(scope.resources.size()))
                           .arg(static_cast<qulonglong>(
                               scope.resource_lease_count)));
                for (const auto &resource : scope.resources) {
                    auto *item = new QTreeWidgetItem(scope_item);
                    item->setText(0, text(resource.name));
                    item->setText(1, text(resource.kind));
                    item->setText(
                        2, owner.tr("handle %1")
                               .arg(static_cast<qulonglong>(resource.handle)));
                    item->setText(3, byteCount(resource.declared_bytes));
                }
            }
        }
    }

    void populateBarriers() {
        for (std::size_t index = 0; index < model->barriers.size(); ++index) {
            const auto &barrier = model->barriers[index];
            auto *item = new QTreeWidgetItem(barriers);
            item->setText(0, QString::number(static_cast<qulonglong>(index)));
            item->setText(1, text(barrier.from));
            item->setText(2, text(barrier.to));
            item->setText(3, text(barrier.resource));
            item->setText(4, text(barrier.kind));
        }
    }

    void populateMaterials() {
        if (model->material_routes.empty()) {
            auto *empty = new QTreeWidgetItem(materials);
            empty->setText(0, owner.tr("No material routes were published."));
            empty->setDisabled(true);
            return;
        }
        for (const auto &route : model->material_routes) {
            auto *item = new QTreeWidgetItem(materials);
            item->setText(0, text(route.route));
            item->setText(1, text(route.pass));
            item->setText(2, text(route.phase));
            item->setText(3, text(route.contract));
            item->setText(4, text(route.shader_contract));
        }
    }
};

FramePlanWidget::FramePlanWidget(EmbeddedViewport *viewport, QWidget *parent)
    : QWidget(parent) {
    if (viewport == nullptr) {
        throw std::invalid_argument(
            "FramePlanWidget requires an embedded viewport");
    }
    setObjectName(QStringLiteral("pelican.framePlan"));
    impl_ = std::make_unique<Impl>(*this, *viewport);
}

FramePlanWidget::~FramePlanWidget() = default;

void FramePlanWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    // Coming back into view is a moment the numbers are stale, so ask once
    // now and let the clock take over.
    impl_->requestGpuTiming();
    impl_->updateGpuTimingPoll();
    // Now that it is in front, it may ask. Nothing is lost by having waited:
    // both reads are cheap and the canvas draws its bands the moment they land.
    impl_->requestAuthoringContext();
    impl_->requestTaskAuthoring();
    impl_->refreshRawJson();
    if (impl_->model) impl_->loadPlacement();
}

void FramePlanWidget::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    impl_->updateGpuTimingPoll();
}

void FramePlanWidget::receiveResult(const QByteArray &result_json) {
    impl_->consumeResult(result_json);
}

void FramePlanWidget::receiveGpuTimingResult(
    const QByteArray &result_json) {
    impl_->consumeGpuTimingResult(result_json);
}

void FramePlanWidget::setProjectRoot(
    const std::filesystem::path &project_root) {
    const auto project_file = project_root / "project.json";
    std::ifstream input{project_file, std::ios::binary};
    if (!input) {
        throw std::runtime_error(
            "could not open project.json for shader source resolution: " +
            project_file.string());
    }
    const std::string contents{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    auto parsed = Pelican::parseProjectEnvelopeText(contents);
    Pelican::ProjectPathResolver resolver;
    resolver.setup(project_root, false, parsed.envelope);
    impl_->project_resolver = std::move(resolver);
    impl_->followShaderSources();
}

void FramePlanWidget::refresh() { impl_->requestRefresh(); }

void FramePlanWidget::receiveShaderReloadStatus(const QByteArray &status_json) {
    impl_->reload_follower->consume(status_json);
}

void FramePlanWidget::receiveShaderReloadOutcome(
    const ShaderReloadOutcome &outcome) {
    impl_->applyShaderReloadOutcome(outcome);
}

void FramePlanWidget::setShaderReloadLogAction(
    std::function<void(const QString &)> action) {
    impl_->shader_reload_log_action = std::move(action);
}

void FramePlanWidget::setEngineRestartAction(
    std::function<void(const QString &reason)> action) {
    impl_->engine_restart_action = std::move(action);
}

void FramePlanWidget::openNewRenderTarget(const QString &name) {
    impl_->new_target_name = name.toStdString();
    impl_->openNewRenderTarget();
}

void FramePlanWidget::setRenderTargetCreatedAction(
    std::function<void(const QString &name)> action) {
    impl_->render_target_created_action = std::move(action);
}

void FramePlanWidget::setInlineFieldChoices(
    std::function<QStringList(const QString &field)> choices) {
    impl_->shader_choices = std::move(choices);
}

void FramePlanWidget::engineRestartDeclined(const QString &message) {
    // The engine that committed the change is still the one running: read
    // what it has now, as after any other commit, then say why nothing
    // restarted.
    impl_->restart_declined_notice = message;
    impl_->requestAuthoringContext();
    impl_->requestTaskAuthoring();
    impl_->requestRefresh();
    impl_->status->setStyleSheet({});
    impl_->showStatus(message);
}


void FramePlanWidget::openTasksForHitGroup(const QString &group) {
    if (group.isEmpty() || !impl_->task_edit_action) return;
    if (!impl_->model) {
        impl_->requested_hit_group = group;
        impl_->requestRefresh();
        return;
    }
    QStringList labels;
    QStringList authored_names;
    const auto matches = [&](const auto &hit_entries) {
        return std::ranges::any_of(hit_entries, [&](const auto &slot) { return text(slot.group_name) == group; });
    };
    for (const auto &node : impl_->model->nodes) {
        if (node.kind != "compute" || node.authored_name.empty()) continue;
        const auto &shaders = node.shader_resolution;
        if (!matches(shaders.closesthit_slots) && !matches(shaders.anyhit_slots) && !matches(shaders.intersection_slots)) continue;
        labels.push_back(text(node.name));
        authored_names.push_back(text(node.authored_name));
    }
    if (labels.empty()) {
        impl_->showStatus(tr("No RT task uses hit group %1").arg(group), tr("No RT task using hit group %1 is present in the loaded Frame Plan. Refresh to update it.").arg(group));
        return;
    }
    int index = 0;
    if (labels.size() > 1) {
        bool accepted = false;
        const auto selected = QInputDialog::getItem(this, tr("Related RT tasks"),
            tr("Tasks using hit group %1 in the loaded Frame Plan").arg(group), labels, 0, false, &accepted);
        if (!accepted) return;
        index = labels.indexOf(selected);
    }
    if (index >= 0) impl_->task_edit_action(authored_names.at(index));
}

void FramePlanWidget::setNodeOrderAction(std::function<void(const QString &, const QStringList &, quint64)> action) {
    impl_->node_order_action = std::move(action);
}

void FramePlanWidget::setTaskConnectionAction(std::function<void(const QString &, const QStringList &)> action) {
    impl_->task_connection_action = std::move(action);
}

void FramePlanWidget::setPresetPassConnectionAction(std::function<void(const QString &, const QString &, const QStringList &)> action) {
    impl_->pass_connection_action = std::move(action);
}

void FramePlanWidget::setGraphNodeOpenAction(GraphNodeOpenAction action) {
    impl_->graph_node_open_action = std::move(action);
}

void FramePlanWidget::setTaskEditAction(std::function<void(const QString &)> action) {
    impl_->task_edit_action = std::move(action);
}

void FramePlanWidget::setPresetPassEditAction(std::function<void(const QString &, const QString &)> action) {
    impl_->preset_pass_edit_action = std::move(action);
}

void FramePlanWidget::setEditCapability(FramePlanEditCapability capability) {
    impl_->edit = std::move(capability);
    impl_->connectEditSignals();
    impl_->requestAuthoringContext();
    impl_->requestTaskAuthoring();
}

void FramePlanWidget::setLayoutCapability(FramePlanLayoutCapability capability) {
    impl_->placement = std::move(capability);
    impl_->placement_loaded = false;
    if (impl_->model) impl_->loadPlacement();
}

void FramePlanWidget::setPassInsertAction(
    std::function<void(const QString &, const QString &)> action) {
    impl_->pass_insert_action = std::move(action);
}

void FramePlanWidget::setTaskInsertAction(std::function<void()> action) {
    impl_->task_insert_action = std::move(action);
}

void FramePlanWidget::setRenderConfigDigest(
    std::shared_ptr<RenderConfigDigest> digest) {
    impl_->digest_hub = std::move(digest);
    if (impl_->digest_hub)
        impl_->digest_hub->subscribe(this, [impl = impl_.get()](const std::string &moved) {
            impl->documentMoved(moved);
        });
}

void FramePlanWidget::refreshAuthoringContext() {
    impl_->requestAuthoringContext();
    impl_->requestTaskAuthoring();
    impl_->loadPlacement();
}

void FramePlanWidget::setShaderSourceOpenAction(
    ShaderSourceOpenAction action) {
    if (!action) {
        throw std::invalid_argument(
            "shader source open action must not be empty");
    }
    impl_->shader_source_open_action = std::move(action);
}

ToolLayoutSnapshot FramePlanWidget::toolLayoutSnapshot() const {
    return {
        impl_->tabOrder(),
        impl_->logical_splitter->saveState(),
    };
}

bool FramePlanWidget::restoreToolLayout(
    const ToolLayoutSnapshot &snapshot) {
    const ToolLayoutSnapshot previous = toolLayoutSnapshot();
    if (!impl_->reorderTabs(snapshot.tab_order)) {
        return false;
    }
    if (!impl_->logical_splitter->restoreState(
            snapshot.logical_splitter_state)) {
        impl_->reorderTabs(previous.tab_order);
        impl_->logical_splitter->restoreState(
            previous.logical_splitter_state);
        return false;
    }
    return true;
}

void FramePlanWidget::applyDefaultToolLayout() {
    impl_->reorderTabs(defaultToolTabOrder());
    impl_->logical_splitter->setSizes({600, 360});
}

} // namespace PelicanStudio
