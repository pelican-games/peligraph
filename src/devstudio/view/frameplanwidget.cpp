#include "frameplanwidget.hpp"

#include "frameplangraphics.hpp"
#include "../model/frameplanmodel.hpp"
#include "../viewport/embeddedviewport.hpp"

#include <QAbstractItemView>
#include <QByteArray>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QGraphicsView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace PelicanStudio {
namespace {

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

class FramePlanGraphicsView final : public QGraphicsView {
  public:
    using ZoomObserver = std::function<void(qreal, const QString &)>;

    FramePlanGraphicsView(QGraphicsScene *scene, QWidget *parent)
        : QGraphicsView{scene, parent} {
        setProperty("pelicanZoomScale", 1.0);
        setProperty("pelicanZoomMinimum", MinimumZoom);
        setProperty("pelicanZoomMaximum", MaximumZoom);
        setProperty("pelicanZoomBoundary", QString{});
    }

    void setZoomObserver(ZoomObserver observer) {
        observer_ = std::move(observer);
    }

  protected:
    void wheelEvent(QWheelEvent *event) override {
        int delta = event->angleDelta().y();
        if (delta == 0) {
            delta = event->pixelDelta().y();
        }
        if (delta == 0) {
            QGraphicsView::wheelEvent(event);
            return;
        }

        const qreal current = transform().m11();
        const qreal requested =
            current * std::pow(1.0015, static_cast<qreal>(delta));
        const qreal bounded = std::clamp(requested, MinimumZoom, MaximumZoom);
        QString boundary;
        if (requested < MinimumZoom) {
            boundary = QStringLiteral("minimum");
        } else if (requested > MaximumZoom) {
            boundary = QStringLiteral("maximum");
        }
        if (std::abs(bounded - current) > 0.000001) {
            scale(bounded / current, bounded / current);
        }
        setProperty("pelicanZoomScale", bounded);
        setProperty("pelicanZoomBoundary", boundary);
        setAccessibleDescription(
            boundary.isEmpty()
                ? QStringLiteral("Frame-plan zoom %1 percent")
                      .arg(qRound(bounded * 100.0))
                : QStringLiteral("Frame-plan zoom %1 limit reached at %2 percent")
                      .arg(boundary)
                      .arg(qRound(bounded * 100.0)));
        if (observer_) {
            observer_(bounded, boundary);
        }
        event->accept();
    }

  private:
    static constexpr qreal MinimumZoom = 0.25;
    static constexpr qreal MaximumZoom = 4.0;
    ZoomObserver observer_;
};

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
    QTabWidget *tabs = nullptr;
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
    std::optional<FramePlanModel> model;
    qint64 pending_request = 0;

    Impl(FramePlanWidget &widget, EmbeddedViewport &embedded_viewport)
        : owner{widget}, viewport{embedded_viewport} {
        auto *layout = new QVBoxLayout(&owner);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        auto *toolbar = new QHBoxLayout;
        refresh = new QPushButton(owner.tr("Refresh"), &owner);
        refresh->setObjectName(QStringLiteral("pelican.framePlanRefresh"));
        refresh->setToolTip(owner.tr(
            "Fetch get_frame_plan once. The multi-thousand-line response is "
            "not polled every frame because transfer, parsing, and rebuilding "
            "the trees would stall the editor."));
        filter = new QLineEdit(&owner);
        filter->setObjectName(QStringLiteral("pelican.framePlanFilter"));
        filter->setPlaceholderText(owner.tr(
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
            "Choose one logical render target. The graph shows only passes "
            "within the selected resource depth."));
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
        toolbar->addWidget(zoom_status);
        layout->addLayout(toolbar);

        status = new QLabel(&owner);
        status->setObjectName(QStringLiteral("pelican.framePlanStatus"));
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        status->setWordWrap(true);
        layout->addWidget(status);

        tabs = new QTabWidget(&owner);
        tabs->setObjectName(QStringLiteral("pelican.framePlanTabs"));
        auto *logical_tab = new QSplitter(Qt::Horizontal, tabs);
        logical_scene = new FramePlanGraphicsScene(&owner);
        logical = new FramePlanGraphicsView(logical_scene, logical_tab);
        logical->setObjectName(QStringLiteral("pelican.framePlanLogicalView"));
        logical->setRenderHint(QPainter::Antialiasing, true);
        logical->setDragMode(QGraphicsView::ScrollHandDrag);
        logical->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        logical->setResizeAnchor(QGraphicsView::AnchorViewCenter);
        logical->setBackgroundBrush(QColor{QStringLiteral("#20262d")});
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
            logical_tab);
        logical_details->setObjectName(
            QStringLiteral("pelican.framePlanLogicalDetails"));
        logical_tab->setStretchFactor(0, 3);
        logical_tab->setStretchFactor(1, 2);
        logical_tab->setSizes({600, 360});
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
        tabs->addTab(logical_tab, owner.tr("Logical graph"));
        tabs->addTab(passes, owner.tr("Passes"));
        tabs->addTab(resources, owner.tr("Resources"));
        tabs->addTab(physical, owner.tr("Physical plan"));
        tabs->addTab(decisions, owner.tr("Decisions"));
        tabs->addTab(backends, owner.tr("Backends"));
        tabs->addTab(barriers, owner.tr("Barriers"));
        tabs->addTab(materials, owner.tr("Materials"));
        tabs->addTab(raw_json, owner.tr("Raw JSON"));
        layout->addWidget(tabs, 1);

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
                             model.reset();
                             clearTrees();
                             requestRefresh();
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
        QObject::connect(
            &viewport, &EmbeddedViewport::inspectorRpcFailed, &owner,
            [this](qint64 request_id, const QString &message) {
                receiveFailure(request_id, message);
            });

