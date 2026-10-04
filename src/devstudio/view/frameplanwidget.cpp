#include "frameplanwidget.hpp"

#include "frameplanedit.hpp"
#include "frameplangraphics.hpp"
#include "fullscreenshadertemplate.hpp"
#include "shaderinputcontract.hpp"
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
#include "rendereditcompletion.hpp"
#include "shadersourceresolver.hpp"
#include "inspectormodel.hpp"
#include "graphfoldgroups.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
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
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
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
#include <QMenu>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <tuple>
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
// UX-6b, decision 2: under this zoom a node's GPU time is its tooltip.
constexpr qreal GpuTimingReadableScale = 0.6;

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
    // RU-1: the row Undo / Redo join, after Refresh.
    QHBoxLayout *toolbar_row = nullptr;
    QComboBox *target = nullptr;
    QSpinBox *depth = nullptr;
    QLabel *zoom_status = nullptr;
    QTimer *gpu_timing_poll = nullptr;
    // UX-6b, decisions 1 and 3: the frame's GPU time and the plan's size on
    // the status row, and what the last edit moved beside it.
    QLabel *perf_summary = nullptr;
    QLabel *perf_delta = nullptr;
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
    // LP-4d2: a loop whose copies are picked once the plan read for it is in.
    QString requested_loop_group;
    void showRequestedLoopGroup() {
        const auto loop = std::exchange(requested_loop_group, QString{});
        if (loop.isEmpty()) return;
        if (!logical_scene->selectLoopGroup(loop.toStdString())) {
            picked_loop_line.clear();
            showStatus(owner.tr("The frame plan has no copies of loop %1.").arg(loop));
            return;
        }
        // The copies in the middle of the view.
        QRectF picked;
        for (QGraphicsItem *item : logical_scene->selectedItems())
            picked = picked.isValid() ? picked.united(item->sceneBoundingRect())
                                      : item->sceneBoundingRect();
        if (picked.isValid()) logical->centerOn(picked.center());
        picked_loop_line = owner.tr("Loop %1: its copies are picked.").arg(loop);
        picked_loop_copies = logical_scene->selectedNodes();
        showStatus(picked_loop_line);
    }
    // UX-12b, decision 6: the line a picked loop put up, and the copies it
    // picked. Every plan read also asks for the authoring context and the
    // task catalog, and each runs populate() again when it lands; the
    // summary line populate() writes went over this one milliseconds after
    // it was shown. It stays while the copies are what is picked, until the
    // next edit is sent.
    QString picked_loop_line;
    std::set<FramePlanNodeKey> picked_loop_copies;
    void showPickedLoopLine() {
        if (picked_loop_line.isEmpty()) return;
        if (logical_scene->selectedNodes() != picked_loop_copies) {
            picked_loop_line.clear();
            picked_loop_copies.clear();
            return;
        }
        showStatus(picked_loop_line);
    }
    std::optional<Pelican::ProjectPathResolver> project_resolver;
    FramePlanWidget::ShaderSourceOpenAction shader_source_open_action;
    // F-3: the engine's shader reload status, read after a shader file a node
    // reads is written.
    std::unique_ptr<ShaderReloadFollower> reload_follower;
    // F-5: where a shader reload's log goes besides the marks (the Pass
    // Editor's log under its form); empty text clears it.
    std::function<void(const QString &)> shader_reload_log_action;
    // UX-6a, decision 2: the files the Graph tab's passes read, followed
    // with the plan's; and who hears that a reload moved declarations.
    NodeShaderSources extra_shader_sources;
    std::function<void()> shaders_reloaded_action;
    // UX-6a, decision 4: who takes TRANSFER_SRC off an image.
    FramePlanWidget::DropTransferSourceAction drop_transfer_source_action;
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
    // VI-3a: what the edit in flight says once it has committed, and what
    // the last committed one said; it outlasts the refreshed plan's summary
    // until the next edit is sent.
    QString submitted_notice;
    QString commit_notice;
    // VI-3a rule 4: a reload was refused for a pass; say so once the plan
    // that shows it arrives, and keep saying it while it stands.
    bool announce_reload_refusal = false;
    QString shown_refusal_line;
    QPushButton *refusal_disconnect = nullptr;
    QPushButton *refusal_open_shader = nullptr;
    std::function<void(const QString &, const QString &)> preset_pass_edit_action;
    std::function<void(const QString &)> task_edit_action;
    FramePlanWidget::GraphNodeOpenAction graph_node_open_action;
    FramePlanWidget::GraphLoopNodeOpenAction graph_loop_node_open_action;
    FramePlanWidget::GraphPassOpenAction graph_pass_open_action;
    // GL-5c, decisions 1 and 2: who places the palette's node on the Graph
    // tab for an Add on a graph root, and the Add that waits for the context
    // to say whether the root selects a preset (with what it does if so).
    FramePlanWidget::GraphPlaceAction graph_place_action;
    std::string pending_graph_place;
    std::function<void()> pending_graph_fallback;
    // GL-5c, decision 3: whether the Graph tab edits a pass of the plan.
    std::function<bool(const QString &pass)> graph_edits_pass;
    // GL-5c, decision 5: the entries that open in the Graph, whose tooltips
    // say so once the context says where they go.
    QPushButton *edit_task_button = nullptr;
    QPushButton *edit_pass_button = nullptr;
    QPushButton *add_material_button = nullptr;
    QAction *add_material_action = nullptr;
    QAction *add_fullscreen_action = nullptr;
    QAction *add_task_action = nullptr;
    // LP-4e: who hears that a decompose committed, and the loop it made.
    std::function<void(const QString &loop)> loop_decomposed_action;
    std::string decomposed_loop;
    // The confirmation in front of a decompose, while it is open.
    QPointer<QMessageBox> decompose_prompt;
    // LP-4f2: whether a feature has a loop version, and taking it in (the
    // Features tab's).
    std::function<std::optional<QString>(const QString &feature)> loop_version_offered;
    std::function<void(const QString &feature)> loop_version_replace;
    // LP-4e, decision 5: the draw_loop materials read past the plan's first
    // page ("Read more" in the details), by loop; cleared by a new plan.
    std::map<std::string, std::vector<FramePlanLoopElement>, std::less<>> loop_more;
    qint64 pending_loop_page_request = 0;
    std::string loop_page_node;
    std::function<void(const QString &, const QStringList &)> task_connection_action;
    std::function<void(const QString &, const QString &, const QStringList &)> pass_connection_action;
    std::function<void(const QString &, const QStringList &, quint64)> node_order_action;
    qint64 pending_request = 0;
    // BS-1s: the status line the engine's placeholder plan put there.
    QString fallback_line;
    // Q-18. Another panel's edit that committed can change the plan (the
    // Graph tab's loop count, a branch, a view axis). In view, the plan is
    // read again at once -- after the read in flight, which may predate the
    // commit; out of view, when the panel is next shown.
    bool plan_stale = false;
    bool plan_reread = false;
    // What asks for the plan: the viewport's RPC, unless setPlanRequest
    // named another (a test counts the reads there).
    std::function<qint64(QString *error)> plan_request;
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
    // GL-3a, decision 8: the root's target_planning (read with the root
    // document after each authoring context), for the rows' Serial /
    // Isolate / No alias checks.
    qint64 pending_planning_request = 0;
    GraphTargetPlanning planning;
    QAction *planning_serial = nullptr;
    QAction *planning_isolate = nullptr;
    QAction *planning_no_alias = nullptr;
    // --- UX-15b: the lower layer's decisions (EL-2), named so a scenario
    // fires them: Pin this candidate acts on the picked Backends row.
    QAction *backend_pin = nullptr;
    QAction *backend_unpin = nullptr;
    QAction *physical_freeze = nullptr;
    QAction *physical_remove = nullptr;
    QAction *physical_eject_again = nullptr;
    // Beside the line of a physical_fragment_stale refusal.
    QPushButton *eject_again = nullptr;
    // The bands on screen. A tree lets go of a row's widget only later
    // (deleteLater), so a band is deleted here before its tree is filled
    // again, and its name never answers for two widgets at once.
    std::vector<QPointer<QWidget>> bands;
    void dropBands() {
        for (auto &band : std::exchange(bands, {})) delete band.data();
    }
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
    // --- Q-14: a pass given an input its shader does not read. The edit in
    // flight as it was sent, so its refusal can name the pass and its shader;
    // and the two ways out, beside the status line.
    FramePlanEditRequest submitted_request;
    struct ShaderContractFix {
        std::string graph;
        std::string pass;
        std::string fragment;
        std::vector<std::string> inputs;
        // Q-15: the shader declares an input the pass cannot supply, at
        // `binding` when the engine said.
        bool unsupplied = false;
        std::optional<std::size_t> binding;
    };
    std::optional<ShaderContractFix> contract_fix;
    // VI-3b: the pass a drop on a no-input shader was aimed at.
    std::string last_drop_pass;
    QPushButton *contract_use_passthrough = nullptr;
    QPushButton *contract_open_shader = nullptr;
    std::optional<std::filesystem::path> project_root;

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
        toolbar_row = toolbar;
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
        // GL-5d, decision 6: the Graph's Order… on a rendering-section pass
        // is this one.
        order_nodes->setToolTip(owner.tr(
            "The order of the plan's nodes (data dependencies still decide). The same as "
            "Order… on a pass of the rendering section in the Graph."));
        toolbar->addWidget(order_nodes);
        QObject::connect(order_nodes, &QPushButton::clicked, &owner, [this] { orderNodes(); });
        auto *edit_task = new QPushButton(owner.tr("Edit task"), &owner);
        edit_task->setObjectName(QStringLiteral("pelican.framePlanEditTask"));
        edit_task_button = edit_task;
        toolbar->addWidget(edit_task);
        QObject::connect(edit_task, &QPushButton::clicked, &owner, [this] {
            const auto &selected = logical_scene->selectedNode();
            // GL-5c, decision 3: a task a render.compute node made is the
            // Graph's to edit.
            if (selected && editsInGraph(*selected)) {
                openInGraph(*selected);
                return;
            }
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
        edit_pass_button = edit_pass;
        toolbar->addWidget(edit_pass);
        QObject::connect(edit_pass, &QPushButton::clicked, &owner, [this] {
            const auto &selected = logical_scene->selectedNode();
            // GL-5c, decision 3: a pass the Graph edits opens there.
            if (selected && editsInGraph(*selected)) {
                openInGraph(*selected);
                return;
            }
            if (selected && preset_pass_edit_action)
                preset_pass_edit_action(QString::fromStdString(selected->graph), QString::fromStdString(selected->name));
        });
        auto *new_target = new QPushButton(owner.tr("New render target..."), &owner);
        new_target->setObjectName(
            QStringLiteral("pelican.framePlanNewRenderTarget"));
        // GL-5d, decision 6.
        new_target->setToolTip(owner.tr(
            "Declare a render target. The same form as the Graph's New render target… (a "
            "render.target_ref's combo); a double-click on a resource edits it, as Edit "
            "target… on its reference in the Graph does."));
        toolbar->addWidget(new_target);
        auto *new_material_pass =
            new QPushButton(owner.tr("Add material pass..."), &owner);
        new_material_pass->setObjectName(
            QStringLiteral("pelican.framePlanAddMaterialPass"));
        add_material_button = new_material_pass;
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
        perf_summary = new QLabel(&owner);
        perf_summary->setObjectName(QStringLiteral("pelican.framePlanPerf"));
        perf_summary->setFont(studioSmallFont());
        perf_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        perf_summary->setToolTip(owner.tr(
            "The frame's GPU time (get_status, the average over the engine's window) and the "
            "frame plan's passes, barriers and alias groups"));
        status_row->addWidget(perf_summary);
        perf_delta = new QLabel(&owner);
        perf_delta->setObjectName(QStringLiteral("pelican.framePlanPerfDelta"));
        perf_delta->setFont(studioSmallFont());
        perf_delta->setTextInteractionFlags(Qt::TextSelectableByMouse);
        perf_delta->setToolTip(owner.tr(
            "The last edit: the plan and the GPU time of the generation before it and of the one "
            "it made"));
        // Cut rather than widening the panel; the whole line is its tooltip.
        perf_delta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        perf_delta->setMinimumWidth(120);
        status_row->addWidget(perf_delta, 1);
        reload_authoring = new QPushButton(owner.tr("Reload"), &owner);
        reload_authoring->setObjectName(
            QStringLiteral("pelican.framePlan.reloadAuthoring"));
        reload_authoring->setToolTip(owner.tr(
            "Read the rendering document again, then repeat the change."));
        reload_authoring->hide();
        QObject::connect(reload_authoring, &QPushButton::clicked, &owner,
                         [this] { reloadAuthoring(); });
        status_row->addWidget(reload_authoring);
        // UX-15b, decision 2: a frozen plan the graph moved away from.
        eject_again = new QPushButton(owner.tr("Eject again"), &owner);
        eject_again->setObjectName(
            QStringLiteral("pelican.framePlan.physical.ejectAgainButton"));
        eject_again->setToolTip(owner.tr(
            "Freeze the physical plan again from the plan on screen: one change, "
            "which Undo takes back."));
        eject_again->hide();
        QObject::connect(eject_again, &QPushButton::clicked, &owner,
                         [this] { freezePhysicalPlan(); });
        status_row->addWidget(eject_again);
        // Q-14: the two ways out of a pass whose shader does not read the
        // inputs it was given.
        const auto actions = shaderInputContractActions();
        // VI-3b: engine://passthrough takes any number of inputs, so it is
        // the way out that writes no file.
        contract_use_passthrough =
            new QPushButton(owner.tr(actions[0].label.c_str()), &owner);
        contract_use_passthrough->setObjectName(
            QStringLiteral("pelican.framePlan.shaderContract.usePassthrough"));
        contract_use_passthrough->setToolTip(owner.tr(
            "Draws the pass with engine://passthrough, which takes any number "
            "of inputs and shows one of them: the inputs and the shader go in "
            "one change."));
        contract_use_passthrough->hide();
        QObject::connect(contract_use_passthrough, &QPushButton::clicked,
                         &owner, [this] { useEnginePassthrough(); });
        status_row->addWidget(contract_use_passthrough);
        contract_open_shader =
            new QPushButton(owner.tr(actions[1].label.c_str()), &owner);
        contract_open_shader->setObjectName(
            QStringLiteral("pelican.framePlan.shaderContract.openShader"));
        contract_open_shader->setToolTip(owner.tr(
            "Opens the pass's shader and copies the lines that declare and "
            "read the inputs."));
        contract_open_shader->hide();
        QObject::connect(contract_open_shader, &QPushButton::clicked, &owner,
                         [this] { openContractShader(); });
        status_row->addWidget(contract_open_shader);
        // VI-3a rule 4: the two ways out of a reload that no longer declares
        // a binding the pass feeds.
        refusal_disconnect = new QPushButton(owner.tr("Disconnect"), &owner);
        refusal_disconnect->setObjectName(
            QStringLiteral("pelican.framePlan.reloadRefused.disconnect"));
        refusal_disconnect->setToolTip(owner.tr(
            "Takes the input the new shader no longer declares out of the "
            "pass, so the pass draws with the new shader."));
        refusal_disconnect->hide();
        QObject::connect(refusal_disconnect, &QPushButton::clicked, &owner,
                         [this] { disconnectRefusedInput(); });
        status_row->addWidget(refusal_disconnect);
        refusal_open_shader =
            new QPushButton(owner.tr("Open the shader"), &owner);
        refusal_open_shader->setObjectName(
            QStringLiteral("pelican.framePlan.reloadRefused.openShader"));
        refusal_open_shader->setToolTip(owner.tr(
            "Opens the pass's shader, to declare the binding again."));
        refusal_open_shader->hide();
        QObject::connect(refusal_open_shader, &QPushButton::clicked, &owner,
                         [this] { openRefusedShader(); });
        status_row->addWidget(refusal_open_shader);
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
        // LP-4b: the View menu of a node's right-click.
        logical_scene->viewSelectRequested =
            [this](const FramePlanNodeKey &node, const std::string &axis,
                   std::optional<std::uint64_t> value) {
                pickView(node, axis, value);
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
        // VI-3a: sockets from the shader's declarations. The order of the
        // pass's inputs is the binding, and it is the authoring context's.
        logical_scene->passInputsProvider =
            [this](const FramePlanNodeKey &node) {
                return framePlanPassInputList(authoring, node.graph,
                                              node.name);
            };
        logical_scene->socketDropRequested =
            [this](const FramePlanNodeKey &consumer, const QString &resource,
                   const FramePlanSocketDrop &drop) {
                dropOnSocket(consumer, resource.toStdString(), drop);
            };
        logical_scene->socketCutRequested =
            [this](const FramePlanNodeKey &consumer, std::size_t input_index) {
                cutSocketInput(consumer, input_index);
            };
        // VI-3b: engine://passthrough shows the input whose socket is
        // clicked; the variadic group can be fixed and made variadic again.
        logical_scene->passthroughShownProvider =
            [this](const FramePlanNodeKey &node) {
                return framePlanPassthroughShown(authoring, node.graph,
                                                 node.name);
            };
        logical_scene->socketClicked = [this](const FramePlanNodeKey &node,
                                              std::size_t binding) {
            showPassthroughInput(node, binding);
        };
        logical_scene->socketsFixRequested =
            [this](const FramePlanNodeKey &node) { fixSockets(node); };
        logical_scene->socketsVariadicRequested =
            [this](const FramePlanNodeKey &node) { makeSocketsVariadic(node); };
        logical_scene->fixedSocketsOffered =
            [this](const FramePlanNodeKey &node) {
                return fixedSocketsSource(node).has_value();
            };
        logical_scene->inlineFieldChoices =
            [this](const FramePlanNodeKey &, const QString &field) {
                return shader_choices ? shader_choices(field) : QStringList{};
            };
        // Double-click is the way to the detailed editors: everything the
        // bands do not carry lives in the two forms that already exist.
        logical_scene->nodeEditRequested =
            [this](const FramePlanNodeKey &node) {
                // GL-5c, decision 3: what the Graph edits opens there.
                if (editsInGraph(node)) {
                    openInGraph(node);
                    return;
                }
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
        add_fullscreen_action = add_pass;
        QObject::connect(add_pass, &QAction::triggered, &owner, [this] {
            // GL-5c, decision 1: on a graph root, the palette's fullscreen pass.
            addThrough("render.fullscreen", [this] {
                const auto anchor = logical_scene->insertAnchor();
                if (!anchor) {
                    showStatus(owner.tr(
                        "Select the node the new pass should follow first."));
                    return;
                }
                if (logical_scene->nodeAddRequested)
                    logical_scene->nodeAddRequested(*anchor, false);
            });
        });
        auto *add_task = new QAction(
            QStringLiteral("Add compute task..."), &owner);
        add_task->setObjectName(
            QStringLiteral("pelican.framePlan.addComputeTask"));
        owner.addAction(add_task);
        add_task_action = add_task;
        QObject::connect(add_task, &QAction::triggered, &owner, [this] {
            // GL-5c, decision 2: on a graph root, the palette's compute task
            // (the root's rendering section declares no compute_tasks, MG-2b).
            addThrough("render.compute", [this] {
                const auto anchor = logical_scene->insertAnchor();
                if (logical_scene->nodeAddRequested)
                    logical_scene->nodeAddRequested(
                        anchor ? *anchor
                               : FramePlanNodeKey{model ? model->graph
                                                        : std::string{},
                                                  std::string{}},
                        true);
            });
        });
        logical_scene->setInsertActions(add_pass, add_task);
        // Q-12: a task a render.compute node made is edited in the Graph
        // tab. Named like the others; offered on a right-click over such a
        // task only.
        // VI-3b: Fix sockets / Make sockets variadic, named like the insert
        // entries and acting on the node right-clicked (or picked).
        auto *fix_sockets = new QAction(QStringLiteral("Fix sockets"), &owner);
        fix_sockets->setObjectName(QStringLiteral("pelican.framePlan.fixSockets"));
        // GL-5d, decision 6: the Graph offers the same on the pass.
        fix_sockets->setToolTip(owner.tr(
            "One fixed declaration per connected input, in the pass's shader. The same as "
            "Fix sockets on the pass in the Graph (its right-click or its detail column)."));
        owner.addAction(fix_sockets);
        QObject::connect(fix_sockets, &QAction::triggered, &owner, [this] {
            if (const auto node = logical_scene->insertAnchor()) fixSockets(*node);
            else showStatus(owner.tr("Select the pass whose sockets to fix first."));
        });
        auto *variadic_sockets =
            new QAction(QStringLiteral("Make sockets variadic"), &owner);
        variadic_sockets->setObjectName(
            QStringLiteral("pelican.framePlan.makeSocketsVariadic"));
        variadic_sockets->setToolTip(owner.tr(
            "Put back the line Fix sockets replaced. The same as Make sockets variadic on the "
            "pass in the Graph."));
        owner.addAction(variadic_sockets);
        QObject::connect(variadic_sockets, &QAction::triggered, &owner, [this] {
            if (const auto node = logical_scene->insertAnchor())
                makeSocketsVariadic(*node);
            else
                showStatus(owner.tr("Select the pass whose sockets to make "
                                    "variadic first."));
        });
        logical_scene->setSocketActions(fix_sockets, variadic_sockets);
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
                // GL-1, decision 6: every pass of the plan is on the Graph
                // canvas, a graph node's or folded.
                return graphNodeOf(node) != nullptr || loopOriginOf(node) != nullptr ||
                       planPassOf(node) != nullptr;
            };
        // LP-4e: a material pass becomes a loop over its materials. Named
        // like the others; acts on the node right-clicked (or picked).
        auto *decompose_entry = new QAction(
            QStringLiteral("Decompose into loop..."), &owner);
        decompose_entry->setObjectName(
            QStringLiteral("pelican.framePlan.decomposeIntoLoop"));
        owner.addAction(decompose_entry);
        QObject::connect(decompose_entry, &QAction::triggered, &owner, [this] {
            const auto node = logical_scene->insertAnchor();
            if (!node) {
                showStatus(owner.tr("Select the material pass to decompose first."));
                return;
            }
            decompose(node->graph, node->name);
        });
        logical_scene->setDecomposeAction(decompose_entry);
        logical_scene->decomposeOffered =
            [this](const FramePlanNodeKey &node) -> std::optional<std::string> {
                if (!decomposeAbout(node)) return std::nullopt;
                return framePlanDecomposeRefusal(authoring, model ? &*model : nullptr,
                                                 node.graph, node.name);
            };
        // LP-4f2: a feature's group offers the feature's loop version; the
        // Features tab owns the rest (the confirmation and the request).
        auto *loop_version_entry = new QAction(
            QStringLiteral("Replace with loop version…"), &owner);
        loop_version_entry->setObjectName(
            QStringLiteral("pelican.framePlan.replaceWithLoopVersion"));
        owner.addAction(loop_version_entry);
        QObject::connect(loop_version_entry, &QAction::triggered, &owner, [this] {
            const auto feature = logical_scene->loopVersionFeature();
            if (feature.empty()) {
                showStatus(owner.tr("Right-click the group of the feature to replace first."));
                return;
            }
            if (!loop_version_replace) {
                showStatus(owner.tr("Replacing a feature is unavailable here."));
                return;
            }
            loop_version_replace(text(feature));
        });
        logical_scene->setLoopVersionAction(loop_version_entry);
        logical_scene->loopVersionOffered =
            [this](const std::string &feature) -> std::optional<std::string> {
                if (!loop_version_offered) return std::nullopt;
                const auto reason = loop_version_offered(text(feature));
                if (!reason) return std::nullopt;
                return reason->toStdString();
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
        add_material_action = add_material;
        // GL-5c, decision 1: on a graph root, the palette's material pass.
        QObject::connect(add_material, &QAction::triggered, &owner, [this] {
            addThrough("render.material_pass", [this] { openMaterialPassForm(); });
        });
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
                // UX-6b, decision 2: too small to read, the time is the
                // node's tooltip instead.
                logical_scene->setGpuTimingCompact(scale < GpuTimingReadableScale);
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
             owner.tr("Selected"), owner.tr("Detail"),
             // GL-3a, decision 6: the fold group a decision came from.
             owner.tr("From")},
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
             owner.tr("Contract"), owner.tr("Shader contract"),
             // UX-12c, decision 2.
             owner.tr("Drawn")},
            tabs);
        // GL-2b: named, so a scenario can pick one of their rows.
        passes->setObjectName(QStringLiteral("pelican.framePlanPasses"));
        resources->setObjectName(QStringLiteral("pelican.framePlanResources"));
        physical->setObjectName(QStringLiteral("pelican.framePlanPhysical"));
        decisions->setObjectName(QStringLiteral("pelican.framePlanDecisions"));
        backends->setObjectName(QStringLiteral("pelican.framePlanBackends"));
        barriers->setObjectName(QStringLiteral("pelican.framePlanBarriers"));
        materials->setObjectName(QStringLiteral("pelican.framePlanMaterials"));
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
        // GL-2b, decision 4: each tab is a named entry too
        // (`pelican.framePlan.tab.barriers`), so a scenario brings one
        // forward the way a click on it does.
        for (int index = 0; index < tabs->count(); ++index) {
            QWidget *page = tabs->widget(index);
            auto *show_tab = new QAction(tabs->tabText(index), &owner);
            show_tab->setObjectName(QStringLiteral("pelican.framePlan.tab.") + toolTabId(page));
            owner.addAction(show_tab);
            QObject::connect(show_tab, &QAction::triggered, &owner,
                             [this, page] { tabs->setCurrentWidget(page); });
        }
        // Every row of these tabs that names a pass (its own, a resource's
        // readers and writers, a barrier's two ends, a decision's
        // from_nodes) opens it in the Graph tab; several are offered one
        // by one. Named, the entry opens the first pass of the row picked.
        auto *open_row = new QAction(QStringLiteral("Open in Graph"), &owner);
        open_row->setObjectName(QStringLiteral("pelican.framePlan.openRowInGraph"));
        owner.addAction(open_row);
        QObject::connect(open_row, &QAction::triggered, &owner, [this] {
            auto *tree = qobject_cast<QTreeWidget *>(tabs->currentWidget());
            const auto names = tree != nullptr ? rowNodes(tree->currentItem()) : QStringList{};
            if (names.isEmpty()) {
                showStatus(owner.tr("Select a row that names a pass first."));
                return;
            }
            openRowNode(names.front());
        });
        // GL-3a, decision 8: the graph-wide decisions (EL-2) on the picked
        // row of Passes or Resources, named so a scenario fires them.
        const auto planning_action = [this](const char *name, const QString &label,
                                            const char *field) {
            auto *action = new QAction(label, &owner);
            action->setObjectName(QString::fromLatin1(name));
            action->setCheckable(true);
            owner.addAction(action);
            QObject::connect(action, &QAction::triggered, &owner,
                             [this, field] { togglePlanning(field); });
            return action;
        };
        planning_serial = planning_action("pelican.framePlan.planning.serial",
                                          owner.tr("Serial"), "serial");
        planning_isolate = planning_action("pelican.framePlan.planning.isolate",
                                           owner.tr("Isolate"), "isolate");
        planning_no_alias = planning_action("pelican.framePlan.planning.noAlias",
                                            owner.tr("No alias"), "no_alias");
        // UX-15b, decisions 1 and 2. Always on: what cannot be sent says why
        // on the status line.
        const auto lower_action = [this](const char *name, const QString &label,
                                         void (Impl::*run)()) {
            auto *action = new QAction(label, &owner);
            action->setObjectName(QString::fromLatin1(name));
            owner.addAction(action);
            QObject::connect(action, &QAction::triggered, &owner, [this, run] { (this->*run)(); });
            return action;
        };
        backend_pin = lower_action("pelican.framePlan.backend.pin",
                                   owner.tr("Pin this candidate"), &Impl::pinPickedCandidate);
        backend_unpin = lower_action("pelican.framePlan.backend.unpin", owner.tr("Unpin"),
                                     &Impl::unpinBackend);
        physical_freeze = lower_action("pelican.framePlan.physical.freeze",
                                       owner.tr("Freeze this physical plan"),
                                       &Impl::freezePhysicalPlan);
        physical_remove = lower_action("pelican.framePlan.physical.remove", owner.tr("Remove"),
                                       &Impl::removeFrozenPlan);
        physical_eject_again = lower_action("pelican.framePlan.physical.ejectAgain",
                                            owner.tr("Eject again"), &Impl::freezePhysicalPlan);
        for (QTreeWidget *tree : {passes, resources, physical, decisions, backends, barriers}) {
            tree->setContextMenuPolicy(Qt::CustomContextMenu);
            QObject::connect(tree, &QWidget::customContextMenuRequested, &owner,
                             [this, tree](const QPoint &at) { rowMenu(*tree, at); });
        }

        QObject::connect(tabs, &QTabWidget::currentChanged, &owner,
                         [this](int) { refreshRawJson(); });
        QObject::connect(refresh, &QPushButton::clicked, &owner,
                         [this] { requestRefresh(); });
        QObject::connect(logical_scene, &QGraphicsScene::selectionChanged,
                         &owner,
                         [this] {
                             populateLogicalDetails();
                             updateReloadRefusal();
                         },
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
                             eject_again->hide();
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
        dropBands();
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
        plan_reread = false;
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
        requested_loop_group.clear();
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
        plan_reread = false;
        pending_gpu_timing_request = 0;
        gpu_timing_poll->stop();
        model.reset();
        clearTrees();
        refresh->setEnabled(false);
        status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        // UX-8, decision 13 (c): a code the engine stopped on reads as its
        // sentence and the file to fix; the line itself stays the detail.
        // UX-9, decision 5: the running engine's line names the code and its
        // fields but no file; the project's render document is the one.
        if (const auto stopped = engineStoppedSentence(
                fatal_error_line.toStdString(),
                projectRenderDocument(std::filesystem::path{viewport.projectRoot().toStdWString()}));
            !stopped.empty()) {
            showStatus(QString::fromStdString(stopped), fatal_error_line);
            return;
        }
        showStatus(oneLine(fatal_error_line), fatal_error_line);
    }

    // BS-1s, decision 2: true when the plan is the engine's placeholder,
    // which the status row then says (renderFallbackSentence); a plan of the
    // project's takes that line away again.
    bool showRenderFallback(const QByteArray &result_json) {
        RenderPipelineReading reading;
        if (result_json.contains("\"render_pipeline\"")) {
            try {
                reading = readRenderPipeline(Json::parse(
                    result_json.constData(), result_json.constData() + result_json.size()));
            } catch (const std::exception &) {
            }
        }
        if (reading.state == RenderPipelineReading::State::Fallback) {
            fallback_line = QString::fromStdString(renderFallbackSentence(reading));
            status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
            showStatus(fallback_line,
                       fallback_line + QLatin1Char('\n') +
                           QString::fromStdString(reading.refusal.value("message", std::string{})));
            return true;
        }
        if (!fallback_line.isEmpty() && status->text() == fallback_line) {
            status->setStyleSheet({});
            showStatus({});
        }
        fallback_line.clear();
        return false;
    }

    // N-7 rule 2: the panel has one status row. It is one line, it carries
    // the long form as its tooltip, and an empty line takes the row with it.
    void showStatus(const QString &line, const QString &detail = {}) {
        setPanelNotice(status, line, detail.isEmpty() ? line : detail);
        // GL-5d: an action the Graph tab ran here says it there too.
        if (status_said) {
            const auto said = status_said;
            if (!running_when_read && pending_edit_request == 0 && pending_ticket.isEmpty())
                status_said = {};
            said(line);
        }
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
        // UX-6a, decision 2: and the Graph tab's, read or not, in view or not.
        for (const auto &[node, paths] : extra_shader_sources) {
            auto &held = sources[node];
            for (const auto &path : paths)
                if (std::ranges::none_of(held, [&](const std::filesystem::path &known) {
                        return sameShaderSource(known, path);
                    }))
                    held.push_back(path);
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
        // VI-3a: a reload refused for a pass is said on that pass, with its
        // way out (updateReloadRefusal), not as a failure no node reads.
        if (reading.error_code == "shader_reload_refused@1") {
            clearReloadNotice();
        } else if (!marks.unmatched.empty()) {
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
            // VI-3a: the sockets are the shader's declarations, which the
            // reload may have changed; and a refusal it lifted comes down.
            if (viewport.rpcReady()) requestRefresh();
            if (shaders_reloaded_action) shaders_reloaded_action();
            break;
        case Kind::refused:
            // VI-1b: not a success. The pass keeps its previous shader; the
            // plan that says so is read, and the canvas marks the socket.
            announce_reload_refusal = true;
            if (shader_reload_log_action) shader_reload_log_action(outcome.message);
            if (viewport.rpcReady()) requestRefresh();
            else updateReloadRefusal();
            if (shaders_reloaded_action) shaders_reloaded_action();
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
        hideShaderContract();
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
            // VI-3b: a shader that declares no input -- the two ways out.
            if (request.passthrough_inputs && model) {
                ShaderContractFix fix;
                fix.graph = model->graph;
                fix.pass = request.params.is_object()
                               ? request.params.value("pass_name", std::string{})
                               : std::string{};
                if (fix.pass.empty()) fix.pass = last_drop_pass;
                fix.fragment = request.fragment;
                fix.inputs = *request.passthrough_inputs;
                offerContractButtons(std::move(fix), true);
            }
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
            : request.method == "decompose_material_pass"
                ? edit.decomposeMaterialPass
            // GL-3a, decision 8.
            : request.method == "edit_target_planning" ? edit.editTargetPlanning
            // UX-15b.
            : request.method == "edit_vulkan_plan_pins" ? edit.editVulkanPlanPins
            : request.method == "put_vulkan_physical_fragment"
                ? edit.putVulkanPhysicalFragment
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
        hideShaderContract();
        refusal_disconnect->hide();
        refusal_open_shader->hide();
        eject_again->hide();
        shown_refusal_line.clear();
        submitted_notice = text(request.notice);
        commit_notice.clear();
        picked_loop_line.clear();
        submitted_request = request;
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
        if (receivePlanning(request_id, bytes)) return true;
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
            // GL-3a, decision 8: and the root's target_planning with it.
            if (authoring.received) requestPlanning();
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
            // GL-5c: an Add that waited to know whether the root selects a
            // preset, and the tooltips that say where the Adds go.
            if (authoring.received) {
                syncDelegationTips();
                if (!pending_graph_place.empty()) {
                    const auto op = std::exchange(pending_graph_place, {});
                    addThrough(op, std::exchange(pending_graph_fallback, {}));
                }
            }
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
            // GL-5d: the Graph's action that waited for this context.
            runWhenRead();
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
            runWhenRead();  // GL-5f, decision 2
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

    // A rejection's `error` (or a reply that is one itself).
    static const Json &refusalOf(const Json &response) {
        const auto error = response.is_object() ? response.find("error") : response.end();
        return error != response.end() && error->is_object() ? *error : response;
    }

    // UX-5a: the code and the message's sentence -- never the JSON a
    // message can carry, which the line would cut off before its reason.
    static QString responseError(const Json &response) {
        const auto line = engineRefusalLine(refusalOf(response));
        return line.empty() ? QStringLiteral("the engine refused the edit") : text(line);
    }

    // UX-5a: all a refusal said, for the tooltip and Copy message.
    static QString responseDetail(const Json &response) {
        const auto &reason = refusalOf(response);
        if (!reason.is_object()) return text(response.dump());
        const auto code = reason.value("code", std::string{});
        const auto message = reason.value("message", std::string{});
        if (code.empty() && message.empty()) return text(response.dump());
        return text(code.empty() ? message : message.empty() ? code : code + ": " + message);
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
        if (offerShaderContract(response, message)) return;
        // UX-5a: the line is the sentence; the tooltip (and Copy message)
        // has the engine's whole message as well.
        const QString whole = responseDetail(response);
        showStatus(oneLine(message), message.contains(whole) ? message : message + QLatin1Char('\n') + whole);
        // UX-15b, decision 2: the frozen plan is older than the graph.
        if (framePlanPhysicalFragmentStale(refusalOf(response))) eject_again->show();
    }

    // --- Q-14. The pass's inputs and its shader's declarations disagree:
    // one line that says what to do, and the two ways to do it.
    bool offerShaderContract(const Json &response, const QString &message) {
        const auto error = response.find("error");
        if (error == response.end() || !error->is_object() ||
            submitted_request.method != "edit_preset_pass")
            return false;
        const auto &params = submitted_request.params;
        const auto shader = params.find("shader");
        const auto inputs = params.find("input");
        if (shader == params.end() || !shader->is_object() ||
            !shader->contains("fragment") || !shader->at("fragment").is_string() ||
            inputs == params.end() || !inputs->is_array())
            return false;
        ShaderContractFix fix;
        fix.graph = params.value("graph", std::string{});
        fix.pass = params.value("pass_name", std::string{});
        fix.fragment = shader->at("fragment").get<std::string>();
        for (const auto &input : *inputs)
            if (input.is_string()) fix.inputs.push_back(input.get<std::string>());
        // VI-3b: the count the pass's published declarations give, when they
        // are the fragment the refused edit named.
        const auto notice = shaderInputContractNotice(
            *error, fix.pass, fix.fragment,
            shaderInputDeclaredFor(passInputsOf(fix.pass), fix.fragment));
        if (!notice.matched) return false;
        // Q-15: a declaration the pass cannot supply is removed, not fed.
        fix.unsupplied =
            notice.direction == ShaderInputContractShaderInputUnsupplied;
        fix.binding = notice.binding;
        showStatus(text(notice.line), message);
        offerContractButtons(std::move(fix),
                             shaderInputContractOffers(
                                 notice, ShaderInputContractUsePassthrough));
        return true;
    }

    void offerContractButtons(ShaderContractFix fix, bool use_passthrough) {
        const bool can_open =
            project_root.has_value() &&
            (fix.fragment.starts_with("project://") ||
             fix.fragment == EnginePassthroughFragment);
        contract_fix = std::move(fix);
        contract_use_passthrough->setVisible(use_passthrough);
        contract_open_shader->show();
        contract_open_shader->setEnabled(can_open);
    }

    // VI-3b: the declarations get_frame_plan published for a pass.
    const FramePlanPassInputs *passInputsOf(const std::string &pass) const {
        if (!model) return nullptr;
        const auto node =
            std::ranges::find(model->nodes, pass, &FramePlanNode::name);
        return node == model->nodes.end() || !node->pass_inputs
                   ? nullptr
                   : &*node->pass_inputs;
    }

    void hideShaderContract() {
        contract_fix.reset();
        if (contract_use_passthrough) contract_use_passthrough->hide();
        if (contract_open_shader) contract_open_shader->hide();
    }

    std::vector<FullscreenShaderTemplateInput> contractTemplateInputs(
        const ShaderContractFix &fix) const {
        std::vector<FullscreenShaderTemplateInput> result;
        for (const auto &input : fix.inputs) {
            const auto name = input.substr(0, input.find('@'));
            bool image = true;
            if (model) {
                const auto found = std::ranges::find(
                    model->resources, name, &FramePlanResource::name);
                if (found != model->resources.end() && found->kind == "buffer")
                    image = false;
            }
            result.push_back({.resource = input, .image = image});
        }
        return result;
    }

    // VI-3b. Use engine://passthrough: the inputs the pass was given and that
    // fragment, in one edit. No file is written.
    void useEnginePassthrough() {
        if (!contract_fix) return;
        const auto fix = *contract_fix;
        hideShaderContract();
        submitBuilt([this, fix] {
            return framePlanPassInputsAndShaderRequest(
                authoring, fix.graph, fix.pass, fix.inputs,
                std::string{EnginePassthroughFragment});
        });
        if (pending_edit_request != 0)
            showStatus(owner.tr("Sending the inputs and engine://passthrough "
                                "together..."));
    }

    // VI-3b. engine://passthrough copied into the project's shaders folder,
    // with `source` as its text (the engine's own, or it with its sockets
    // fixed), and the edit that makes the pass draw with the copy. The
    // pass's shader.defines stay as they are. Returns the file written.
    // GL-5d: a pass a graph node draws takes the copy through `graph_fragment`
    // (the node's shader.fragment, the Graph tab's edit), not edit_preset_pass,
    // which only reaches a pass the rendering section declares.
    std::optional<QString> copyEngineShader(
        const std::string &graph, const std::string &pass, std::string_view source,
        const std::function<bool(const QString &reference)> &graph_fragment = {}) {
        try {
            const auto name = engineShaderCopyName(*project_root, pass);
            const auto written = writeShaderTemplate(*project_root, name, source);
            if (!written.created)
                throw std::runtime_error(written.path.string() +
                                         " is already there");
            if (graph_fragment) {
                if (graph_fragment(text(written.reference)))
                    return QStringLiteral("shaders/%1.frag").arg(text(name));
                showStatus(owner.tr("Wrote %1, but the Graph could not move %2 onto it.")
                               .arg(QStringLiteral("shaders/%1.frag").arg(text(name)),
                                    text(pass)));
                return std::nullopt;
            }
            submitBuilt([this, graph, pass, reference = written.reference] {
                return framePlanPassShaderRequest(authoring, graph, pass,
                                                  "fragment", reference);
            });
            return QStringLiteral("shaders/%1.frag").arg(text(name));
        } catch (const std::exception &error) {
            const QString message = QString::fromUtf8(error.what());
            status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
            showStatus(owner.tr("Could not write the shader: %1").arg(oneLine(message)),
                       message);
            return std::nullopt;
        }
    }

    std::optional<std::string> readProjectShader(const std::filesystem::path &path) {
        std::ifstream input{path, std::ios::binary};
        if (!input) return std::nullopt;
        return std::string{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
    }

    bool writeProjectShader(const std::filesystem::path &path,
                            const std::string &source) {
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        output.write(source.data(), static_cast<std::streamsize>(source.size()));
        output.close();
        if (!output) {
            status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
            showStatus(owner.tr("Could not write %1.")
                           .arg(QString::fromStdWString(path.wstring())));
            return false;
        }
        // The watcher would see it too; this does not wait for it.
        reload_follower->sourceChanged();
        return true;
    }

    // --- VI-3b. Fix sockets: the variadic group becomes one fixed
    // declaration per connected input, in the pass's shader file.
    void fixSockets(const FramePlanNodeKey &node,
                    const std::function<bool(const QString &reference)> &graph_fragment = {}) {
        if (!model || node.graph != model->graph) {
            showStatus(owner.tr("The frame plan has no pass %1: refresh it.").arg(text(node.name)));
            return;
        }
        const auto fix = framePlanSocketFix(authoring, *model, node.graph, node.name);
        if (!fix.refusal.empty()) {
            status->setStyleSheet({});
            showStatus(text(fix.refusal));
            return;
        }
        if (!project_root) {
            showStatus(owner.tr("Open a project first: the shader is written "
                                "into its shaders folder."));
            return;
        }
        if (fix.fragment == EnginePassthroughFragment) {
            const auto rewritten = fixShaderSockets(
                *engineFragmentSource(fix.fragment), fix.first, fix.inputs,
                fix.taken);
            if (!rewritten.ok()) {
                showStatus(text(node.name + ": " + rewritten.refusal));
                return;
            }
            const auto file = copyEngineShader(node.graph, node.name,
                                               rewritten.source, graph_fragment);
            if (file && (pending_edit_request != 0 || graph_fragment))
                showStatus(owner.tr("Copied engine://passthrough to %1 with "
                                    "its sockets fixed; sending the new "
                                    "fragment...")
                               .arg(*file));
            return;
        }
        const auto path = projectFragmentSourcePath(*project_root, fix.fragment);
        const auto source = path ? readProjectShader(*path) : std::nullopt;
        if (!source) {
            showStatus(owner.tr("Could not read %1.").arg(text(fix.fragment)));
            return;
        }
        const auto rewritten =
            fixShaderSockets(*source, fix.first, fix.inputs, fix.taken);
        if (!rewritten.ok()) {
            showStatus(text(node.name + ": " + rewritten.refusal));
            return;
        }
        if (!writeProjectShader(*path, rewritten.source)) return;
        status->setStyleSheet({});
        showStatus(owner.tr("Fixed %1's sockets in %2; the shader reloads.")
                       .arg(text(node.name), text(fix.fragment)));
    }

    // The source of a pass's project shader when it holds sockets Fix
    // sockets wrote.
    std::optional<std::pair<std::filesystem::path, std::string>>
    fixedSocketsSource(const FramePlanNodeKey &node) {
        if (!project_root) return std::nullopt;
        const auto *inputs = passInputsOf(node.name);
        if (inputs == nullptr) return std::nullopt;
        const auto path = projectFragmentSourcePath(*project_root, inputs->fragment);
        if (!path) return std::nullopt;
        auto source = readProjectShader(*path);
        if (!source || !shaderHasFixedSockets(*source)) return std::nullopt;
        return std::pair{*path, std::move(*source)};
    }

    // Make sockets variadic: the block Fix sockets wrote, back to the line it
    // replaced. A declaration written by hand is never touched.
    void makeSocketsVariadic(const FramePlanNodeKey &node) {
        if (!model || node.graph != model->graph) {
            showStatus(owner.tr("The frame plan has no pass %1: refresh it.").arg(text(node.name)));
            return;
        }
        const auto found = fixedSocketsSource(node);
        if (!found) {
            showStatus(owner.tr("%1's shader has no sockets Studio fixed.")
                           .arg(text(node.name)));
            return;
        }
        const auto rewritten = variadicShaderSockets(found->second);
        if (!rewritten.ok()) {
            showStatus(text(node.name + ": " + rewritten.refusal));
            return;
        }
        if (!writeProjectShader(found->first, rewritten.source)) return;
        status->setStyleSheet({});
        showStatus(owner.tr("Made %1's sockets variadic again; the shader "
                            "reloads.")
                       .arg(text(node.name)));
    }

    // VI-3b. A click on a connected socket of a pass that draws with
    // engine://passthrough: show that input. On any other pass a click does
    // nothing.
    void showPassthroughInput(const FramePlanNodeKey &node, std::size_t binding) {
        if (!model || node.graph != model->graph ||
            !framePlanPassthroughShown(authoring, node.graph, node.name))
            return;
        submitBuilt([this, graph = node.graph, pass = node.name, binding] {
            return model ? framePlanPassthroughInputRequest(authoring, *model,
                                                            graph, pass, binding)
                         : FramePlanEditRequest{};
        });
    }

    // Open the shader: the file, and on the clipboard the lines to add.
    void openContractShader() {
        if (!contract_fix || !project_root) return;
        const auto &fix = *contract_fix;
        if (fix.fragment == EnginePassthroughFragment) {
            // VI-3b: the engine's shader is copied into the project first,
            // and the pass is moved onto the copy; the copy is what opens.
            const auto graph = fix.graph;
            const auto pass = fix.pass;
            const auto name = engineShaderCopyName(*project_root, pass);
            hideShaderContract();
            const auto file = copyEngineShader(
                graph, pass, *engineFragmentSource(EnginePassthroughFragment));
            if (!file) return;
            const auto path = shaderTemplatePath(*project_root, name);
            if (!shader_source_open_action || !shader_source_open_action(path)) {
                showStatus(owner.tr("The desktop could not open %1.")
                               .arg(QString::fromStdWString(path.wstring())));
                return;
            }
            status->setStyleSheet({});
            showStatus(owner.tr("Copied engine://passthrough to %1 and opened it.")
                           .arg(*file));
            return;
        }
        const auto path = projectFragmentSourcePath(*project_root, fix.fragment);
        if (!path) {
            showStatus(owner.tr("%1 is not a file of this project.")
                           .arg(text(fix.fragment)));
            return;
        }
        if (fix.unsupplied) {
            // Q-15: nothing to add -- the declaration is what goes.
            if (!shader_source_open_action || !shader_source_open_action(*path)) {
                showStatus(owner.tr("The desktop could not open %1.")
                               .arg(QString::fromStdWString(path->wstring())));
                return;
            }
            status->setStyleSheet({});
            showStatus(text(shaderInputUnsuppliedOpenedLine(fix.binding)) +
                       owner.tr(" Then connect the input again."));
            return;
        }
        // VI-3b: the lines start after the fixed image declarations the pass's
        // shader publishes (none known: from input 0).
        std::size_t first = 0;
        if (const auto *inputs = passInputsOf(fix.pass);
            inputs != nullptr && shaderInputDeclarationsAreFor(*inputs, fix.fragment))
            first = shaderInputFixedImageCount(*inputs);
        const auto inputs = contractTemplateInputs(fix);
        QApplication::clipboard()->setText(
            text(fullscreenShaderInputSnippet(inputs, first)));
        if (!shader_source_open_action || !shader_source_open_action(*path)) {
            showStatus(owner.tr("The desktop could not open %1.")
                           .arg(QString::fromStdWString(path->wstring())));
            return;
        }
        status->setStyleSheet({});
        showStatus(owner.tr("The declarations are on the clipboard: add them to "
                            "the shader and save, and it reloads by itself. "
                            "Then connect the input again."));
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
            // LP-4e: what the decompose made is named here, not in the commit.
            if (submitted_method == "decompose_material_pass")
                decomposed_loop = framePlanDecomposedLoop(response);
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
            // G-4 / RS-1: the change is written, but the engine runs it only
            // after a restart (material_route_default is read once at start;
            // a material's physical contract is built once). Restart the
            // engine on the same project; the Frame Plan comes back when it
            // is ready again. RSX: read the way every panel reads a commit.
            if (const auto completion = readRenderEditCompletion(response);
                completion.restart_required) {
                const QString reason = text(completion.restart_line);
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
            commit_notice = std::exchange(submitted_notice, {});
            showStatus(commit_notice.isEmpty()
                           ? owner.tr("Canvas change committed.")
                           : commit_notice);
            // LP-4e: the loop it made is shown where it is edited.
            if (submitted_method == "decompose_material_pass") {
                const auto loop = std::exchange(decomposed_loop, {});
                if (!loop.empty() && loop_decomposed_action)
                    loop_decomposed_action(text(loop));
            }
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
            runWhenRead();
            return true;
        }
        if (request_id == pending_task_request) {
            pending_task_request = 0;
            task_authoring = {};
            task_reread = false;
            resumeRetry();
            runWhenRead();  // GL-5f, decision 2
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
        // VI-3b: a pass whose shader declares no input takes none; the drop
        // is answered with the way out instead of being sent.
        if (task.empty() && connect && names.size() == 1) {
            const auto node = std::ranges::find(model->nodes, consumer.name,
                                                &FramePlanNode::name);
            if (node != model->nodes.end() &&
                framePlanNodeDeclaresNoInputs(*node)) {
                last_drop_pass = consumer.name;
                dropOnSocket(consumer, names.front(), FramePlanSocketDrop{});
                return;
            }
        }
        submitBuilt([this, graph = model->graph, pass = consumer.name, task,
                     names, connect] {
            return task.empty()
                       ? framePlanPassResourceRequest(authoring, graph, pass,
                                                      names, connect)
                       : framePlanTaskResourceRequest(task_authoring, task,
                                                      names, connect);
        });
    }

    // VI-3a: a drop on a pass whose sockets come from its shader.
    void dropOnSocket(const FramePlanNodeKey &consumer,
                      const std::string &resource,
                      const FramePlanSocketDrop &drop) {
        if (!model || consumer.graph != model->graph) return;
        submitBuilt([this, graph = model->graph, pass = consumer.name,
                     resource, drop] {
            return model ? framePlanSocketConnectRequest(authoring, *model,
                                                         graph, pass,
                                                         resource, drop)
                         : FramePlanEditRequest{};
        });
    }

    // VI-3a: one input of such a pass taken out -- a socket dragged away,
    // Delete on its edge, or Disconnect after a refused reload.
    void cutSocketInput(const FramePlanNodeKey &consumer,
                        std::size_t input_index) {
        if (!model || consumer.graph != model->graph) return;
        submitBuilt([this, graph = model->graph, pass = consumer.name,
                     input_index] {
            return model ? framePlanSocketDisconnectRequest(
                               authoring, *model, graph, pass, input_index)
                         : FramePlanEditRequest{};
        });
    }

    // --- VI-3a rule 4. A reload the engine refused for a pass because the
    // new shader no longer declares a binding the pass feeds: the pass keeps
    // its previous shader, its socket and line end are red, and the status
    // line says so with the two ways out. Shown right after the reload, and
    // again whenever that pass is picked, for as long as the refusal stands.
    struct ReloadRefusalFix {
        std::string graph;
        std::string pass;
        std::string fragment;
        std::size_t input_index = 0;
        std::string line;
    };
    std::optional<ReloadRefusalFix> reload_refusal_fix;

    std::optional<ReloadRefusalFix> reloadRefusalFor(const FramePlanNode &node) {
        if (!model || !node.pass_inputs || !node.pass_inputs->reload_refused)
            return std::nullopt;
        const auto sockets = framePlanInputSockets(
            *model, node, framePlanPassInputList(authoring, model->graph,
                                                 node.name));
        const auto *removed = framePlanRemovedSocket(sockets);
        if (removed == nullptr || !removed->input_index) return std::nullopt;
        return ReloadRefusalFix{
            .graph = model->graph,
            .pass = node.name,
            .fragment = node.pass_inputs->fragment,
            .input_index = *removed->input_index,
            .line = framePlanReloadRefusalLine(node.name, *removed),
        };
    }

    void updateReloadRefusal() {
        std::optional<ReloadRefusalFix> fix;
        if (model) {
            const auto &selected = logical_scene->selectedNode();
            if (selected && selected->graph == model->graph) {
                const auto node = std::ranges::find(
                    model->nodes, selected->name, &FramePlanNode::name);
                if (node != model->nodes.end()) fix = reloadRefusalFor(*node);
            }
            if (!fix && announce_reload_refusal)
                for (const auto &node : model->nodes)
                    if ((fix = reloadRefusalFor(node))) break;
        }
        if (!fix) {
            // Only once nothing is refused any more does the next reload
            // stop being news.
            if (model && std::ranges::none_of(model->nodes, [](const auto &node) {
                    return node.pass_inputs && node.pass_inputs->reload_refused;
                }))
                announce_reload_refusal = false;
            const bool shown = reload_refusal_fix.has_value();
            reload_refusal_fix.reset();
            refusal_disconnect->hide();
            refusal_open_shader->hide();
            if (shown && status->text() == shown_refusal_line) {
                status->setStyleSheet({});
                showStatus({});
            }
            shown_refusal_line.clear();
            return;
        }
        shown_refusal_line = text(fix->line);
        status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        showStatus(shown_refusal_line);
        refusal_disconnect->show();
        refusal_open_shader->show();
        refusal_open_shader->setEnabled(
            project_root.has_value() &&
            projectFragmentSourcePath(*project_root, fix->fragment).has_value());
        reload_refusal_fix = std::move(fix);
    }

    void disconnectRefusedInput() {
        if (!reload_refusal_fix) return;
        const auto fix = *reload_refusal_fix;
        cutSocketInput(FramePlanNodeKey{fix.graph, fix.pass}, fix.input_index);
    }

    // Q-14's Open, for a project:// shader: the file, in the desktop's editor.
    void openRefusedShader() {
        if (!reload_refusal_fix || !project_root) return;
        const auto &fix = *reload_refusal_fix;
        const auto path = projectFragmentSourcePath(*project_root, fix.fragment);
        if (!path) {
            showStatus(owner.tr("%1 is not a file of this project.")
                           .arg(text(fix.fragment)));
            return;
        }
        if (!shader_source_open_action || !shader_source_open_action(*path))
            showStatus(owner.tr("The desktop could not open %1.")
                           .arg(QString::fromStdWString(path->wstring())));
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

    // LP-4b: a pick in a node's View menu is one edit: edit_preset_pass for
    // a pass, edit_compute_task (the whole declaration) for a task.
    void pickView(const FramePlanNodeKey &node, const std::string &axis,
                  std::optional<std::uint64_t> value) {
        if (!model || node.graph != model->graph) return;
        const auto task = authoredTask(node.name);
        if (!task.empty()) {
            submitBuilt([this, task, axis, value] {
                return framePlanTaskViewSelectRequest(task_authoring, task,
                                                      axis, value);
            });
            return;
        }
        submitBuilt([this, graph = model->graph, pass = node.name, axis,
                     value] {
            return framePlanPassViewSelectRequest(authoring, graph, pass, axis,
                                                  value);
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
        // LP-4e: a decompose refusal reads by its code and reason.
        if (error != response.end() && error->is_object() &&
            submitted_method == "decompose_material_pass")
            return text(framePlanDecomposeRejection(*error));
        if (error != response.end() && error->is_object() &&
            (submitted_method == "edit_render_targets" ||
             error->value("code", std::string{}).starts_with("render_target_")))
            return text(framePlanRenderTargetRejection(*error,
                                                       authoring.preset_name));
        // G-6: a graph variant that refused the edit is named, whatever the
        // edit was. LP-4a: a view_select refusal reads as its sentence;
        // LP-2: and so does a loop's (the draw-loop ones come back with their
        // own code, whatever edit hit them).
        if (error != response.end() && error->is_object() &&
            (error->contains("variant") ||
             !viewSelectRefusalDescription(*error).empty() ||
             !graphLoopRefusalDescription(*error).empty()))
            return text(renderAuthoringRefusalLine(*error));
        // GL-3a, decision 8: a preset that declares target_planning itself.
        if (error != response.end() && error->is_object() &&
            submitted_method == "edit_target_planning" &&
            error->value("code", std::string{}) == "rendering_key_not_allowed_here")
            return owner.tr("The pipeline preset declares target_planning, which this project "
                            "cannot override: fork the preset to set it "
                            "(rendering_key_not_allowed_here).");
        if (error != response.end() && error->is_object() &&
            submitted_method == "add_authored_pass") {
            // G-4: name the materials no pass would draw, and the fix.
            if (const auto notice = materialRouteUnservedNotice(*error);
                !notice.empty())
                return owner.tr("The engine refused the pass: %1").arg(text(notice));
            return owner.tr("The engine refused the pass: %1")
                .arg(text(engineRefusalLine(*error)));
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
        // GL-5b, decision 2: the Graph's New render target… opens the same way.
        presentNewRenderTarget(renderTargetForm(), authoring,
                               std::exchange(new_target_name, {}));
    }

    void openRenderTargetForm(const std::string &name) {
        if (authoring.findRenderTarget(name) == nullptr) {
            // The context may simply be older than the plan on screen: ask
            // for it once and open the form when it answers.
            pending_form_target = name;
            requestAuthoringContext();
            return;
        }
        // GL-5d, decision 3: the Graph's Edit target… opens it the same way.
        (void)presentRenderTargetEdit(renderTargetForm(), authoring, name);
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

    // GL-1. A node of this plan that is a pass (not an anchor).
    const FramePlanNode *planPassOf(const FramePlanNodeKey &node) const {
        if (!model || node.graph != model->graph) return nullptr;
        const auto found = std::ranges::find(model->nodes, node.name, &FramePlanNode::name);
        return found != model->nodes.end() && found->kind != "anchor" ? &*found : nullptr;
    }

    // LP-4c. The loop_origin of a pass or task a graph.loop expanded.
    const FramePlanLoopOrigin *loopOriginOf(const FramePlanNodeKey &node) const {
        if (!model || node.graph != model->graph) return nullptr;
        const auto found = std::ranges::find(model->nodes, node.name, &FramePlanNode::name);
        return found != model->nodes.end() && found->loop_origin ? &*found->loop_origin
                                                                 : nullptr;
    }

    // --- LP-4e: Decompose into loop.

    // Whether the entry is about this node at all: a material pass of the
    // plan, or one a draw_loop already draws in (offered disabled, with its
    // reason). Not a task, an anchor, a fullscreen pass or a loop's copy.
    bool decomposeAbout(const FramePlanNodeKey &node) const {
        if (!model || node.graph != model->graph) return false;
        const auto found = std::ranges::find(model->nodes, node.name, &FramePlanNode::name);
        if (found == model->nodes.end() || found->loop_origin) return false;
        if (framePlanDrawLoopOf(*model, node.name) != nullptr) return true;
        const auto *row = authoring.find(node.graph, node.name);
        return row != nullptr && row->declaration.value("type", std::string{}) == "material";
    }

    // Build, confirm, send. The request is built again when Decompose is
    // pressed, from the context as it is then.
    void decompose(const std::string &graph, const std::string &pass) {
        const auto build = [this, graph, pass] {
            return framePlanDecomposeRequest(authoring, model ? &*model : nullptr, graph, pass);
        };
        const auto request = build();
        if (!request.sendable()) {
            const QString reason = text(request.refusal);
            status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
            showStatus(oneLine(reason), reason);
            return;
        }
        if (pending_edit_request != 0 || !pending_ticket.isEmpty()) {
            showStatus(owner.tr("Wait for the current change to finish."));
            return;
        }
        if (decompose_prompt) {
            decompose_prompt->raise();
            return;
        }
        auto *prompt = new QMessageBox(QMessageBox::Warning, owner.tr("Decompose into loop"),
                                       text(framePlanDecomposeConfirmation(pass)),
                                       QMessageBox::NoButton, &owner);
        prompt->setObjectName(QStringLiteral("pelican.framePlan.decomposeConfirm"));
        // Named, so a scenario answers the way a person does.
        auto *accept = prompt->addButton(owner.tr("Decompose"), QMessageBox::AcceptRole);
        accept->setObjectName(QStringLiteral("pelican.framePlan.decomposeConfirm.ok"));
        auto *cancel = prompt->addButton(QMessageBox::Cancel);
        cancel->setObjectName(QStringLiteral("pelican.framePlan.decomposeConfirm.cancel"));
        prompt->setDefaultButton(cancel);
        prompt->setEscapeButton(cancel);
        prompt->setAttribute(Qt::WA_DeleteOnClose);
        decompose_prompt = prompt;
        QObject::connect(prompt, &QMessageBox::buttonClicked, &owner,
                         [this, accept, build, pass](QAbstractButton *button) {
                             if (button != accept) {
                                 showStatus(owner.tr("%1 is left as it is.").arg(text(pass)));
                                 return;
                             }
                             submitBuilt(build);
                         });
        prompt->open();
    }

    // Decision 5: the materials of a draw_loop past the plan's first page, a
    // page at a time, for the details' "Read more".
    void requestLoopMaterials(const std::string &loop) {
        if (!model || pending_loop_page_request != 0) return;
        const auto found = std::ranges::find(model->loops, loop, &FramePlanLoop::node);
        if (found == model->loops.end()) return;
        const auto held = found->offset + found->elements.size() + loop_more[loop].size();
        QString error;
        pending_loop_page_request = viewport.requestRpc(
            QStringLiteral("get_frame_plan"),
            QJsonObject{{QStringLiteral("loop_elements"),
                         QJsonObject{{QStringLiteral("node"), text(loop)},
                                     {QStringLiteral("offset"), static_cast<qint64>(held)},
                                     {QStringLiteral("limit"), 256}}}},
            &error);
        if (pending_loop_page_request == 0) {
            showStatus(oneLine(error), error);
            return;
        }
        loop_page_node = loop;
    }

    void receiveLoopMaterials(const QByteArray &result_json) {
        const auto loop = std::exchange(loop_page_node, {});
        try {
            const auto page = buildFramePlanModel(result_json.toStdString());
            const auto found = std::ranges::find(page.loops, loop, &FramePlanLoop::node);
            if (!model || found == page.loops.end()) return;
            const auto held = std::ranges::find(model->loops, loop, &FramePlanLoop::node);
            if (held == model->loops.end()) return;
            auto &more = loop_more[loop];
            // Spliced only where the held list ends: a plan that moved in
            // between would put materials in the wrong place.
            if (found->offset != held->offset + held->elements.size() + more.size()) return;
            more.insert(more.end(), found->elements.begin(), found->elements.end());
            populateLogicalDetails();
        } catch (const std::exception &error) {
            const QString message = QString::fromUtf8(error.what());
            showStatus(oneLine(message), message);
        }
    }

    // GL-2b, decision 4. The frame plan nodes a row of a tab names: its
    // own, else its nearest parent's. Only passes (not anchors) are kept.
    static constexpr int RowNodesRole = Qt::UserRole + 71;
    void setRowNodes(QTreeWidgetItem *item, const std::vector<std::string> &names) const {
        QStringList kept;
        for (const auto &name : names)
            if (model && !kept.contains(text(name)) &&
                planPassOf(FramePlanNodeKey{model->graph, name}) != nullptr)
                kept.push_back(text(name));
        if (!kept.isEmpty()) item->setData(0, RowNodesRole, kept);
    }
    QStringList rowNodes(const QTreeWidgetItem *item) const {
        for (; item != nullptr; item = item->parent()) {
            const auto names = item->data(0, RowNodesRole).toStringList();
            if (!names.isEmpty()) return names;
        }
        return {};
    }
    void openRowNode(const QString &name) {
        if (!model) return;
        openInGraph(FramePlanNodeKey{model->graph, name.toStdString()});
    }
    // The row's right-click: Open in Graph, a submenu of passes when the
    // row names several, switched off when it names none (the whole graph).
    void rowMenu(QTreeWidget &tree, const QPoint &at) {
        QTreeWidgetItem *item = tree.itemAt(at);
        if (item == nullptr) return;
        tree.setCurrentItem(item);
        const auto names = rowNodes(item);
        QMenu menu;
        addPlanningEntries(menu, item);
        addLowerLayerEntries(menu, tree, item);
        if (names.size() > 1) {
            QMenu *each = menu.addMenu(owner.tr("Open in Graph"));
            for (const auto &name : names) {
                QAction *open = each->addAction(name);
                QObject::connect(open, &QAction::triggered, &owner,
                                 [this, name] { openRowNode(name); });
            }
        } else {
            QAction *open = menu.addAction(owner.tr("Open in Graph"));
            open->setEnabled(names.size() == 1);
            if (names.isEmpty())
                open->setToolTip(owner.tr("This row names no pass: it is about the whole graph."));
            else
                QObject::connect(open, &QAction::triggered, &owner,
                                 [this, name = names.front()] { openRowNode(name); });
        }
        menu.exec(tree.viewport()->mapToGlobal(at));
    }
    // --- GL-3a, decision 8: Serial / Isolate on a pass's row, No alias on a
    // resource's, written with edit_target_planning by the expanded name.
    static constexpr int PlanningSubjectRole = Qt::UserRole + 72;
    void setPlanningSubject(QTreeWidgetItem *item, const char *kind, const std::string &name,
                            bool loop_copy) const {
        item->setData(0, PlanningSubjectRole,
                      QStringList{QString::fromLatin1(kind), text(name),
                                  loop_copy ? QStringLiteral("loop") : QString{}});
    }
    // {kind, name, "loop"?} of a row, or empty.
    static QStringList planningSubject(const QTreeWidgetItem *item) {
        return item != nullptr ? item->data(0, PlanningSubjectRole).toStringList() : QStringList{};
    }
    void addPlanningEntries(QMenu &menu, const QTreeWidgetItem *item) {
        const auto subject = planningSubject(item);
        if (subject.size() < 2 || !model || !edit.editTargetPlanning) return;
        if (subject.value(2) == QLatin1String{"loop"}) {
            // LP-1a refuses a loop's copy by name: the per-element way.
            QAction *why = menu.addAction(text(std::string{graphTargetPlanningLoopLine}));
            why->setObjectName(QStringLiteral("pelican.framePlan.planning.loopLine"));
            why->setEnabled(false);
            menu.addSeparator();
            return;
        }
        const auto kind = subject.value(0).toStdString();
        const auto name = subject.value(1).toStdString();
        const bool can = authoring.received && pending_edit_request == 0 && pending_ticket.isEmpty();
        const auto entry = [&](QAction *action, const char *field) {
            action->setChecked(planning.flag(kind, name, field));
            action->setEnabled(can);
            menu.addAction(action);
        };
        if (kind == "resource") {
            entry(planning_no_alias, "no_alias");
        } else {
            entry(planning_serial, "serial");
            entry(planning_isolate, "isolate");
        }
        menu.addSeparator();
    }
    void togglePlanning(const char *field) {
        auto *tree = qobject_cast<QTreeWidget *>(tabs->currentWidget());
        const auto subject = planningSubject(tree != nullptr ? tree->currentItem() : nullptr);
        if (subject.size() < 2 || !model) {
            showStatus(owner.tr("Select a pass or a resource row first."));
            return;
        }
        if (subject.value(2) == QLatin1String{"loop"}) {
            showStatus(text(std::string{graphTargetPlanningLoopLine}) + QLatin1Char('.'));
            return;
        }
        const auto kind = subject.value(0).toStdString();
        const auto name = subject.value(1).toStdString();
        const std::string wanted{field};
        if ((kind == "resource") != (wanted == "no_alias")) {
            showStatus(kind == "resource" ? owner.tr("A resource takes No alias only.")
                                          : owner.tr("A pass takes Serial and Isolate."));
            return;
        }
        if (!authoring.received) {
            showStatus(owner.tr("The authoring context has not been read yet."));
            return;
        }
        const bool on = !planning.flag(kind, name, wanted);
        FramePlanEditRequest request;
        request.method = "edit_target_planning";
        request.params = graphTargetPlanningParams(authoring.source_digest, model->graph, kind,
                                                   name, wanted, on);
        const std::string title = wanted == "no_alias" ? "No alias"
                                  : wanted == "serial" ? "Serial"
                                                       : "Isolate";
        request.notice = title + (on ? " set on " : " taken off ") + name + ".";
        submit(request);
    }
    // --- UX-15b, decisions 1 and 2: a backend pinned from its Backends row,
    // and the physical plan frozen from the Physical plan tab. Each is one
    // edit (EL-2), which RU-1's Undo takes back.
    static constexpr int BackendCandidateRole = Qt::UserRole + 73;
    static std::string candidateOf(const QTreeWidgetItem *item) {
        for (; item != nullptr; item = item->parent()) {
            const auto name = item->data(0, BackendCandidateRole).toString();
            if (!name.isEmpty()) return name.toStdString();
        }
        return {};
    }
    static FramePlanEditRequest noPlanYet() {
        FramePlanEditRequest request;
        request.refusal = "The frame plan has not arrived yet.";
        return request;
    }
    void pinCandidate(const std::string &candidate) {
        submitBuilt([this, candidate] {
            return model ? framePlanBackendPinRequest(authoring, *model, candidate) : noPlanYet();
        });
    }
    void pinPickedCandidate() {
        const auto candidate = candidateOf(backends->currentItem());
        if (candidate.empty()) {
            showStatus(owner.tr("Pick a candidate row on the Backends tab first."));
            return;
        }
        pinCandidate(candidate);
    }
    void unpinBackend() {
        submitBuilt([this] {
            return model ? framePlanBackendUnpinRequest(authoring, *model) : noPlanYet();
        });
    }
    void freezePhysicalPlan() {
        submitBuilt([this] {
            return model ? framePlanPhysicalFreezeRequest(authoring, *model) : noPlanYet();
        });
    }
    void removeFrozenPlan() {
        submitBuilt([this] {
            return model ? framePlanPhysicalRemoveRequest(authoring, *model) : noPlanYet();
        });
    }
    // A candidate row's right-click: Pin this candidate, switched off (with
    // the reason as its tooltip) where it cannot be sent; Unpin while a pin
    // applies.
    void addLowerLayerEntries(QMenu &menu, const QTreeWidget &tree, const QTreeWidgetItem *item) {
        if (&tree != backends || !model) return;
        const auto candidate = candidateOf(item);
        if (candidate.empty()) return;
        const auto request = framePlanBackendPinRequest(authoring, *model, candidate);
        const bool idle = pending_edit_request == 0 && pending_ticket.isEmpty();
        QAction *pin = menu.addAction(owner.tr("Pin this candidate"));
        pin->setObjectName(QStringLiteral("pelican.framePlan.backend.pinEntry"));
        pin->setEnabled(request.sendable() && idle);
        pin->setToolTip(text(request.refusal));
        QObject::connect(pin, &QAction::triggered, &owner,
                         [this, candidate] { pinCandidate(candidate); });
        if (!model->physical_plan.pinned_backend_candidate.empty()) {
            QAction *unpin = menu.addAction(owner.tr("Unpin"));
            unpin->setEnabled(idle);
            QObject::connect(unpin, &QAction::triggered, &owner, [this] { unpinBackend(); });
        }
        menu.setToolTipsVisible(true);
        menu.addSeparator();
    }
    // A row across the whole tree, first, holding `line` and its buttons.
    QLabel *addBand(QTreeWidget *tree, const QString &line, const char *label_name,
                    std::initializer_list<std::pair<QAction *, const char *>> buttons) {
        auto *item = new QTreeWidgetItem;
        tree->insertTopLevelItem(0, item);
        item->setFirstColumnSpanned(true);
        auto *band = new QWidget;
        auto *row = new QHBoxLayout(band);
        row->setContentsMargins(4, 2, 4, 2);
        auto *label = new QLabel(line, band);
        label->setObjectName(QString::fromLatin1(label_name));
        row->addWidget(label);
        for (const auto &[action, name] : buttons) {
            auto *button = new QPushButton(action->text(), band);
            button->setObjectName(QString::fromLatin1(name));
            QObject::connect(button, &QPushButton::clicked, action, &QAction::trigger);
            row->addWidget(button);
        }
        row->addStretch(1);
        tree->setItemWidget(item, 0, band);
        bands.emplace_back(band);
        return label;
    }
    void requestPlanning() {
        if (!edit.requestRootDocument || authoring.source_reference.empty() ||
            pending_planning_request != 0)
            return;
        QString error;
        pending_planning_request =
            edit.requestRootDocument(text(authoring.source_reference), &error);
    }
    bool receivePlanning(qint64 request_id, const QByteArray &bytes) {
        if (request_id == 0 || request_id != pending_planning_request) return false;
        pending_planning_request = 0;
        const auto response =
            Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
        if (!response.is_object() || !model) return true;
        for (const auto &document : response.value("documents", Json::array())) {
            if (!document.is_object() ||
                document.value("reference", std::string{}) != authoring.source_reference)
                continue;
            const auto content =
                Json::parse(document.value("content", std::string{}), nullptr, false);
            planning = GraphTargetPlanning::fromRoot(content, model->graph);
        }
        return true;
    }

    // GL-2b, decision 3: the Graph tab's Layers row picks a plan node here.
    // UX-11, decision 4.
    void showMaterialRoutes(const QString &pass) {
        tabs->setCurrentWidget(materials);
        materials->clearSelection();
        QTreeWidgetItem *first = nullptr;
        for (int index = 0; index < materials->topLevelItemCount(); ++index) {
            auto *item = materials->topLevelItem(index);
            if (item->text(1) != pass) continue;
            item->setSelected(true);
            if (first == nullptr) first = item;
        }
        if (first == nullptr) {
            showStatus(owner.tr("The frame plan routes no material to %1.").arg(pass));
            return;
        }
        materials->setCurrentItem(first, 0, QItemSelectionModel::NoUpdate);
        materials->scrollToItem(first);
        showStatus(owner.tr("The material routes to %1 are picked.").arg(pass));
    }

    void showPlanNode(const std::string &name) {
        tabs->setCurrentWidget(logical_splitter);
        if (!model || !logical_scene->selectPlanNode(FramePlanNodeKey{model->graph, name})) {
            showStatus(owner.tr("The frame plan has no node %1: refresh it.").arg(text(name)));
            return;
        }
        QRectF picked;
        for (QGraphicsItem *item : logical_scene->selectedItems())
            picked = picked.isValid() ? picked.united(item->sceneBoundingRect())
                                      : item->sceneBoundingRect();
        if (picked.isValid()) logical->centerOn(picked.center());
        showStatus(owner.tr("%1 is picked.").arg(text(name)));
    }

    // --- GL-5c: the Frame Plan as a mode of the Graph.

    // Decisions 1 and 2. An Add on a root that is a graph and selects no
    // preset (Blank's) places the palette's node -- `op` -- on the Graph tab,
    // in the graph's `nodes`, as the palette does; on a root that selects a
    // preset, `own` runs: the Frame Plan's form, as before. A context not
    // read yet is read first.
    void addThrough(const std::string &op, std::function<void()> own) {
        if (graph_place_action) {
            if (!authoring.received) {
                pending_graph_place = op;
                pending_graph_fallback = std::move(own);
                requestAuthoringContext();
                showStatus(owner.tr("Reading the render authoring context..."));
                return;
            }
            if (authoring.direct && graph_place_action(text(op))) return;
        }
        if (own) own();
    }

    // --- GL-5d: the Graph's Order… / Fix sockets / Make sockets variadic are
    // this panel's own, run from the Graph tab -- while this panel may be out
    // of view, when it reads nothing on its own (N-8). Each waits for the plan
    // and the authoring context read again, and the status lines it leaves go
    // to `said` (the Graph's notice) as well as to this panel's row.

    // Order...: the plan's nodes, in the plan's order, for the Features tab's
    // node order dialog.
    void orderNodes() {
        if (!model || !node_order_action) {
            showStatus(owner.tr("Node ordering is unavailable: no frame plan has been read."));
            return;
        }
        if (!model->runtime_generation || *model->runtime_generation == 0) {
            showStatus(owner.tr("Node ordering needs a published runtime generation"), owner.tr("Node ordering requires a published runtime generation. Refresh the Frame Plan first."));
            return;
        }
        QStringList names;
        for (const auto &node : model->nodes) names.push_back(QString::fromStdString(node.name));
        node_order_action(QString::fromStdString(model->graph), names, *model->runtime_generation);
    }

    struct WhenRead {
        std::function<void()> action;
        std::function<void(const QString &)> said;
        bool tasks = false;  // GL-5f, decision 2
    };
    std::vector<WhenRead> when_read;
    // Who hears this panel's status lines: the caller of the action running,
    // and of the edit it sent, until that edit is done.
    std::function<void(const QString &)> status_said;
    bool running_when_read = false;

    void whenRead(std::function<void()> action, std::function<void(const QString &)> said,
                  bool tasks = false) {
        when_read.push_back({std::move(action), std::move(said), tasks});
        if (planReadable()) readPlanAgain();
        if (pending_authoring_request != 0) authoring_reread = true;
        else requestAuthoringContext(true);
        // GL-5f, decision 2: the task catalog, read only while shown otherwise.
        if (tasks) {
            if (pending_task_request != 0) task_reread = true;
            else requestTaskAuthoring(true);
        }
        // Whatever could not be asked for, what is in hand is all there is.
        runWhenRead();
    }

    // Once neither read is on its way.
    void runWhenRead() {
        if (when_read.empty() || pending_request != 0 || plan_reread ||
            pending_authoring_request != 0 || authoring_reread)
            return;
        // GL-5f, decision 2.
        if ((pending_task_request != 0 || task_reread) &&
            std::ranges::any_of(when_read, &WhenRead::tasks))
            return;
        auto actions = std::exchange(when_read, {});
        for (auto &entry : actions) {
            status_said = std::move(entry.said);
            running_when_read = true;
            if (entry.action) entry.action();
            running_when_read = false;
            if (pending_edit_request == 0 && pending_ticket.isEmpty()) status_said = {};
        }
    }

    // Decision 3. What Edit pass / Edit task and a double-click open on the
    // Graph tab: a loop's copy (its body node), a task a render.compute node
    // made, and a pass the Graph edits (a graph node's, or the rendering
    // section's in its detail column). A preset's or a feature's pass, read
    // only there, opens in the Pass Editor as before.
    bool editsInGraph(const FramePlanNodeKey &node) const {
        if (!graph_edits_pass) return false;
        if (loopOriginOf(node) != nullptr || graphNodeOf(node) != nullptr) return true;
        const auto *pass = planPassOf(node);
        return pass != nullptr && pass->kind != "compute" && graph_edits_pass(text(pass->name));
    }

    // Decision 5: the entries that go to the Graph say so.
    void syncDelegationTips() {
        const bool graph = graph_place_action && authoring.received && authoring.direct;
        const auto tip = [graph](const QString &op, const QString &own) {
            return graph ? QObject::tr("Opens in the Graph: the palette's %1, placed in the "
                                       "graph's nodes and picked there.")
                               .arg(op)
                         : own;
        };
        const QString material = tip(QStringLiteral("render.material_pass"),
                                     QObject::tr("The material pass form: the pass goes into "
                                                 "a fragment of the project."));
        if (add_material_button) add_material_button->setToolTip(material);
        if (add_material_action) add_material_action->setToolTip(material);
        if (add_fullscreen_action)
            add_fullscreen_action->setToolTip(
                tip(QStringLiteral("render.fullscreen"),
                    QObject::tr("The Pass Editor, on a new pass after the node picked.")));
        if (add_task_action)
            add_task_action->setToolTip(tip(QStringLiteral("render.compute"),
                                            QObject::tr("The Task Editor, on a new task.")));
        // Decision 3: Edit pass / Edit task go where the pass is edited.
        if (edit_pass_button)
            edit_pass_button->setToolTip(
                graph_edits_pass
                    ? QObject::tr("Opens in the Graph: a pass the Graph edits (a graph node's, "
                                  "or the rendering section's) is picked there, its detail "
                                  "column open. A preset's or a feature's pass opens in the "
                                  "Pass Editor.")
                    : QObject::tr("Opens the picked pass in the Pass Editor."));
        if (edit_task_button)
            edit_task_button->setToolTip(
                graph_edits_pass
                    ? QObject::tr("Opens in the Graph: a task a render.compute node made is "
                                  "picked there. Any other task opens in the Task Editor.")
                    : QObject::tr("Opens the picked task in the Task Editor."));
    }

    void openInGraph(const std::optional<FramePlanNodeKey> &node) {
        // Decision 10: an expanded copy opens the body node it copies, in the
        // body its element's branch runs.
        if (const auto *origin = node ? loopOriginOf(*node) : nullptr) {
            if (!graph_loop_node_open_action) {
                showStatus(owner.tr("Opening the Graph tab is unavailable."));
                return;
            }
            if (!graph_loop_node_open_action(text(origin->loop), origin->element,
                                             text(origin->node)))
                showStatus(owner.tr("The Graph tab has no loop %1 with a body node %2 yet: "
                                    "refresh it.")
                               .arg(text(origin->loop), text(origin->node)));
            return;
        }
        const auto *source = node ? graphNodeOf(*node) : nullptr;
        if (source == nullptr) {
            // GL-1, decision 6: any other pass, on the Graph canvas.
            if (const auto *pass = node ? planPassOf(*node) : nullptr) {
                if (!graph_pass_open_action) {
                    showStatus(owner.tr("Opening the Graph tab is unavailable."));
                    return;
                }
                if (!graph_pass_open_action(text(pass->name)))
                    showStatus(owner.tr("The Graph tab has not read pass %1 yet: refresh it.")
                                   .arg(text(pass->name)));
                return;
            }
            showStatus(owner.tr("Select a pass first."));
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

    [[nodiscard]] bool planReadable() const {
        return plan_request ? true : viewport.rpcReady();
    }

    // Q-18. Read the plan because a commit may have changed it: now when in
    // view and nothing is being read, after the read in flight, or when the
    // panel is next shown. Without an engine there is nothing to read; the
    // engine coming back reads the plan anyway.
    void planChangedByCommit() {
        if (!planReadable()) return;
        if (!owner.isVisible()) {
            plan_stale = true;
            return;
        }
        readPlanAgain();
    }

    void readPlanAgain() {
        if (pending_request != 0) {
            plan_reread = true;
            return;
        }
        requestRefresh();
    }

    void requestRefresh() {
        if (pending_request != 0) {
            return;
        }
        if (!planReadable()) {
            showUnavailable(owner.tr("the engine RPC connection is not ready."));
            return;
        }

        QString error;
        plan_stale = false;
        pending_request = plan_request
                              ? plan_request(&error)
                              : viewport.requestRpc(QStringLiteral("get_frame_plan"),
                                                    QJsonObject{}, &error);
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
        if (request_id != 0 && request_id == pending_loop_page_request) {
            pending_loop_page_request = 0;
            receiveLoopMaterials(result_json);
            return;
        }
        if (request_id != pending_request) {
            return;
        }
        pending_request = 0;
        unpublished_plan_retries = 0;
        owner.receiveResult(result_json);
        // Q-18: a commit landed while this read was out; it may predate it.
        if (std::exchange(plan_reread, false)) requestRefresh();
        // GL-5d: the Graph's action that waited for this read.
        runWhenRead();
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
            // BS-1s, decision 2: the plan of the engine's placeholder says so
            // (get_frame_plan carries render_pipeline while it lasts). There
            // is nothing of the project's to edit in it, so its documents are
            // not read for editing either.
            if (showRenderFallback(result_json)) return;
            requestAuthoringContext();
            requestTaskAuthoring();
            followShaderSources();
            if (!requested_hit_group.isEmpty()) {
                const auto group = std::exchange(requested_hit_group, QString{});
                owner.openTasksForHitGroup(group);
            }
            showRequestedLoopGroup();
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
        if (request_id != 0 && request_id == pending_loop_page_request) {
            pending_loop_page_request = 0;
            loop_page_node.clear();
            showStatus(oneLine(message), message);
            return;
        }
        if (request_id != pending_request) {
            return;
        }
        pending_request = 0;
        // Q-18: the read a commit was waiting behind is gone; the one after
        // it is what the panel shows.
        if (std::exchange(plan_reread, false)) {
            requestRefresh();
            return;
        }
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
        // GL-5d: an action that waited runs on what is in hand, and says why
        // it cannot.
        runWhenRead();
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
        dropBands();
        passes->clear();
        resources->clear();
        physical->clear();
        decisions->clear();
        backends->clear();
        barriers->clear();
        materials->clear();
        raw_json->clear();
        raw_json_stale = true;
        // LP-4e: pages read for the plan before this one say nothing now.
        loop_more.clear();
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
        // UX-6b, decision 2: the new nodes' times follow the zoom too.
        logical_scene->setGpuTimingCompact(logical->transform().m11() <
                                           GpuTimingReadableScale);
        populateLogicalDetails();
        populatePasses();
        populateResources();
        populatePhysicalPlan();
        populateDecisions();
        populateBackends();
        addBackendBand();
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
        // VI-3a: what the last canvas edit moved, then a refused reload,
        // which outranks it.
        if (!commit_notice.isEmpty()) showStatus(commit_notice);
        showPickedLoopLine();
        updateReloadRefusal();
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
            setRowNodes(item, {node.name});
            setPlanningSubject(item, "node", node.name, node.loop_origin.has_value());
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
            // UX-15b, decision 5: the rewrite that put the pass here.
            if (model)
                if (const auto lines = framePlanReplacedByLines(*model, node.name);
                    !lines.empty()) {
                    QStringList said;
                    for (const auto &line : lines) said.push_back(text(line));
                    addFact(facts, owner.tr("Replaced by"), said.join(QStringLiteral("; ")));
                    item->setToolTip(1, owner.tr("Replaced by %1").arg(said.join(QStringLiteral("; "))));
                }
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
            // LP-4f3 (VF-2): the size the family's passes draw at.
            if (const auto size = framePlanFamilyExtentLine(node); !size.empty())
                addFact(facts, owner.tr("Family size"),
                        text(size.substr(std::string_view{"Family size: "}.size())));
            // LP-4c: the loop that expanded it, and what it copies.
            if (node.loop_origin)
                addFact(facts, owner.tr("Loop origin"),
                        text(framePlanLoopOriginLine(*node.loop_origin)));
            // LP-4e, decision 5: the draw_loop that draws inside this pass,
            // its materials by the branch each took.
            if (model)
                if (const auto *loop = framePlanDrawLoopOf(*model, node.name))
                    addDrawLoopMaterials(tree, item, *loop);
            // LP-4a: the views of the family the node runs for.
            if (!node.view_select.empty())
                addFact(facts, owner.tr("View select"),
                        text(framePlanViewSelectLine(node.view_select,
                                                     node.view_axes)
                                 .substr(6)));
            if (const auto idle = framePlanNoSelectedViewNotice(node);
                !idle.empty())
                addFact(facts, owner.tr("This frame"), text(idle));
            addFact(facts, owner.tr("Material variant"),
                    text(node.material_variant));
            // UX-12c, decision 2 (engine MP-1): what a material pass drew in
            // the last frame -- how many materials, their draws, each one.
            if (node.drawn_materials)
                addDrawnMaterials(item, *node.drawn_materials);
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

    // LP-4e, decision 5. "Draw loop <id>": one group per branch with the
    // materials that took it, and "Read more" while the plan holds fewer
    // materials than it says there are.
    void addDrawLoopMaterials(QTreeWidget *tree, QTreeWidgetItem *item,
                              const FramePlanLoop &loop) {
        std::vector<FramePlanLoopElement> elements{loop.elements.begin(),
                                                   loop.elements.end()};
        if (const auto more = loop_more.find(loop.node); more != loop_more.end())
            elements.insert(elements.end(), more->second.begin(), more->second.end());
        const auto total = std::max(loop.elementCount(), elements.size());
        auto *group = groupItem(item, owner.tr("Draw loop %1").arg(text(loop.node)), total);
        std::map<std::size_t, std::vector<std::string>> by_branch;
        for (const auto &element : elements)
            by_branch[element.branch].push_back(
                element.key.empty() ? std::to_string(element.element) : element.key);
        for (const auto branch : loop.unmatched_branches) by_branch.try_emplace(branch);
        for (const auto &[branch, keys] : by_branch) {
            auto *row = groupItem(group, owner.tr("Branch %1").arg(branch), keys.size());
            if (keys.empty()) {
                addFact(row, owner.tr("Materials"),
                        owner.tr("none: no material takes this branch"));
                continue;
            }
            for (const auto &key : keys) addFact(row, owner.tr("Material"), text(key));
        }
        if (elements.size() < total) {
            auto *more = new QTreeWidgetItem(group);
            more->setText(1, owner.tr("%1 of %2 shown")
                                 .arg(static_cast<qulonglong>(elements.size()))
                                 .arg(static_cast<qulonglong>(total)));
            auto *read_more = new QPushButton(owner.tr("Read more"), tree);
            read_more->setObjectName(QStringLiteral("pelican.framePlan.loopMaterialsMore"));
            read_more->setEnabled(pending_loop_page_request == 0);
            QObject::connect(read_more, &QPushButton::clicked, &owner,
                             [this, node = loop.node] { requestLoopMaterials(node); });
            tree->setItemWidget(more, 2, read_more);
        }
    }

    void populatePasses() {
        populatePasses(passes, model->nodes);
        passes->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    }

    void populateDecisions() {
        for (const auto &group : model->decision_groups) {
            auto *subject = new QTreeWidgetItem(decisions);
            std::vector<std::string> subject_nodes;
            for (const auto &decision : group.decisions)
                subject_nodes.insert(subject_nodes.end(), decision.from_nodes.begin(),
                                     decision.from_nodes.end());
            setRowNodes(subject, subject_nodes);
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
                setRowNodes(item, decision.from_nodes);
                item->setText(1, text(decision.id));
                item->setText(2, text(decision.selected));
                item->setText(3, text(decision.detail));
                if (!decision.from_fold.empty())
                    item->setText(4, owner.tr("fold %1").arg(text(decision.from_fold)));
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
            setRowNodes(candidate_item, candidate.from_nodes);
            // UX-15b, decision 1: the row Pin this candidate acts on.
            candidate_item->setData(0, BackendCandidateRole, text(candidate.candidate));
            if (candidate.selected || candidate.feasible)
                candidate_item->setToolTip(
                    0, owner.tr("Right-click: Pin this candidate, so the planner keeps it."));
            candidate_item->setText(0, text(candidate.candidate));
            candidate_item->setText(
                1, candidate.selected
                       ? owner.tr("Selected")
                       : (candidate.feasible ? owner.tr("Feasible")
                                             : owner.tr("Rejected")));
            if (candidate.candidate == model->physical_plan.pinned_backend_candidate)
                candidate_item->setText(1, owner.tr("%1, pinned").arg(candidate_item->text(1)));
            candidate_item->setText(2, text(candidate.endpoint));
            QFont font = candidate_item->font(0);
            font.setBold(true);
            candidate_item->setFont(0, font);

            for (const auto &failure : candidate.failures) {
                auto *item = new QTreeWidgetItem(candidate_item);
                setRowNodes(item, {failure.subject});
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

    // UX-15b, decision 1: the pin the document applies, first, with the way
    // off it; without one, where the way on is.
    void addBackendBand() {
        if (!model->physical_plan.available()) return;
        const auto &pinned = model->physical_plan.pinned_backend_candidate;
        if (!model->physical_plan.applied_pin_package.empty())
            addBand(backends,
                    owner.tr("Pinned: %1").arg(pinned.empty() ? owner.tr("(no candidate named)")
                                                              : text(pinned)),
                    "pelican.framePlan.backend.state",
                    {{backend_unpin, "pelican.framePlan.backend.unpinButton"}});
        else
            addBand(backends,
                    owner.tr("Not pinned: the planner picks the candidate. Right-click a "
                             "Selected or Feasible row to pin it."),
                    "pelican.framePlan.backend.state", {});
    }

    void populateResources() {
        for (const FramePlanResource &resource : model->resources) {
            auto *item = new QTreeWidgetItem(resources);
            // GL-2a's from_nodes; a plan without them, the writers and readers.
            if (!resource.from_nodes.empty()) {
                setRowNodes(item, resource.from_nodes);
            } else {
                auto names = resource.writers;
                names.insert(names.end(), resource.readers.begin(), resource.readers.end());
                setRowNodes(item, names);
            }
            setPlanningSubject(item, "resource", resource.name,
                               resource.loop_origin.has_value());
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
            // IA-1s, decision 3: the asset it starts from, and its manifest
            // sha256 (the start of it), in the row's facts and its writers'
            // column, where nothing else would be.
            if (resource.initial_asset) {
                const auto line = text(framePlanInitialAssetLine(*resource.initial_asset));
                addFact(facts, owner.tr("Initial contents"), line);
                if (resource.writers.empty()) item->setText(2, line);
            }
            if (resource.loop_origin)
                addFact(facts, owner.tr("Loop origin"),
                        text(framePlanLoopOriginLine(*resource.loop_origin)));
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

            // UX-15b, decision 5: what rewrote the graph before it was
            // planned, and by which provider.
            if (!model->physical_plan.rewrites.empty()) {
                auto *rewrites = groupItem(state, owner.tr("Compile rewrites"),
                                           model->physical_plan.rewrites.size());
                for (const auto &rewrite : model->physical_plan.rewrites) {
                    auto *item = new QTreeWidgetItem(rewrites);
                    setRowNodes(item, rewrite.replacement_nodes);
                    item->setText(0, text(rewrite.name));
                    item->setText(1, rewrite.kind == "subgraph_replacement"
                                         ? owner.tr("Subgraph replacement")
                                     : rewrite.kind == "graph_transform"
                                         ? owner.tr("Graph transform")
                                         : owner.tr("Render strategy"));
                    item->setText(2, text(rewrite.provider));
                    item->setText(3, rewrite.replacement_nodes.empty()
                                         ? text(framePlanRewriteLine(rewrite))
                                         : owner.tr("%1 -> %2")
                                               .arg(joined(rewrite.source_nodes),
                                                    joined(rewrite.replacement_nodes)));
                    item->setToolTip(3, text(framePlanRewriteLine(rewrite)));
                }
            }

            auto *aliases = groupItem(
                state,
                owner.tr("Alias groups (%1)")
                    .arg(static_cast<qulonglong>(
                        model->physical_plan.alias_groups.size())));
            for (const auto &group : model->physical_plan.alias_groups) {
                auto *item = new QTreeWidgetItem(aliases);
                setRowNodes(item, group.from_nodes);
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
                        // A pair of passes (fusion, parallel) or of resources
                        // (alias): the passes, or the resources' passes.
                        std::vector<std::string> pair_nodes{candidate.first, candidate.second};
                        for (const auto &resource : model->resources)
                            if (resource.name == candidate.first || resource.name == candidate.second)
                                pair_nodes.insert(pair_nodes.end(), resource.from_nodes.begin(),
                                                  resource.from_nodes.end());
                        setRowNodes(item, pair_nodes);
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
                setRowNodes(item, scope.nodes);
                item->setText(0, text(scope.id));
                item->setText(1, text(scope.kind));
                item->setText(2, joined(scope.nodes));
                QString scope_facts =
                    owner.tr("%1; %2 execution(s); %3 local reads")
                        .arg(text(scope.view_execution))
                        .arg(static_cast<qulonglong>(scope.execution_count))
                        .arg(static_cast<qulonglong>(
                            scope.local_reads.size()));
                // LP-4a: a scope whose passes run only for some views.
                if (!scope.view_select.empty())
                    scope_facts += QStringLiteral("; %1").arg(
                        text(framePlanViewSelectLine(scope.view_select)));
                item->setText(3, scope_facts);
            }

            auto *lowering_group = groupItem(
                state, owner.tr("Lowering nodes"),
                model->physical_plan.lowering_nodes.size());
            for (const auto &node : model->physical_plan.lowering_nodes) {
                auto *item = new QTreeWidgetItem(lowering_group);
                setRowNodes(item, node.sources);
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
                setRowNodes(item, {dependency.from, dependency.to});
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
        populateTransferSourceRows();
        populateRayQuery();
        addPhysicalBand();
    }

    // UX-15b, decision 2: whether the document freezes the physical plan,
    // first, and the one button that changes it.
    void addPhysicalBand() {
        if (!model->physical_plan.available()) return;
        if (!model->physical_plan.applied_physical_fragment.empty())
            addBand(physical,
                    owner.tr("Frozen: every compile keeps the physical plan the document "
                             "holds."),
                    "pelican.framePlan.physical.state",
                    {{physical_remove, "pelican.framePlan.physical.removeButton"}});
        else
            addBand(physical,
                    owner.tr("Not frozen: the planner decides the physical plan each compile."),
                    "pelican.framePlan.physical.state",
                    {{physical_freeze, "pelican.framePlan.physical.freezeButton"}});
    }

    // UX-15b, decision 4: the ray-query scene's resident BLAS. A project
    // without ray tracing asks for none, and the section is not shown.
    void populateRayQuery() {
        if (!model->ray_query || !model->ray_query->shown()) return;
        const auto &scene = *model->ray_query;
        auto *group = new QTreeWidgetItem(physical);
        group->setText(0, owner.tr("Ray query acceleration structures"));
        QFont font = group->font(0);
        font.setBold(true);
        group->setFont(0, font);
        group->setText(1, scene.requested ? owner.tr("Requested") : owner.tr("Not requested"));
        group->setText(2, owner.tr("%1 resident BLAS, %2 TLAS instances")
                              .arg(static_cast<qulonglong>(scene.resident_blas_keys.size()))
                              .arg(static_cast<qulonglong>(scene.tlas_instance_count)));
        addFact(group, owner.tr("Active BLAS"),
                QString::number(static_cast<qulonglong>(scene.active_blas_count)));
        addFact(group, owner.tr("BLAS variants"),
                QString::number(static_cast<qulonglong>(scene.blas_variant_count)));
        if (!scene.named_tlas_instance_counts.empty()) {
            auto *named = groupItem(group, owner.tr("Named TLAS"),
                                    scene.named_tlas_instance_counts.size());
            for (const auto &[name, count] : scene.named_tlas_instance_counts)
                addFact(named, text(name),
                        owner.tr("%1 instances").arg(static_cast<qulonglong>(count)));
        }
        auto *keys = groupItem(group, owner.tr("Resident BLAS"), scene.resident_blas_keys.size());
        keys->setToolTip(1, owner.tr("One BLAS per geometry the scene keeps built; every TLAS "
                                     "shares them."));
        for (std::size_t index = 0; index < scene.resident_blas_keys.size(); ++index) {
            const auto &key = scene.resident_blas_keys[index];
            auto *item = new QTreeWidgetItem(keys);
            item->setText(0, QStringLiteral("#%1").arg(static_cast<qulonglong>(index)));
            item->setText(1, text(key.geometry_kind));
            item->setText(2, owner.tr("allocation %1")
                                 .arg(static_cast<qulonglong>(key.geometry_allocation_id)));
            item->setText(3, owner.tr("%1 geometries, flags 0x%2")
                                 .arg(static_cast<qulonglong>(key.geometry_count))
                                 .arg(static_cast<qulonglong>(key.geometry_flags), 0, 16));
        }
        group->setExpanded(true);
    }

    // UX-6a, decision 4: an image the planner keeps out of aliasing that has
    // TRANSFER_SRC, with a button that takes the flag off. The engine reads
    // the flag as "read from outside the frame plan". PO-1's reason says why
    // after ": " (causes joined by "; "), e.g. "...: usage TRANSFER_SRC is
    // outside the alias-safe set {...}": that sentence decides. A reason
    // with no causes (an engine before PO-1) leaves it to the usage.
    static bool keptByTransferSource(const FramePlanResource &resource) {
        if (resource.kind == "buffer" || resource.representation.empty() || resource.aliasable)
            return false;
        if (resource.reason.find("usage TRANSFER_SRC is outside") != std::string::npos)
            return true;
        if (resource.reason.find(": ") != std::string::npos) return false;
        return resource.usage &&
               std::ranges::find(*resource.usage, "TRANSFER_SRC") != resource.usage->end();
    }

    void populateTransferSourceRows() {
        std::vector<const FramePlanResource *> kept;
        for (const auto &resource : model->resources)
            if (keptByTransferSource(resource)) kept.push_back(&resource);
        if (kept.empty()) return;
        auto *group = new QTreeWidgetItem(physical);
        group->setText(0, owner.tr("Not aliased for TRANSFER_SRC (%1)")
                              .arg(static_cast<qulonglong>(kept.size())));
        group->setToolTip(0, owner.tr("TRANSFER_SRC lets an image be read from outside the "
                                      "frame plan, so the planner never aliases it. Nothing in "
                                      "Studio reads an image's contents."));
        QFont font = group->font(0);
        font.setBold(true);
        group->setFont(0, font);
        for (const auto *resource : kept) {
            auto *item = new QTreeWidgetItem(group);
            setRowNodes(item, resource->from_nodes);
            item->setText(0, text(resource->name));
            item->setText(1, owner.tr("Not aliasable"));
            item->setText(2, text(resource->reason));
            auto *drop = new QPushButton(owner.tr("Remove TRANSFER_SRC"));
            drop->setObjectName(QStringLiteral("pelican.framePlan.dropTransferSrc.") +
                                text(resource->name));
            drop->setToolTip(owner.tr("Take TRANSFER_SRC off %1's usage, so the planner may "
                                      "alias it.")
                                 .arg(text(resource->name)));
            drop->setEnabled(static_cast<bool>(drop_transfer_source_action));
            const auto name = text(resource->name);
            const auto origin = resource->loop_origin;
            QObject::connect(drop, &QPushButton::clicked, &owner, [this, name, origin] {
                if (!drop_transfer_source_action || !drop_transfer_source_action(name, origin))
                    showStatus(owner.tr("Could not take TRANSFER_SRC off %1: it is not an "
                                        "image the Graph tab can edit now.")
                                   .arg(name));
            });
            physical->setItemWidget(item, 3, drop);
        }
        group->setExpanded(true);
    }

    void populateBarriers() {
        for (std::size_t index = 0; index < model->barriers.size(); ++index) {
            const auto &barrier = model->barriers[index];
            auto *item = new QTreeWidgetItem(barriers);
            setRowNodes(item, {barrier.from, barrier.to});
            item->setText(0, QString::number(static_cast<qulonglong>(index)));
            item->setText(1, text(barrier.from));
            item->setText(2, text(barrier.to));
            item->setText(3, text(barrier.resource));
            item->setText(4, text(barrier.kind));
        }
    }

    // UX-12c, decision 2: "3 materials, 12 draws (frame 340)".
    QString drawnSummary(const FramePlanDrawnMaterials &drawn) const {
        return owner.tr("%1, %2 (frame %3)%4")
            .arg(owner.tr("%n material(s)", nullptr, static_cast<int>(drawn.materials.size())),
                 owner.tr("%n draw(s)", nullptr, static_cast<int>(drawn.draws())))
            .arg(static_cast<qulonglong>(drawn.frame))
            .arg(drawn.truncated ? owner.tr(", the first %1 listed").arg(drawn.materials.size())
                                 : QString{});
    }

    void addDrawnMaterials(QTreeWidgetItem *item, const FramePlanDrawnMaterials &drawn) {
        auto *group = groupItem(item, owner.tr("Materials drawn"));
        group->setText(2, drawnSummary(drawn));
        for (std::size_t index = 0; index < drawn.materials.size(); ++index)
            addFact(group, text(framePlanDrawnMaterialLabel(drawn, index)),
                    owner.tr("%n draw(s)", nullptr,
                             static_cast<int>(drawn.materials[index].draws)));
    }

    void populateMaterials() {
        if (model->material_routes.empty()) {
            auto *empty = new QTreeWidgetItem(materials);
            empty->setText(0, owner.tr("No material routes were published."));
            empty->setDisabled(true);
        }
        const auto drawnOf = [&](const std::string &pass) -> const FramePlanDrawnMaterials * {
            for (const auto &node : model->nodes)
                if (node.name == pass && node.drawn_materials) return &*node.drawn_materials;
            return nullptr;
        };
        for (const auto &route : model->material_routes) {
            auto *item = new QTreeWidgetItem(materials);
            item->setText(0, text(route.route));
            item->setText(1, text(route.pass));
            item->setText(2, text(route.phase));
            item->setText(3, text(route.contract));
            item->setText(4, text(route.shader_contract));
            // UX-12c, decision 2: what the route's pass drew.
            if (const auto *drawn = drawnOf(route.pass))
                item->setText(5, drawnSummary(*drawn));
        }
        // UX-12c, decision 2: each material a pass drew in the last frame, a
        // row of its own, named by the pass that drew it (the Graph's
        // Materials drawn picks these with the routes).
        for (const auto &node : model->nodes) {
            if (!node.drawn_materials) continue;
            const auto &drawn = *node.drawn_materials;
            for (std::size_t index = 0; index < drawn.materials.size(); ++index) {
                const auto &material = drawn.materials[index];
                auto *item = new QTreeWidgetItem(materials);
                item->setText(0, text(framePlanDrawnMaterialLabel(drawn, index)));
                item->setToolTip(0, text(material.id));
                item->setText(1, text(node.name));
                item->setText(5, owner.tr("drawn by %1, %n draw(s)", nullptr,
                                          static_cast<int>(material.draws))
                                     .arg(text(node.name)));
            }
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
    // Q-18: a commit elsewhere while out of view left the plan behind.
    if (impl_->plan_stale && impl_->planReadable()) impl_->readPlanAgain();
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
    impl_->project_root = project_root;
    impl_->followShaderSources();
}

void FramePlanWidget::refresh() { impl_->requestRefresh(); }

void FramePlanWidget::planChangedByCommit() { impl_->planChangedByCommit(); }

void FramePlanWidget::setPlanRequest(std::function<qint64(QString *error)> request) {
    impl_->plan_request = std::move(request);
}

void FramePlanWidget::receiveShaderReloadStatus(const QByteArray &status_json) {
    impl_->reload_follower->consume(status_json);
}

void FramePlanWidget::setExtraShaderSources(NodeShaderSources sources) {
    if (impl_->extra_shader_sources == sources) return;
    impl_->extra_shader_sources = std::move(sources);
    impl_->followShaderSources();
}

void FramePlanWidget::setShadersReloadedAction(std::function<void()> reloaded) {
    impl_->shaders_reloaded_action = std::move(reloaded);
}

void FramePlanWidget::setPerfLines(const QString &summary, const QString &delta) {
    impl_->perf_summary->setText(summary);
    impl_->perf_delta->setText(delta);
    if (!delta.isEmpty()) impl_->perf_delta->setToolTip(delta);
}

void FramePlanWidget::setDropTransferSourceAction(DropTransferSourceAction action) {
    impl_->drop_transfer_source_action = std::move(action);
}

void FramePlanWidget::receiveShaderReloadOutcome(
    const ShaderReloadOutcome &outcome) {
    impl_->applyShaderReloadOutcome(outcome);
}

void FramePlanWidget::setWatchedRenderDocuments(
    std::vector<std::filesystem::path> files,
    std::function<void(const QString &path)> changed) {
    impl_->reload_follower->setDocumentFiles(std::move(files), std::move(changed));
}

void FramePlanWidget::observeEngineStatus(const nlohmann::json &status) {
    impl_->reload_follower->observeStatus(readShaderReloadStatus(status));
}

void FramePlanWidget::fixSockets(const QString &graph, const QString &pass) {
    impl_->fixSockets(FramePlanNodeKey{graph.toStdString(), pass.toStdString()});
}

void FramePlanWidget::fixSockets(const QString &graph, const QString &pass,
                                 std::function<bool(const QString &reference)> graph_fragment) {
    impl_->fixSockets(FramePlanNodeKey{graph.toStdString(), pass.toStdString()}, graph_fragment);
}

void FramePlanWidget::makeSocketsVariadic(const QString &graph, const QString &pass) {
    impl_->makeSocketsVariadic(FramePlanNodeKey{graph.toStdString(), pass.toStdString()});
}

void FramePlanWidget::orderNodes() { impl_->orderNodes(); }

void FramePlanWidget::whenRead(std::function<void()> action,
                               std::function<void(const QString &line)> said, bool tasks) {
    impl_->whenRead(std::move(action), std::move(said), tasks);
}

// --- GL-5e: this panel's own functions, run for the Graph tab.

void FramePlanWidget::removePass(const QString &graph, const QString &pass) {
    impl_->removeNode(FramePlanNodeKey{graph.toStdString(), pass.toStdString()});
}

void FramePlanWidget::runShaderContract(const QString &graph, const QString &pass,
                                        const QString &fragment, const QStringList &inputs,
                                        bool unsupplied, std::optional<std::size_t> binding,
                                        bool use_passthrough) {
    Impl::ShaderContractFix fix;
    fix.graph = graph.toStdString();
    fix.pass = pass.toStdString();
    fix.fragment = fragment.toStdString();
    for (const auto &input : inputs) fix.inputs.push_back(input.toStdString());
    fix.unsupplied = unsupplied;
    fix.binding = binding;
    impl_->contract_fix = std::move(fix);
    if (use_passthrough) impl_->useEnginePassthrough();
    else impl_->openContractShader();
}

void FramePlanWidget::runReloadRefusal(const QString &graph, const QString &pass,
                                       bool disconnect) {
    const auto name = pass.toStdString();
    std::optional<Impl::ReloadRefusalFix> fix;
    if (impl_->model && impl_->model->graph == graph.toStdString())
        if (const auto node = std::ranges::find(impl_->model->nodes, name, &FramePlanNode::name);
            node != impl_->model->nodes.end())
            fix = impl_->reloadRefusalFor(*node);
    if (!fix) {
        impl_->showStatus(tr("%1's shader reloaded since: nothing to disconnect.").arg(pass));
        return;
    }
    impl_->reload_refusal_fix = std::move(fix);
    if (disconnect) impl_->disconnectRefusedInput();
    else impl_->openRefusedShader();
}

void FramePlanWidget::pinBackendCandidate(const QString &candidate) {
    impl_->pinCandidate(candidate.toStdString());
}

void FramePlanWidget::unpinBackend() { impl_->unpinBackend(); }

void FramePlanWidget::freezePhysicalPlan() { impl_->freezePhysicalPlan(); }

void FramePlanWidget::removeFrozenPhysicalPlan() { impl_->removeFrozenPlan(); }

std::vector<FramePlanViewAxis> FramePlanWidget::taskViewAxes(
    const QString &task) const {
    if (!impl_->model || task.isEmpty()) return {};
    const auto name = task.toStdString();
    for (const auto &node : impl_->model->nodes)
        if (node.kind == "compute" && node.authored_name == name)
            return node.view_axes;
    return {};
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

void FramePlanWidget::showLoopGroup(const QString &loop) {
    if (loop.isEmpty()) return;
    impl_->requested_loop_group = loop;
    impl_->requestRefresh();
}

void FramePlanWidget::showPlanNode(const QString &node) {
    impl_->showPlanNode(node.toStdString());
}

void FramePlanWidget::showMaterialRoutes(const QString &pass) {
    impl_->showMaterialRoutes(pass);
}

void FramePlanWidget::setGraphPlaceAction(GraphPlaceAction action) {
    impl_->graph_place_action = std::move(action);
    if (impl_->authoring.received) impl_->syncDelegationTips();
}

void FramePlanWidget::setGraphEditsPassQuery(std::function<bool(const QString &pass)> query) {
    impl_->graph_edits_pass = std::move(query);
    impl_->syncDelegationTips();
}

void FramePlanWidget::setGraphPassOpenAction(GraphPassOpenAction action) {
    impl_->graph_pass_open_action = std::move(action);
}

void FramePlanWidget::setGraphLoopNodeOpenAction(GraphLoopNodeOpenAction action) {
    impl_->graph_loop_node_open_action = std::move(action);
}

void FramePlanWidget::decomposeMaterialPass(const QString &graph, const QString &pass) {
    impl_->decompose(graph.toStdString(), pass.toStdString());
}

void FramePlanWidget::setLoopDecomposedAction(std::function<void(const QString &loop)> action) {
    impl_->loop_decomposed_action = std::move(action);
}

void FramePlanWidget::setLoopVersionActions(
    std::function<std::optional<QString>(const QString &feature)> offered,
    std::function<void(const QString &feature)> replace) {
    impl_->loop_version_offered = std::move(offered);
    impl_->loop_version_replace = std::move(replace);
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

void FramePlanWidget::setRenderHistoryActions(QAction *undo, QAction *redo) {
    // The keys: the actions' own, taken while this panel holds the focus.
    addAction(undo);
    addAction(redo);
    // The buttons: after Refresh, each firing its action and following its
    // enabled state and its tooltip (the item the next press is about).
    int index = impl_->toolbar_row->indexOf(impl_->refresh) + 1;
    for (const auto &entry :
         {std::tuple{undo, tr("Undo"), QStringLiteral("pelican.framePlan.undoButton")},
          std::tuple{redo, tr("Redo"), QStringLiteral("pelican.framePlan.redoButton")}}) {
        QAction *action = std::get<0>(entry);
        auto *button = new QPushButton(std::get<1>(entry), this);
        button->setObjectName(std::get<2>(entry));
        impl_->toolbar_row->insertWidget(index++, button);
        const auto follow = [button, action] {
            button->setEnabled(action->isEnabled());
            button->setToolTip(action->toolTip());
        };
        follow();
        connect(action, &QAction::changed, button, follow);
        connect(button, &QPushButton::clicked, action, &QAction::trigger);
    }
}

void FramePlanWidget::showRenderHistoryLine(const QString &line) {
    if (line.isEmpty()) return;
    impl_->status->setStyleSheet({});
    impl_->showStatus(line);
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