        populateLogicalDetails();

        if (viewport.rpcReady()) {
            requestRefresh();
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
        tabs->setEnabled(false);
    }

    void showUnavailable(const QString &reason) {
        pending_request = 0;
        model.reset();
        clearTrees();
        refresh->setEnabled(false);
        status->setStyleSheet(QStringLiteral("color: #b36b00;"));
        status->setText(owner.tr("Frame plan unavailable: %1 Start or restart "
                                 "pelican_player, then press Refresh.")
                            .arg(reason));
    }

    void showFailure(const QString &message) {
        refresh->setEnabled(viewport.rpcReady());
        tabs->setEnabled(model.has_value());
        status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        status->setText(model
                            ? owner.tr("Frame plan refresh failed: %1 The previous "
                                       "snapshot is still displayed.")
                                  .arg(message)
                            : owner.tr("Frame plan refresh failed: %1").arg(message));
    }

    void showEngineFailure(const QString &fatal_error_line) {
        pending_request = 0;
        model.reset();
        clearTrees();
        refresh->setEnabled(false);
        status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        status->setText(fatal_error_line);
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
        status->setText(owner.tr("Requesting the current frame plan..."));
    }

    void receiveRpcResult(qint64 request_id, const QByteArray &result_json) {
        if (request_id != pending_request) {
            return;
        }
        pending_request = 0;
        owner.receiveResult(result_json);
    }

    void consumeResult(const QByteArray &result_json) {
        try {
            const bool had_model = model.has_value();
            const std::string previous_graph =
                model ? model->graph : std::string{};
            const QString previous_target = target->currentText();
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
                for (const auto &name : targets) {
                    target->addItem(text(name));
                }
                int selected_index = -1;
                if (!targets.empty() && (!had_model || !same_graph)) {
                    selected_index = 0;
                } else if (same_graph && !previous_target.isEmpty()) {
                    selected_index = target->findText(previous_target,
                                                      Qt::MatchExactly);
                }
                target->setCurrentIndex(selected_index);
            }
            target->setEnabled(!targets.empty());
            populate();
        } catch (const std::exception &error) {
            showFailure(QString::fromUtf8(error.what()));
        }
    }

    void receiveFailure(qint64 request_id, const QString &message) {
        if (request_id != pending_request) {
            return;
        }
        pending_request = 0;
        showFailure(message);
    }

    void populate() {
        passes->clear();
        resources->clear();
        physical->clear();
        decisions->clear();
        backends->clear();
        barriers->clear();
        materials->clear();
        raw_json->clear();
        const std::optional<FramePlanNodeKey> selected_target =
            target->currentIndex() < 0
                ? std::nullopt
                : std::optional<FramePlanNodeKey>{FramePlanNodeKey{
                      model->graph, target->currentText().toStdString()}};
        logical_scene->populate(*model, selected_target, depth->value());
        populateLogicalDetails();
        populatePasses();
        populateResources();
        populatePhysicalPlan();
        populateDecisions();
        populateBackends();
        populateBarriers();
        populateMaterials();
        raw_json->setPlainText(text(model->raw_json));
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
                                             ? owner.tr("physical available (profile %1, endpoint %2; %3 alias groups, %4 alias candidates)")
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
            selected_target
                ? owner.tr("target %1 at depth %2")
                      .arg(text(selected_target->name))
                      .arg(depth->value())
                : owner.tr("no target selected");
        status->setText(
            owner.tr("%1 | %2 | %3%4 passes/tasks (%5 compute), %6, "
                     "%7 resources, %8 barriers | %9 | %10 response | "
                     "refreshed %11. Snapshot updates "
                     "only when the engine connects or Refresh is pressed; it "
                     "is not polled per frame.")
                .arg(text(model->graph), target_summary, generation)
                .arg(static_cast<qulonglong>(model->nodes.size()))
                .arg(static_cast<qulonglong>(compute_count))
                .arg(execution_summary)
                .arg(static_cast<qulonglong>(model->resources.size()))
                .arg(static_cast<qulonglong>(model->barriers.size()))
                .arg(physical_summary, byteCount(model->response_bytes),
                     QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));
    }

    void addResourceGroup(QTreeWidgetItem *pass_item, const QString &title,
                          const std::vector<std::string> &names) {
        auto *group = groupItem(pass_item, title, names.size());
        for (const auto &name : names) {
            auto *item = new QTreeWidgetItem(group);
            item->setText(1, resourceLabel(*model, name));
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

void FramePlanWidget::receiveResult(const QByteArray &result_json) {
    impl_->consumeResult(result_json);
}

} // namespace PelicanStudio
