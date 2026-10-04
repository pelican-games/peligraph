#include "renderfeatureswidget.hpp"
#include "featureparameterform.hpp"
#include "featurerefusalfix.hpp"
#include "fullscreenshadertemplate.hpp"
#include "frameplanedit.hpp"
#include "panelhelp.hpp"
#include "renderconfigdigest.hpp"
#include "rendereditcompletion.hpp"
#include "proceduralassetdialog.hpp"
#include "jsonrpcrequest.hpp"
#include "taskresourceconnections.hpp"
#include "taskconnectiongraphics.hpp"
#include "inspectormodel.hpp"
#include "viewselectfield.hpp"

#include "../viewport/embeddedviewport.hpp"

#include <QAbstractItemView>
#include <QDialog>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QScrollArea>
#include <QFormLayout>
#include <nlohmann/json.hpp>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <cmath>
#include <array>
#include <limits>
#include <QTableWidget>
#include <QSet>
#include <QByteArray>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <functional>
#include <utility>
#include <stdexcept>

namespace PelicanStudio {
namespace {

constexpr int ReferenceRole = Qt::UserRole;
constexpr int FeatureNameRole = Qt::UserRole + 1;
constexpr int AvailableRole = Qt::UserRole + 2;
constexpr int UnavailableReasonRole = Qt::UserRole + 3;
constexpr int InstanceRole = Qt::UserRole + 4;

QString responseError(const QJsonObject &response) {
    const auto error = response.value(QStringLiteral("error"));
    if (error.isObject()) {
        const auto object = error.toObject();
        // LP-4a: a view_select refusal reads as its sentence, the code last.
        if (const auto sentence = viewSelectRefusalDescription(nlohmann::json::parse(
                QJsonDocument{object}.toJson(QJsonDocument::Compact).toStdString(),
                nullptr, false));
            !sentence.empty())
            return QString::fromStdString(sentence);
        const auto message =
            object.value(QStringLiteral("message")).toString();
        const auto code =
            object.value(QStringLiteral("code")).toString();
        if (!message.isEmpty() && !code.isEmpty()) {
            return QStringLiteral("%1 (%2)").arg(message, code);
        }
        if (!message.isEmpty()) return message;
        if (!code.isEmpty()) return code;
    }
    return QStringLiteral("The engine rejected the render feature edit.");
}

std::optional<QJsonObject> decodeObject(
    const QByteArray &bytes, QString *error) {
    QJsonParseError parse_error;
    const auto document =
        QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("Invalid engine response: %1")
                         .arg(parse_error.errorString());
        }
        return std::nullopt;
    }
    return document.object();
}

// RSX, decision 1. What the editors here hand a commit the engine runs only
// after a restart: Render features' own restartEngine, which says so on its
// notice and asks the main window to restart (requestEngineRestart).
using EngineRestartAction = std::function<void(const QString &reason)>;

// RSX, decision 1. Every editor here reads a ticket's last answer through the
// one reading all panels use (completeRenderEdit). A commit that asks for a
// restart has handed it to `restart` before this returns; the editor then
// reads nothing more from the engine about to stop.
RenderEditCompletion completeEdit(const QJsonObject &response,
                                  const EngineRestartAction &restart) {
    return completeRenderEdit(
        nlohmann::json::parse(
            QJsonDocument{response}.toJson(QJsonDocument::Compact).toStdString(),
            nullptr, false),
        [&restart](const std::string &line) {
            if (restart) restart(QString::fromStdString(line));
        });
}

// RSX: the editors Render features opens are given its restart.
struct RestartsEngine {
    EngineRestartAction restart;
    void setEngineRestartAction(EngineRestartAction action) {
        restart = std::move(action);
    }
};

class FrameNodeOrderDialog final : public QDialog, public RestartsEngine {
  EmbeddedViewport &viewport;
  std::function<void()> committed;
  QString graph, digest, ticket;
  const quint64 expected_generation;
  QJsonObject orders;
  QListWidget *nodes;
  QLabel *notice;
  QPushButton *up, *down, *apply, *reset, *retry;
  QTimer poll;
  qint64 request_id = 0;
  enum class Request {
    Context,
    Submit,
    Result
  } request_kind = Request::Context;

  void controls() {
    const bool idle = request_id == 0 && ticket.isEmpty();
    const int row = nodes->currentRow();
    nodes->setEnabled(idle);
    up->setEnabled(idle && row > 0);
    down->setEnabled(idle && row >= 0 && row + 1 < nodes->count());
    apply->setEnabled(idle && viewport.rpcReady() && !digest.isEmpty());
    reset->setEnabled(apply->isEnabled());
    retry->setEnabled(viewport.rpcReady() && request_id == 0);
  }
  void request(Request kind, const QString &method,
               const QJsonObject &params = {}) {
    if (request_id)
      return;
    QString error;
    request_kind = kind;
    request_id = viewport.requestRpc(method, params, &error);
    if (!request_id) {
      poll.stop();
      notice->setText(error);
    }
    controls();
  }
  void readResult() {
    if (!ticket.isEmpty())
      request(Request::Result, "get_edit_result", {{"ticket", ticket}});
  }
  // FO-1s, decision 1 (engine FO-1): a refused order is one sentence, and
  // the two rows it put the wrong way round are red until a row moves or the
  // order is sent again.
  static constexpr int ConflictRole = Qt::UserRole + 1;
  void refused(const QJsonObject &response) {
    const auto error = nlohmann::json::parse(
        QJsonDocument{response.value("error").toObject()}.toJson(QJsonDocument::Compact).toStdString(),
        nullptr, false);
    const auto sentence =
        error.is_discarded() ? QString{} : QString::fromStdString(graphLoopRefusalDescription(error, false));
    notice->setText(sentence.isEmpty() ? orderRefusal(response) : sentence);
    clearConflict();
    if (error.is_discarded()) return;
    const auto fields = graphRefusalFields(error, "frame_node_order.conflicts_dependency@1");
    if (!fields || !fields->is_object()) return;
    const auto first = QString::fromStdString(fields->value("first", std::string{}));
    const auto second = QString::fromStdString(fields->value("second", std::string{}));
    for (int row = 0; row < nodes->count(); ++row) {
      auto *item = nodes->item(row);
      if (item->text() != first && item->text() != second) continue;
      item->setForeground(QColor(0xd9, 0x4c, 0x3d));
      item->setData(ConflictRole, true);
      item->setToolTip(notice->text());
    }
  }
  void clearConflict() {
    for (int row = 0; row < nodes->count(); ++row) {
      auto *item = nodes->item(row);
      if (!item->data(ConflictRole).toBool()) continue;
      item->setData(Qt::ForegroundRole, QVariant{});
      item->setData(ConflictRole, QVariant{});
      item->setToolTip({});
    }
  }
  void submit(bool dependency_order) {
    clearConflict();
    auto edited = orders;
    if (dependency_order)
      edited.remove(graph);
    else {
      QJsonArray names;
      for (int row = 0; row < nodes->count(); ++row)
        names.append(nodes->item(row)->text());
      edited.insert(graph, names);
    }
    request(Request::Submit, "edit_frame_node_order",
            {{"base_source_digest", digest}, {"orders", edited}});
  }
  void receive(qint64 id, const QByteArray &bytes) {
    if (!id || id != request_id)
      return;
    request_id = 0;
    QString error;
    const auto response = decodeObject(bytes, &error);
    if (!response) {
      poll.stop();
      notice->setText(error);
      controls();
      return;
    }
    if (request_kind == Request::Context) {
      const auto next_digest =
          response->value("source_digest").toObject().value("hex").toString();
      const auto published_generation = response->value("published_generation").toInteger(-1);
      digest.clear();
      if (published_generation <= 0 || static_cast<quint64>(published_generation) != expected_generation) {
        notice->setText(tr("The Frame Plan is stale. Refresh the Frame Plan and reopen this dialog."));
      } else if (next_digest.isEmpty() || !response->value("orders").isObject()) {
        notice->setText(tr("Invalid node order response."));
      } else {
        digest = next_digest;
        orders = response->value("orders").toObject();
        notice->setText(
            tr("Move nodes, then Apply. Data dependencies and the AS "
               "publication boundary are enforced by the compiler."));
      }
    } else if (request_kind == Request::Submit) {
      if (response->value("status").toString() == "accepted") {
        ticket = response->value("ticket").toString();
        if (ticket.isEmpty())
          notice->setText(tr("The engine returned no edit ticket."));
        else {
          poll.start();
          readResult();
        }
      } else
        refused(*response);
    } else if (response->value("status").toString() != "pending") {
      poll.stop();
      ticket.clear();
      if (const auto completion = completeEdit(*response, restart);
          completion.committed) {
        // RSX: Render features says the engine restarts; nothing is read
        // from the engine about to stop.
        if (!completion.restart_required) committed();
        close();
        return;
      }
      refused(*response);
    }
    controls();
  }

  // GL-5e, decision 8: a refused Apply, in the sentence Studio's refusal
  // table has for its code (where frame_node_order's own codes go once the
  // engine names them, FO-1), else the engine's message and code as they
  // come ("logical graph contains a dependency cycle: main_render
  // (render_pipeline_preflight_failed)").
  static QString orderRefusal(const QJsonObject &response) {
    const auto error = response.value(QStringLiteral("error")).toObject();
    const auto known = knownRefusalDescription(
        error.value(QStringLiteral("code")).toString().toStdString(),
        error.value(QStringLiteral("message")).toString().toStdString());
    return known.empty() ? responseError(response) : QString::fromStdString(known);
  }

public:
  FrameNodeOrderDialog(EmbeddedViewport &embedded, QWidget *parent,
                       QString graph_name, const QStringList &names, quint64 generation,
                       std::function<void()> on_committed)
      : QDialog(parent), viewport(embedded), committed(std::move(on_committed)),
        graph(std::move(graph_name)), expected_generation(generation) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Node order: %1").arg(graph));
    setObjectName("pelican.frameNodeOrder");
    resize(560, 480);
    auto *layout = new QVBoxLayout(this);
    nodes = new QListWidget(this);
    nodes->setObjectName("pelican.frameNodeOrder.nodes");
    nodes->addItems(names);
    layout->addWidget(nodes);
    notice = new QLabel(tr("Loading saved orders..."), this);
    // GL-5d: named, so a scenario reads what the engine said of Apply.
    notice->setObjectName("pelican.frameNodeOrder.notice");
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *buttons = new QHBoxLayout;
    up = new QPushButton(tr("Up"), this);
    down = new QPushButton(tr("Down"), this);
    apply = new QPushButton(tr("Apply"), this);
    reset = new QPushButton(tr("Use dependency order"), this);
    retry = new QPushButton(tr("Retry"), this);
    up->setObjectName("pelican.frameNodeOrder.up");
    down->setObjectName("pelican.frameNodeOrder.down");
    apply->setObjectName("pelican.frameNodeOrder.apply");
    reset->setObjectName("pelican.frameNodeOrder.reset");
    retry->setObjectName("pelican.frameNodeOrder.retry");
    for (auto *button : {up, down, apply, reset, retry})
      buttons->addWidget(button);
    layout->addLayout(buttons);
    connect(nodes, &QListWidget::currentRowChanged, this,
            [this] { controls(); });
    const auto move = [this](int offset) {
      const int row = nodes->currentRow();
      if (row < 0 || row + offset < 0 || row + offset >= nodes->count())
        return;
      clearConflict();
      nodes->insertItem(row + offset, nodes->takeItem(row));
      nodes->setCurrentRow(row + offset);
    };
    connect(up, &QPushButton::clicked, this, [move] { move(-1); });
    connect(down, &QPushButton::clicked, this, [move] { move(1); });
    connect(apply, &QPushButton::clicked, this, [this] { submit(false); });
    connect(reset, &QPushButton::clicked, this, [this] { submit(true); });
    connect(retry, &QPushButton::clicked, this, [this] {
      if (!ticket.isEmpty())
        readResult();
      else
        request(Request::Context, "get_frame_node_order");
    });
    poll.setInterval(100);
    connect(&poll, &QTimer::timeout, this, [this] { readResult(); });
    connect(&viewport, &EmbeddedViewport::inspectorRpcSucceeded, this,
            [this](qint64 id, const QByteArray &bytes) { receive(id, bytes); });
    connect(&viewport, &EmbeddedViewport::inspectorRpcFailed, this,
            [this](qint64 id, const QString &error) {
              if (!id || id != request_id)
                return;
              request_id = 0;
              poll.stop();
              notice->setText(error);
              controls();
            });
    connect(&viewport, &EmbeddedViewport::engineRpcBecameUnavailable, this,
            [this] {
              request_id = 0;
              ticket.clear();
              digest.clear();
              poll.stop();
              notice->setText(tr("Engine disconnected. The order is retained; "
                                 "retry after reconnecting."));
              controls();
            });
    connect(&viewport, &EmbeddedViewport::engineRpcBecameAvailable, this,
            [this] { controls(); });
    request(Request::Context, "get_frame_node_order");
  }
};

class ComputeTaskDialog final : public QDialog, public RestartsEngine {
  EmbeddedViewport &viewport;
  std::function<void()> committed;
  // Q-10: the rendering document's own reference, asked for each time it is
  // needed because Render features may learn it after this dialog opened.
  std::function<QString()> root_reference;
  QComboBox *task_box;
  QPlainTextEdit *editor;
  QLabel *source;
  QLabel *notice;
  QPushButton *apply;
  QPushButton *refresh;
  QPushButton *new_compute;
  QPushButton *new_ray_tracing;
  QPushButton *remove;
  // Q-12: beside the notice, for a task a render.compute node made.
  QPushButton *open_in_graph;
  QLineEdit *destination;
  QComboBox *dispatch_mode;
  std::array<QLineEdit *, 3> dispatch_axes;
  QLineEdit *dispatch_reference;
  QLineEdit *dispatch_offset;
  QPushButton *update_dispatch;
  QWidget *dispatch_form;
  // LP-4b: which views of its family the task runs for. The axes are the
  // frame plan's (view_axes of the task's node); a pick is written into the
  // JSON draft, which Apply sends whole.
  ViewSelectField *view_field;
  RenderFeaturesWidget::TaskViewAxesProvider view_axes;
  TaskConnectionGraphics *connection_graph;
  std::vector<TaskConnectionGraphics::Resource> resource_catalog;
  QTableWidget *connections;
  QPushButton *update_connections;
  QString connections_source;
  using Json = nlohmann::json;
  QJsonArray tasks;
  struct TaskDraft {
    QString text;
    QString root_digest;
    QString source_digest;
  };
  std::array<TaskDraft, 2> new_drafts;
  std::array<bool, 2> new_dirty{};
  std::array<QString, 2> destinations;
  QString submitted_name;
  int submitted_creation = -1;
  bool discard_new_drafts_on_read = false;
  QHash<QString, TaskDraft> drafts;
  QHash<QString, QString> original_declarations;
  QHash<QString, TaskDraft> retained_drafts;
  QString root_digest, ticket, requested_task;
  QTimer poll;
  qint64 request_id = 0;
  enum class Request { Read, Submit, Result } request_kind = Request::Read;
  int creationIndex() const {
    const int index = task_box->currentIndex() - tasks.size();
    return index >= 0 && index < 2 ? index : -1;
  }
  // CT-6s, decision 3: the ray tracing set a new ray tracing draft names,
  // written into the project once per dialog; empty when it has no project
  // or the write failed (the notice says why).
  QString ray_template_reference;
  QString ray_template_failure;
  QString rayTracingTemplateReference() {
    if (!ray_template_reference.isEmpty() || !ray_template_failure.isEmpty())
      return ray_template_reference;
    const std::filesystem::path root{viewport.projectRoot().toStdWString()};
    if (root.empty()) {
      ray_template_failure = tr("No project is open to write the shaders into.");
      return {};
    }
    try {
      const auto written =
          writeRayTracingTemplate(root, rayTracingTemplateName(root));
      if (written.created)
        ray_template_reference = QString::fromStdString(written.reference);
      else
        ray_template_failure = tr("The shaders' name was taken meanwhile.");
    } catch (const std::exception &error) {
      ray_template_failure = QString::fromUtf8(error.what());
    }
    return ray_template_reference;
  }
  void initializeNewDraft(int index) {
    if (!new_drafts[index].root_digest.isEmpty())
      return;
    QJsonObject task{{"name", index == 0 ? "new_compute" : "new_ray_tracing"}};
    if (index == 0) {
      task.insert("shader", "project://shaders/compute");
      task.insert("dispatch", QJsonObject{{"groups", QJsonArray{1, 1, 1}}});
    } else {
      // CT-6s, decision 3: the set the Graph's New... writes (raygen + miss
      // + closest hit), written under a free name -- a draft never names a
      // shader with no file behind it. A project with a ray tracing task
      // starts from that task instead (below), and nothing is written.
      const bool copies = std::any_of(tasks.begin(), tasks.end(), [](const auto &entry) {
        return entry.toObject().value("declaration").toObject().contains("ray_tracing");
      });
      const auto reference = copies ? QString{} : rayTracingTemplateReference();
      task.insert("ray_tracing",
                  QJsonObject{{"raygen", reference},
                              {"miss", QJsonArray{reference}},
                              {"closesthit", QJsonArray{reference}}});
      // The image its raygen writes: a target of that name is the task's
      // to declare, or the connections below point the port elsewhere.
      const auto port = QString::fromUtf8(RayTracingTemplatePort.data(),
                                          static_cast<qsizetype>(RayTracingTemplatePort.size()));
      const auto target = reference.isEmpty()
                              ? port
                              : reference.mid(reference.lastIndexOf('/') + 1) + "_" + port;
      task.insert("writes", QJsonArray{target});
      task.insert("resource_ports",
                  QJsonObject{{port, QJsonObject{{"resource", target}, {"access", "storage"}}}});
      task.insert("dispatch", QJsonObject{{"rays_from", QJsonObject{{"port", port}}}});
    }
    QString exact_ray_template;
    if (index == 1) {
      for (const auto &entry : tasks) {
        const auto declaration =
            entry.toObject().value("declaration").toObject();
        if (!declaration.contains("ray_tracing"))
          continue;
        auto exact =
            Json::parse(original_declarations
                            .value(entry.toObject().value("name").toString())
                            .toStdString());
        exact["name"] = "new_ray_tracing";
        exact_ray_template = QString::fromStdString(exact.dump(2));
        break;
      }
    }
    new_drafts[index] = {
        QString::fromUtf8(QJsonDocument(task).toJson(QJsonDocument::Indented)),
        root_digest,
        {}};
    if (!exact_ray_template.isEmpty())
      new_drafts[index].text = exact_ray_template;
    destinations[index] = index == 0
                              ? "project://features/new_compute.json"
                              : "project://features/new_ray_tracing.json";
  }
  QJsonObject selected() const {
    const int index = task_box->currentIndex();
    return index < 0 || index >= tasks.size() ? QJsonObject{}
                                              : tasks[index].toObject();
  }
  QString rootReference() const {
    return root_reference ? root_reference() : QString{};
  }
  // Q-12 (MG-3b) before Q-10 (MG-2b): a task a render.compute node made is
  // edited in the Graph tab; a task at the rendering document's root has no
  // declaration to write back, whatever the catalog row says.
  FramePlanTaskLock lock(const QJsonObject &task) const {
    if (task.isEmpty())
      return FramePlanTaskLock::read_only;
    return framePlanTaskLock(
        task.value("origin").toString().toStdString(),
        task.value("editable").toBool(),
        task.value("source_reference").toString().toStdString(),
        task.value("provider_feature").toString().toStdString(),
        rootReference().toStdString());
  }
  std::optional<FramePlanGraphNodeRef> graphNode(const QJsonObject &task) const {
    if (task.isEmpty())
      return std::nullopt;
    return framePlanTaskGraphNode(Json::parse(
        QJsonDocument(task).toJson(QJsonDocument::Compact).toStdString(),
        nullptr, false));
  }
  bool editableRow(const QJsonObject &task) const {
    return lock(task) == FramePlanTaskLock::none;
  }
  void openInGraph() {
    const auto source = creationIndex() < 0 ? graphNode(selected())
                                            : std::nullopt;
    if (!source || !open_graph_node)
      return;
    if (!open_graph_node(QString::fromStdString(source->scope),
                         QString::fromStdString(source->node),
                         QString::fromStdString(source->instance)))
      notice->setText(
          tr("The Graph tab has no node %1 in %2 yet: refresh it.")
              .arg(QString::fromStdString(source->node),
                   source->scope == "root"
                       ? tr("the root")
                       : QString::fromStdString(source->scope)));
  }

  // Q-12: the Graph tab's reveal (scope, node, instance); false when it could
  // not show that node.
  RenderFeaturesWidget::GraphNodeOpenAction open_graph_node;

public:
  void setGraphNodeOpenAction(RenderFeaturesWidget::GraphNodeOpenAction action) {
    open_graph_node = std::move(action);
    controls();
  }
  void setViewAxesProvider(RenderFeaturesWidget::TaskViewAxesProvider provider) {
    view_axes = std::move(provider);
    loadView();
    controls();
  }

private:
  // LP-4b: the View field for the draft in the editor. Hidden for a new
  // task (it has no node yet) and for a family without axes.
  void loadView() {
    const auto task =
        Json::parse(editor->toPlainText().toStdString(), nullptr, false);
    std::vector<FramePlanViewAxis> axes;
    if (creationIndex() < 0 && task.is_object() && view_axes)
      axes = view_axes(selected().value("name").toString());
    const bool shown =
        task.is_object() &&
        view_field->load(axes, framePlanDeclaredViewSelect(task),
                         task.value("view_family", std::string{}));
    if (!shown) view_field->clear();
    view_field->setVisible(shown);
  }
  // The draft was retyped: the field follows it unless it already says the
  // same (a pick below wrote it).
  void followDraftView() {
    const auto task =
        Json::parse(editor->toPlainText().toStdString(), nullptr, false);
    if (task.is_object() && view_field->hasRows() &&
        framePlanViewSelectJson(framePlanDeclaredViewSelect(task)) ==
            framePlanViewSelectJson(view_field->selection()))
      return;
    loadView();
  }
  // A pick in the field: the draft's view_select, and nothing else, changes.
  void viewPicked() {
    auto task =
        Json::parse(editor->toPlainText().toStdString(), nullptr, false);
    if (!task.is_object()) {
      notice->setText(tr("Fix the task JSON before choosing views."));
      return;
    }
    const auto chosen = view_field->selection();
    if (chosen.empty())
      task.erase("view_select");
    else
      task["view_select"] = framePlanViewSelectJson(chosen);
    editor->setPlainText(QString::fromStdString(task.dump(2)));
    notice->setText(tr("View updated in the JSON draft. Apply task to save."));
  }
  void loadDispatch() {
    const auto task =
        Json::parse(editor->toPlainText().toStdString(), nullptr, false);
    const QSignalBlocker blocker(dispatch_mode);
    dispatch_mode->clear();
    if (!task.is_object()) {
      dispatch_form->setEnabled(false);
      return;
    }
    const bool ray = task.contains("ray_tracing");
    if (!ray)
      dispatch_mode->addItem(tr("Default (1, 1, 1)"), "default");
    dispatch_mode->addItem(ray ? tr("Ray dimensions") : tr("Workgroup counts"),
                           ray ? "rays" : "groups");
    dispatch_mode->addItem(tr("From image port"),
                           ray ? "rays_from" : "groups_from");
    dispatch_mode->addItem(tr("Indirect buffer"), "indirect");
    const auto dispatch = task.value("dispatch", Json::object());
    QString mode = ray ? "rays" : "default";
    if (dispatch.is_object()) {
      for (const auto *key :
           {"groups", "groups_from", "rays", "rays_from", "indirect"})
        if (dispatch.contains(key))
          mode = key;
      for (int index = 0; index < 3; ++index) {
        Json value = 1;
        const auto dimensions = dispatch.find(ray ? "rays" : "groups");
        if (dimensions != dispatch.end()) {
          if (dimensions->is_array() && dimensions->size() == 3)
            value = dimensions->at(index);
          else if (dimensions->is_object())
            value =
                dimensions->value(std::array{"x", "y", "z"}[index], Json(1));
        }
        dispatch_axes[index]->setText(QString::fromStdString(value.dump()));
      }
      const auto detail = dispatch.value(mode.toStdString(), Json::object());
      dispatch_reference->clear();
      dispatch_offset->setText("0");
      if (detail.is_object()) {
        const auto reference =
            detail.find(mode == "indirect" ? "buffer" : "port");
        if (reference != detail.end() && reference->is_string())
          dispatch_reference->setText(
              QString::fromStdString(reference->get<std::string>()));
        if (detail.contains("offset"))
          dispatch_offset->setText(
              QString::fromStdString(detail.at("offset").dump()));
      }
    }
    dispatch_mode->setCurrentIndex(dispatch_mode->findData(mode));
    dispatchFields();
  }
  void dispatchFields() {
    const auto mode = dispatch_mode->currentData().toString();
    for (auto *axis : dispatch_axes)
      axis->setEnabled(mode == "groups" || mode == "rays");
    dispatch_reference->setEnabled(mode.endsWith("_from") ||
                                   mode == "indirect");
    dispatch_reference->setPlaceholderText(mode == "indirect"
                                               ? tr("Buffer resource name")
                                               : tr("Image port name"));
    dispatch_offset->setEnabled(mode == "indirect");
  }
  void updateDispatch() {
    auto task =
        Json::parse(editor->toPlainText().toStdString(), nullptr, false);
    if (!task.is_object()) {
      notice->setText(tr("Fix the task JSON before updating dispatch."));
      return;
    }
    auto dispatch = task.value("dispatch", Json::object());
    if (!dispatch.is_object()) {
      notice->setText(tr("Dispatch must be a JSON object."));
      return;
    }
    const auto mode = dispatch_mode->currentData().toString();
    if (mode.isEmpty())
      return;
    const auto previous = dispatch.value(mode.toStdString(), Json::object());
    for (const auto *key :
         {"groups", "groups_from", "rays", "rays_from", "indirect"})
      dispatch.erase(key);
    if (mode == "groups" || mode == "rays") {
      auto dimensions = Json::array();
      for (auto *axis : dispatch_axes) {
        bool valid = false;
        const auto number = axis->text().toULongLong(&valid);
        if (!valid || number == 0 || number > UINT32_MAX) {
          notice->setText(
              tr("Each dimension must be an integer from 1 to 4294967295."));
          return;
        }
        dimensions.push_back(number);
      }
      dispatch[mode.toStdString()] = dimensions;
    } else if (mode != "default") {
      if (dispatch_reference->text().isEmpty()) {
        notice->setText(tr("Enter a port or buffer name."));
        return;
      }
      auto detail = previous.is_object() ? previous : Json::object();
      detail[mode == "indirect" ? "buffer" : "port"] =
          dispatch_reference->text().toStdString();
      if (mode == "indirect") {
        bool valid = false;
        const auto offset = dispatch_offset->text().toULongLong(&valid);
        if (!valid || offset % 4 != 0) {
          notice->setText(tr("Offset must be a nonnegative uint64 integer "
                             "aligned to 4 bytes."));
          return;
        }
        detail["offset"] = offset;
      }
      dispatch[mode.toStdString()] = detail;
    }
    if (mode == "default" && dispatch.empty())
      task.erase("dispatch");
    else
      task["dispatch"] = dispatch;
    editor->setPlainText(QString::fromStdString(task.dump(2)));
    notice->setText(
        tr("Dispatch updated in the JSON draft. Apply task to save."));
  }
  void loadConnections() {
    connections_source = editor->toPlainText();
    const auto task = Json::parse(connections_source.toStdString(), nullptr, false);
    const QSignalBlocker blocker(connections);
    connections->setRowCount(0);
    connection_graph->setConnections({}, resource_catalog);
    if (!task.is_object() || !task.contains("resource_ports")) return;
    try {
      (void)rebindTaskResources(task, {});
      std::map<std::string, QStringList> aliases;
      for (const auto &[port, declaration] : task.at("resource_ports").items()) {
        const auto resource = declaration.is_string() ? declaration.get<std::string>() : declaration.value("resource", port);
        auto label = QString::fromStdString(port);
        if (declaration.is_object()) {
          auto abi = declaration;
          abi.erase("resource");
          if (!abi.empty()) label += " " + QString::fromStdString(abi.dump());
        }
        aliases[resource].push_back(label);
      }
      std::vector<TaskConnectionGraphics::PortGroup> groups;
      for (const auto &[resource, ports] : aliases) {
        groups.push_back({QString::fromStdString(resource), ports});
        const int row = connections->rowCount();
        connections->insertRow(row);
        auto *names = new QTableWidgetItem(ports.join("\n"));
        names->setFlags(names->flags() & ~Qt::ItemIsEditable);
        names->setData(Qt::UserRole, QString::fromStdString(resource));
        connections->setItem(row, 0, names);
        connections->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(resource)));
      }
      connections->resizeRowsToContents();
      connection_graph->setConnections(std::move(groups), resource_catalog);
    } catch (const std::exception &) {
      // The JSON editor remains authoritative for malformed declarations.
      connections->setRowCount(0);
    }
  }
  void updateConnections() {
    if (connections_source != editor->toPlainText()) {
      notice->setText(tr("The JSON draft changed; reload its connections before applying.")); return;
    }
    const auto task = Json::parse(connections_source.toStdString(), nullptr, false);
    std::map<std::string, std::string> replacements;
    for (int row = 0; row < connections->rowCount(); ++row) {
      replacements.emplace(connections->item(row, 0)->data(Qt::UserRole).toString().toStdString(),
                           connections->item(row, 1)->text().toStdString());
    }
    try {
      const auto rebound = rebindTaskResources(task, replacements);
      editor->setPlainText(QString::fromStdString(rebound.dump(2)));
      notice->setText(tr("Connections updated in the JSON draft. Apply task to validate and save."));
    } catch (const std::exception &error) {
      notice->setText(QString::fromUtf8(error.what()));
    }
  }
  void controls() {
    const bool idle = request_id == 0 && ticket.isEmpty();
    task_box->setEnabled(idle);
    const bool creating = creationIndex() >= 0;
    const bool editable = creating || editableRow(selected());
    destination->setVisible(creating);
    destination->setEnabled(idle);
    apply->setText(creating ? tr("Create task") : tr("Apply task"));
    new_compute->setEnabled(idle && viewport.rpcReady() &&
                            !root_digest.isEmpty());
    new_ray_tracing->setEnabled(new_compute->isEnabled());
    remove->setEnabled(idle && viewport.rpcReady() && !creating && editable &&
                       !root_digest.isEmpty());
    editor->setReadOnly(!idle || !editable);
    connections->setEnabled(idle && editable);
    connection_graph->setEnabled(idle && editable);
    update_connections->setEnabled(idle && editable && connections->rowCount() > 0);
    dispatch_form->setEnabled(
        idle && editable &&
        Json::parse(editor->toPlainText().toStdString(), nullptr, false)
            .is_object());
    view_field->setEnabled(dispatch_form->isEnabled());
    apply->setEnabled(idle && viewport.rpcReady() && editable &&
                      !root_digest.isEmpty());
    refresh->setEnabled(viewport.rpcReady() && request_id == 0);
    const bool from_node = !creating && graphNode(selected()).has_value();
    open_in_graph->setVisible(from_node);
    open_in_graph->setEnabled(from_node && static_cast<bool>(open_graph_node));
  }
  void showTask() {
    const auto task = selected();
    const QSignalBlocker blocker(editor);
    if (const int index = creationIndex(); index >= 0) {
      initializeNewDraft(index);
      editor->setPlainText(new_drafts[index].text);
      const QSignalBlocker destination_blocker(destination);
      destination->setText(destinations[index]);
      source->setText(tr("Create a task in a new project feature file."));
      notice->setText(tr("Set a unique task name, destination, and existing "
                         "shader references. Add resources and other task "
                         "settings in JSON as needed. For ray tracing, match "
                         "the project ray group layout."));
      // CT-6s: what the ray tracing draft wrote, or why it could not.
      if (index == 1 && !ray_template_reference.isEmpty())
        notice->setText(tr("Wrote %1 (.rgen, .rmiss and .rchit) for this "
                           "task. Point its port at a storage image in "
                           "Connections, set the destination, and apply.")
                            .arg(ray_template_reference));
      else if (index == 1 && !ray_template_failure.isEmpty())
        notice->setText(tr("Could not write the ray tracing shaders: %1")
                            .arg(ray_template_failure));
      loadDispatch();
      loadConnections();
      loadView();
      controls();
      return;
    }
    editor->setPlainText(drafts.value(task.value("name").toString()).text);
    source->setText(tr("%1 (%2)\nSource: %3")
                        .arg(task.value("name").toString(),
                             task.value("kind").toString(),
                             task.value("source_reference").toString()));
    const auto locked = lock(task);
    notice->setText(
        locked == FramePlanTaskLock::graph_node
            ? QString::fromStdString(framePlanGraphNodeTaskReason(
                  task.value("read_only_reason").toString().toStdString()))
        : locked == FramePlanTaskLock::root
            ? QString::fromStdString(framePlanRootTaskRefusal(
                  task.value("name").toString().toStdString()))
        : locked == FramePlanTaskLock::none
            ? tr("Edit the task declaration. Keep its name unchanged.")
            : task.value("read_only_reason")
                  .toString(tr("This task source is read-only.")));
    loadDispatch();
    loadConnections();
    loadView();
    controls();
  }
  void request(Request kind, const QString &method, QJsonObject params = {}) {
    if (request_id)
      return;
    QString error;
    request_kind = kind;
    request_id = viewport.requestRpc(method, params, &error);
    if (!request_id) {
      if (kind == Request::Read)
        discard_new_drafts_on_read = false;
      poll.stop();
      notice->setText(error);
    } else if (kind == Request::Submit) {
      submitted_retried = std::exchange(retrying, false);
    }
    controls();
  }
  void result() {
    if (!ticket.isEmpty())
      request(Request::Result, QStringLiteral("get_edit_result"),
              {{"ticket", ticket}});
  }
  void read() {
    if (!ticket.isEmpty())
      result();
    else
      request(Request::Read, QStringLiteral("get_compute_task_authoring"));
  }
  void submit() {
    const auto task = selected();
    QJsonParseError error;
    const auto document =
        QJsonDocument::fromJson(editor->toPlainText().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
      notice->setText(tr("Task JSON must be an object. %1 (byte %2)")
                          .arg(error.errorString())
                          .arg(error.offset));
      return;
    }
    // Q-10: what goes out -- add, edit, or a refusal by name -- is decided
    // by computeTaskEditorRequest, the same function the tests read.
    const int creation = creationIndex();
    ComputeTaskEditorSubmit submission;
    submission.creating = creation >= 0;
    if (submission.creating) {
      submission.base_source_digest =
          new_drafts[creation].root_digest.toStdString();
      submission.destination_reference = destination->text().toStdString();
    } else {
      const auto draft = drafts.value(task.value("name").toString());
      submission.base_source_digest = draft.root_digest.toStdString();
      submission.task_name = task.value("name").toString().toStdString();
      submission.task_source_digest = draft.source_digest.toStdString();
      submission.source_reference =
          task.value("source_reference").toString().toStdString();
      submission.provider_feature =
          task.value("provider_feature").toString().toStdString();
      submission.editable = task.value("editable").toBool();
      submission.root_reference = rootReference().toStdString();
      submission.origin = task.value("origin").toString().toStdString();
      submission.read_only_reason =
          task.value("read_only_reason").toString().toStdString();
    }
    QJsonObject params;
    try {
      const auto outgoing = computeTaskEditorRequest(
          submission, Json::parse(editor->toPlainText().toStdString()));
      if (!outgoing.sendable()) {
        notice->setText(QString::fromStdString(outgoing.refusal));
        return;
      }
      params = exactJsonRpcObject(outgoing.params);
      submitted_remove = false;
      submitted_creation = creation;
      submitted_name = document.object().value("name").toString();
      request(Request::Submit, QString::fromStdString(outgoing.method), params);
    } catch (const std::exception &error) {
      notice->setText(QString::fromUtf8(error.what()));
    }
  }
  // --- Q-13. Another panel's commit moved the rendering document since this
  // editor read it, and the engine refused the edit for that. The drafts are
  // kept through one more read, the edit is sent once more against the root
  // revision read -- its task's own digest still guards the task -- and a
  // second refusal leaves it to Refresh.
  bool submitted_remove = false;
  bool submitted_retried = false;
  bool retrying = false;
  bool retry_pending = false;
  bool refusedForMovedDocument(const QJsonObject &response) {
    if (response.value("error").toObject().value("code").toString() !=
        QStringLiteral("external_modification"))
      return false;
    if (submitted_retried) {
      notice->setText(tr("The document changed outside this editor. Press "
                         "Refresh to read it again. (%1)")
                          .arg(responseError(response)));
      return true;
    }
    retainEditedDrafts();
    retry_pending = true;
    notice->setText(tr("The document changed since this editor read it; "
                       "reading it again..."));
    read();
    return true;
  }
  void resumeRetry() {
    retry_pending = false;
    if (submitted_creation >= 0)
      new_drafts[submitted_creation].root_digest = root_digest;
    else if (drafts.contains(submitted_name))
      drafts[submitted_name].root_digest = root_digest;
    retrying = true;
    if (submitted_remove)
      removeTask();
    else
      submit();
    if (std::exchange(retrying, false) && request_id == 0)
      notice->setText(tr("The document changed outside this editor. Press "
                         "Refresh to read it again."));
  }
  // --- Q-15. The digest the other panels share (Q-13): what this editor
  // reads it says, and when another panel's commit moves the document it
  // reads it again at once, keeping every draft, as the Pass Editor does.
  std::shared_ptr<RenderConfigDigest> digest_hub;
  // A move told while a request was out; followed when it comes back.
  bool reread_when_idle = false;
  // The read in flight was for a move: the drafts it keeps are sent against
  // the revision it reads (each task's own digest still guards the task).
  bool rebase_on_read = false;
  void retainEditedDrafts() {
    for (const auto &entry : tasks) {
      const auto name = entry.toObject().value("name").toString();
      if (drafts.value(name).text != original_declarations.value(name))
        retained_drafts.insert(name, drafts.value(name));
    }
  }
  // Another panel read or committed the document at `digest`.
  void documentMoved(const std::string &digest) {
    if (root_digest.isEmpty() || root_digest == QString::fromStdString(digest))
      return;
    if (request_id != 0 || !ticket.isEmpty() || retry_pending) {
      reread_when_idle = true;
      return;
    }
    retainEditedDrafts();
    rebase_on_read = true;
    notice->setText(tr("Another panel changed the document; reading it "
                       "again. Your edits are kept."));
    read();
  }
  void followWhenIdle() {
    if (!reread_when_idle || request_id != 0 || !ticket.isEmpty() ||
        retry_pending)
      return;
    reread_when_idle = false;
    if (digest_hub)
      documentMoved(digest_hub->current());
  }
  void rebaseRetainedDrafts() {
    for (auto it = retained_drafts.cbegin(); it != retained_drafts.cend(); ++it)
      if (drafts.contains(it.key()))
        drafts[it.key()].root_digest = root_digest;
    for (int index = 0; index < 2; ++index)
      if (new_dirty[index])
        new_drafts[index].root_digest = root_digest;
  }

public:
  void setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> hub) {
    digest_hub = std::move(hub);
    if (!digest_hub)
      return;
    digest_hub->subscribe(this, [this](const std::string &moved) {
      documentMoved(moved);
    });
    if (!root_digest.isEmpty())
      digest_hub->publish(root_digest.toStdString(), this);
  }

private:
  void removeTask() {
    const auto task = selected();
    if (!editableRow(task))
      return;
    submitted_remove = true;
    submitted_creation = -1;
    submitted_name = task.value("name").toString();
    const auto draft = drafts.value(submitted_name);
    request(Request::Submit, QStringLiteral("remove_compute_task"),
            {{"base_source_digest", draft.root_digest},
             {"base_task_source_digest", draft.source_digest},
             {"task_name", submitted_name}});
  }
  void receive(qint64 id, const QByteArray &bytes) {
    if (!id || id != request_id)
      return;
    request_id = 0;
    QString error;
    const auto response = decodeObject(bytes, &error);
    if (!response) {
      if (request_kind == Request::Read)
        discard_new_drafts_on_read = rebase_on_read = false;
      poll.stop();
      notice->setText(error);
      controls();
      return;
    }
    if (request_kind == Request::Read) {
      const auto digest =
          response->value("source_digest").toObject().value("hex").toString();
      if (digest.isEmpty() || !response->value("tasks").isArray()) {
        discard_new_drafts_on_read = false;
        rebase_on_read = false;
        notice->setText(
            tr("The engine returned an invalid task authoring context."));
      } else {
        const auto previous =
            requested_task.isEmpty() ? task_box->currentText() : requested_task;
        root_digest = digest;
        tasks = response->value("tasks").toArray();
        resource_catalog.clear();
        for (const auto &entry : response->value("resources").toArray()) {
          const auto resource = entry.toObject();
          resource_catalog.push_back({resource.value("name").toString(),
                                      resource.value("kind").toString()});
        }
        drafts.clear();
        original_declarations.clear();
        if (discard_new_drafts_on_read) {
          new_drafts = {};
          new_dirty = {};
          discard_new_drafts_on_read = false;
        }
        for (int index = 0; index < 2; ++index) {
          if (!new_dirty[index])
            new_drafts[index] = {};
        }
        {
          const QSignalBlocker blocker(task_box);
          task_box->clear();
          for (const auto &entry : tasks) {
            const auto task = entry.toObject();
            const auto name = task.value("name").toString();
            task_box->addItem(name);
            const auto original_text = task.value("declaration_json").toString();
            original_declarations.insert(name, original_text);
            drafts.insert(name, retained_drafts.value(
                                    name, TaskDraft{original_text, root_digest,
                                                    task.value("source_digest")
                                                        .toObject()
                                                        .value("hex")
                                                        .toString()}));
          }
          task_box->addItem(tr("(New compute task)"));
          task_box->addItem(tr("(New ray tracing task)"));
          const int index = task_box->findText(previous);
          if (index >= 0)
            task_box->setCurrentIndex(index);
        }
        if (std::exchange(rebase_on_read, false))
          rebaseRetainedDrafts();
        retained_drafts.clear();
        // Q-15: a read for another panel's move keeps the caret where the
        // author left it, when the draft under it is the same text.
        const auto text_before = editor->toPlainText();
        const auto caret = editor->textCursor().position();
        showTask();
        if (editor->toPlainText() == text_before) {
          auto cursor = editor->textCursor();
          cursor.setPosition(std::min(caret, int(text_before.size())));
          editor->setTextCursor(cursor);
        }
        if (!requested_task.isEmpty() && task_box->findText(requested_task) < 0)
          notice->setText(tr("Select the authored task corresponding to %1.")
                              .arg(requested_task));
        requested_task.clear();
        // Q-15: what this editor read, the other panels are told.
        if (digest_hub)
          digest_hub->publish(root_digest.toStdString(), this);
        if (retry_pending) {
          controls();
          resumeRetry();
        }
      }
    } else if (request_kind == Request::Submit) {
      if (response->value("status").toString() == "accepted") {
        ticket = response->value("ticket").toString();
        if (!ticket.isEmpty()) {
          poll.start();
          result();
        } else
          notice->setText(tr("The engine returned no edit ticket."));
      } else if (!refusedForMovedDocument(*response))
        notice->setText(responseError(*response));
    } else if (response->value("status").toString() != "pending") {
      poll.stop();
      ticket.clear();
      if (const auto completion = completeEdit(*response, restart);
          completion.committed) {
        for (const auto &entry : tasks) {
          const auto task = entry.toObject();
          const auto name = task.value("name").toString();
          const auto original = original_declarations.value(name);
          if (name != submitted_name && drafts.value(name).text != original)
            retained_drafts.insert(name, drafts.value(name));
        }
        if (submitted_creation >= 0) {
          new_drafts[submitted_creation] = {};
          new_dirty[submitted_creation] = false;
          requested_task = submitted_name;
        }
        // RSX: the engine restarts; the editor reads the tasks again from
        // the engine that comes back (Refresh).
        if (completion.restart_required) {
          notice->setText(QString::fromStdString(completion.restart_line));
        } else {
          committed();
          read();
        }
      } else if (!refusedForMovedDocument(*response))
        notice->setText(responseError(*response));
    }
    controls();
    followWhenIdle();
  }

public:
  ComputeTaskDialog(EmbeddedViewport &embedded, QWidget *parent, QString task,
                    std::function<void()> on_committed, const QStringList &outputs = {},
                    std::function<QString()> root = {})
      : QDialog(parent), viewport(embedded), committed(std::move(on_committed)),
        root_reference(std::move(root)), requested_task(std::move(task)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Compute and ray tracing tasks"));
    setObjectName(QStringLiteral("pelican.computeTasks"));
    resize(720, 600);
    auto *layout = new QVBoxLayout(this);
    task_box = new QComboBox(this);
    task_box->setObjectName(QStringLiteral("pelican.computeTasks.task"));
    layout->addWidget(task_box);
    source = new QLabel(this);
    source->setWordWrap(true);
    layout->addWidget(source);
    destination = new QLineEdit(this);
    destination->setObjectName(
        QStringLiteral("pelican.computeTasks.destination"));
    destination->setPlaceholderText(tr("New project:// feature file"));
    destination->setToolTip(tr(
        "A new project:// JSON file; existing files cannot be overwritten."));
    layout->addWidget(destination);
    editor = new QPlainTextEdit(this);
    editor->setObjectName(QStringLiteral("pelican.computeTasks.declaration"));
    layout->addWidget(editor);
    dispatch_form = new QWidget(this);
    auto *dispatch_layout = new QFormLayout(dispatch_form);
    dispatch_mode = new QComboBox(dispatch_form);
    dispatch_mode->setObjectName("pelican.computeTasks.dispatchMode");
    dispatch_layout->addRow(tr("Dispatch"), dispatch_mode);
    auto *axes = new QWidget(dispatch_form);
    auto *axes_layout = new QHBoxLayout(axes);
    axes_layout->setContentsMargins(0, 0, 0, 0);
    for (int index = 0; index < 3; ++index) {
      dispatch_axes[index] = new QLineEdit("1", axes);
      dispatch_axes[index]->setObjectName(
          QString("pelican.computeTasks.dispatchAxis%1").arg(index));
      axes_layout->addWidget(dispatch_axes[index]);
    }
    dispatch_layout->addRow(tr("X / Y / Z"), axes);
    dispatch_reference = new QLineEdit(dispatch_form);
    dispatch_reference->setObjectName("pelican.computeTasks.dispatchReference");
    dispatch_layout->addRow(tr("Port / buffer"), dispatch_reference);
    dispatch_offset = new QLineEdit("0", dispatch_form);
    dispatch_offset->setObjectName("pelican.computeTasks.dispatchOffset");
    dispatch_layout->addRow(tr("Byte offset"), dispatch_offset);
    update_dispatch =
        new QPushButton(tr("Update dispatch JSON"), dispatch_form);
    update_dispatch->setObjectName("pelican.computeTasks.updateDispatch");
    dispatch_layout->addRow(update_dispatch);
    layout->addWidget(dispatch_form);
    view_field = new ViewSelectField(QStringLiteral("pelican.computeTasks"), this);
    view_field->hide();
    view_field->changed = [this] { viewPicked(); };
    layout->addWidget(view_field);
    connect(dispatch_mode, &QComboBox::currentIndexChanged, this,
            [this] { dispatchFields(); });
    connect(update_dispatch, &QPushButton::clicked, this,
            [this] { updateDispatch(); });
    connection_graph = new TaskConnectionGraphics(this);
    if (!outputs.isEmpty()) {
      // N-7 rule 1: the procedure is on the thing the procedure acts on.
      connection_graph->setToolTip(tr("Select a task port group, then an output from the dragged source node. Apply task validates and saves the connection."));
      connection_graph->setTargetFilter(outputs);
    }
    layout->addWidget(connection_graph);
    connection_graph->diagnostic = [this](const QString &message) { notice->setText(message); };
    connection_graph->resourceRebound = [this](const QString &from, const QString &to) {
      try {
        const auto task = Json::parse(editor->toPlainText().toStdString());
        const auto rebound = rebindTaskResources(task, {{from.toStdString(), to.toStdString()}});
        editor->setPlainText(QString::fromStdString(rebound.dump(2)));
        notice->setText(tr("Connection updated in the JSON draft. Apply task to validate and save."));
      } catch (const std::exception &error) {
        notice->setText(QString::fromUtf8(error.what()));
      }
    };
    connections = new QTableWidget(0, 2, this);
    connections->setObjectName("pelican.computeTasks.connections");
    connections->setHorizontalHeaderLabels({tr("Ports / ABI (fixed)"), tr("Resource")});
    connections->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    connections->setMaximumHeight(180);
    connections->setToolTip(tr("All ports sharing an original resource are reconnected together. Resource compatibility is checked when applying the task."));
    layout->addWidget(connections);
    update_connections = new QPushButton(tr("Update connections JSON"), this);
    update_connections->setObjectName("pelican.computeTasks.updateConnections");
    layout->addWidget(update_connections);
    connect(update_connections, &QPushButton::clicked, this, [this] { updateConnections(); });
    notice = new QLabel(this);
    notice->setObjectName(QStringLiteral("pelican.computeTasks.notice"));
    notice->setWordWrap(true);
    // Q-12: the way to where a node's task is edited sits on the line that
    // says so.
    open_in_graph = new QPushButton(tr("Open in Graph"), this);
    open_in_graph->setObjectName(
        QStringLiteral("pelican.computeTasks.openInGraph"));
    open_in_graph->setToolTip(
        tr("Show the render.compute node that makes this task in the Graph "
           "tab."));
    open_in_graph->hide();
    auto *notice_row = new QHBoxLayout;
    notice_row->addWidget(notice, 1);
    notice_row->addWidget(open_in_graph, 0, Qt::AlignTop);
    layout->addLayout(notice_row);
    connect(open_in_graph, &QPushButton::clicked, this,
            [this] { openInGraph(); });
    auto *buttons = new QHBoxLayout;
    refresh = new QPushButton(tr("Refresh"), this);
    apply = new QPushButton(tr("Apply task"), this);
    refresh->setObjectName(QStringLiteral("pelican.computeTasks.refresh"));
    apply->setObjectName(QStringLiteral("pelican.computeTasks.apply"));
    refresh->setToolTip(
        tr("Reload declarations and discard local edits for all tasks."));
    new_compute = new QPushButton(tr("New compute"), this);
    new_ray_tracing = new QPushButton(tr("New ray tracing"), this);
    remove = new QPushButton(tr("Delete task"), this);
    new_compute->setObjectName(
        QStringLiteral("pelican.computeTasks.newCompute"));
    new_ray_tracing->setObjectName(
        QStringLiteral("pelican.computeTasks.newRayTracing"));
    remove->setObjectName(QStringLiteral("pelican.computeTasks.remove"));
    remove->setToolTip(tr("Delete the selected task from its source document. "
                          "The feature file remains."));
    buttons->addWidget(new_compute);
    buttons->addWidget(new_ray_tracing);
    buttons->addWidget(remove);
    buttons->addWidget(refresh);
    buttons->addWidget(apply);
    layout->addLayout(buttons);
    connect(task_box, &QComboBox::currentIndexChanged, this,
            [this] { showTask(); });
    connect(editor, &QPlainTextEdit::textChanged, this, [this] {
      loadDispatch();
      loadConnections();
      followDraftView();
      controls();
      if (const int index = creationIndex(); index >= 0) {
        new_drafts[index].text = editor->toPlainText();
        new_dirty[index] = true;
      } else if (task_box->currentIndex() >= 0)
        drafts[selected().value("name").toString()].text =
            editor->toPlainText();
    });
    connect(refresh, &QPushButton::clicked, this, [this] {
      if (!ticket.isEmpty()) {
        result();
        return;
      }
      retained_drafts.clear();
      discard_new_drafts_on_read = true;
      read();
    });
    connect(destination, &QLineEdit::textChanged, this,
            [this](const QString &text) {
              if (const int index = creationIndex(); index >= 0) {
                destinations[index] = text;
                new_dirty[index] = true;
              }
            });
    connect(new_compute, &QPushButton::clicked, this,
            [this] { task_box->setCurrentIndex(tasks.size()); });
    connect(new_ray_tracing, &QPushButton::clicked, this,
            [this] { task_box->setCurrentIndex(tasks.size() + 1); });
    connect(remove, &QPushButton::clicked, this, [this] { removeTask(); });
    connect(apply, &QPushButton::clicked, this, [this] { submit(); });
    poll.setInterval(100);
    connect(&poll, &QTimer::timeout, this, [this] { result(); });
    connect(&viewport, &EmbeddedViewport::inspectorRpcSucceeded, this,
            [this](qint64 id, const QByteArray &bytes) { receive(id, bytes); });
    connect(&viewport, &EmbeddedViewport::inspectorRpcFailed, this,
            [this](qint64 id, const QString &error) {
              if (!id || id != request_id)
                return;
              request_id = 0;
              if (request_kind == Request::Read)
                discard_new_drafts_on_read = rebase_on_read = false;
              poll.stop();
              notice->setText(error);
              controls();
            });
    connect(&viewport, &EmbeddedViewport::engineRpcBecameUnavailable, this,
            [this] {
              request_id = 0;
              discard_new_drafts_on_read = false;
              ticket.clear();
              root_digest.clear();
              poll.stop();
              notice->setText(tr("Engine disconnected. Local edits are "
                                 "retained; refresh after reconnecting."));
              controls();
            });
    connect(&viewport, &EmbeddedViewport::engineRpcBecameAvailable, this,
            [this] { controls(); });
    read();
  }
};

class PresetEditDialog final : public QDialog, public RestartsEngine {
public:
  enum class Mode { Fork, Region };
private:
  Mode mode;
  EmbeddedViewport &viewport;
  std::function<void()> on_committed;
  QLabel *notice;
  QLineEdit *destination;
  QComboBox *regions = nullptr;
  // UX-3, decision 5: the provider picked among the engine's
  // (get_render_authoring_context's region_providers), shown in place of
  // the line while the engine lists any.
  QComboBox *providers = nullptr;
  QLabel *provider_caption = nullptr;
  QPushButton *apply;
  QPushButton *refresh;
  QTimer poll;
  QString root_digest, preset_digest, ticket, committed_reference;
  qint64 request_id = 0;
  enum class Request { Context, Fork, Result } request_kind = Request::Context;
  void controls() {
    const bool idle = request_id == 0 && ticket.isEmpty();
    destination->setEnabled(idle);
    if (regions) regions->setEnabled(idle);
    if (providers) providers->setEnabled(idle);
    apply->setEnabled(idle && viewport.rpcReady() && !root_digest.isEmpty() &&
                      !preset_digest.isEmpty() && (!regions || regions->currentIndex() >= 0));
    refresh->setEnabled(viewport.rpcReady() && request_id == 0);
  }
  void request(Request kind, const QString &method,
               const QJsonObject &params = {}) {
    if (request_id)
      return;
    QString error;
    request_kind = kind;
    request_id = viewport.requestRpc(method, params, &error);
    if (!request_id) {
      poll.stop();
      notice->setText(error);
    }
    controls();
  }
  void result() {
    if (!ticket.isEmpty())
      request(Request::Result, QStringLiteral("get_edit_result"),
              {{"ticket", ticket}});
  }
  void read() {
    if (!ticket.isEmpty())
      result();
    else
      request(Request::Context, QStringLiteral("get_render_authoring_context"));
  }
  // UX-3, decision 5: the engine's providers as a combo; an engine that
  // lists none (before EL-1, or none running) keeps the line to type in.
  void fillProviders(const QJsonArray &listed) {
    if (!providers) return;
    providers->clear();
    providers->addItem(tr("(authored nodes)"), QString{});
    for (const auto &value : listed) {
      const auto entry = value.toObject();
      const auto name = entry["name"].toString();
      if (name.isEmpty()) continue;
      providers->addItem(entry["builtin"].toBool() ? tr("%1 (built in)").arg(name) : name, name);
    }
    const bool listed_any = providers->count() > 1;
    providers->setVisible(listed_any);
    destination->setVisible(!listed_any);
    if (provider_caption)
      provider_caption->setText(listed_any ? tr("Replacement provider")
                                           : tr("Replacement provider (empty = authored nodes)"));
  }
  void selectProvider(const QString &provider) {
    if (!providers) return;
    int index = providers->findData(provider);
    // A provider the region names that the engine does not list stays.
    if (index < 0 && !provider.isEmpty()) {
      providers->addItem(provider, provider);
      index = providers->count() - 1;
    }
    providers->setCurrentIndex(std::max(0, index));
  }
  void receive(qint64 id, const QByteArray &bytes) {
    if (!id || id != request_id)
      return;
    request_id = 0;
    QString error;
    const auto response = decodeObject(bytes, &error);
    if (!response) {
      poll.stop();
      notice->setText(error);
      controls();
      return;
    }
    if (request_kind == Request::Context) {
      root_digest.clear();
      preset_digest.clear();
      if (response->value("config_kind").toString() != "preset") {
        notice->setText(tr("This project uses a direct rendering config. There "
                           "is no preset to fork."));
      } else {
        const auto preset = response->value("pipeline_preset").toObject();
        root_digest =
            response->value("source_digest").toObject()["hex"].toString();
        preset_digest = preset["source_digest"].toObject()["hex"].toString();
        notice->setText(
            tr("Source preset: %1\nCopy this preset into the project and "
               "switch its reference. Existing project settings are retained.")
                .arg(preset["reference"].toString()));
        if (root_digest.isEmpty() || preset_digest.isEmpty())
          notice->setText(tr("The engine did not provide the source digests "
                             "needed to fork this preset."));
        if (mode == Mode::Region) {
          regions->clear();
          destination->clear();
          fillProviders(response->value("region_providers").toArray());
          if (!preset["reference"].toString().startsWith(QStringLiteral("project://"))) {
            preset_digest.clear();
            notice->setText(tr("Fork the preset into the project before editing region replacements."));
          } else {
            for (const auto &graph_value : response->value("graphs").toArray()) {
              const auto graph = graph_value.toObject();
              QStringList seen;
              for (const auto &pass_value : graph["passes"].toArray()) {
                const auto declaration = pass_value.toObject()["preset_declaration"].toObject();
                for (const auto &region_value : declaration["regions"].toArray()) {
                  const auto region = region_value.toString();
                  if (seen.contains(region)) continue;
                  seen.push_back(region);
                  QString provider;
                  for (const auto &replacement : graph["region_replacements"].toArray()) {
                    const auto entry = replacement.toObject();
                    if (entry["region"].toString() == region) provider = entry["provider"].toString();
                  }
                  regions->addItem(graph["name"].toString() + QStringLiteral(" / ") + region,
                    QJsonObject{{"graph", graph["name"]}, {"region", region}, {"provider", provider}});
                }
              }
            }
            notice->setText(regions->count() ? tr("Choose a tagged region. Leave the provider empty to use its authored nodes. Other compiler optimizations remain enabled.")
                                            : tr("This preset has no editable tagged regions."));
          }
        }
      }
    } else if (request_kind == Request::Fork) {
      if (response->value("status").toString() == "accepted") {
        ticket = response->value("ticket").toString();
        committed_reference = response->value("preset_reference").toString();
        if (!ticket.isEmpty()) {
          poll.start();
          result();
        } else
          notice->setText(tr("The engine returned no edit ticket."));
      } else
        notice->setText(responseError(*response));
    } else if (response->value("status").toString() != "pending") {
      poll.stop();
      ticket.clear();
      if (const auto completion = completeEdit(*response, restart);
          completion.restart_required) {
        // RSX: written; the engine that comes back runs it.
        root_digest.clear();
        preset_digest.clear();
        notice->setText(QString::fromStdString(completion.restart_line));
      } else if (completion.committed) {
        root_digest.clear();
        preset_digest.clear();
        notice->setText(mode == Mode::Fork ? tr("Preset copied and selected: %1").arg(committed_reference)
                                          : tr("Region replacement saved. Refresh to edit another region."));
        on_committed();
      } else
        notice->setText(responseError(*response));
    }
    controls();
  }

public:
  PresetEditDialog(EmbeddedViewport &embedded, QWidget *parent,
                   std::function<void()> committed, Mode operation = Mode::Fork)
      : QDialog(parent), mode(operation), viewport(embedded),
        on_committed(std::move(committed)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(mode == Mode::Fork ? QStringLiteral("pelican.presetFork") : QStringLiteral("pelican.regionReplacement"));
    setWindowTitle(mode == Mode::Fork ? tr("Fork preset into project") : tr("Region replacements"));
    resize(560, 210);
    auto *layout = new QVBoxLayout(this);
    notice = new QLabel(tr("Loading preset..."), this);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    if (mode == Mode::Region) {
      regions = new QComboBox(this);
      regions->setObjectName(QStringLiteral("pelican.regionReplacement.region"));
      layout->addWidget(regions);
    }
    provider_caption = new QLabel(mode == Mode::Fork ? tr("New project preset reference") : tr("Replacement provider (empty = authored nodes)"), this);
    layout->addWidget(provider_caption);
    destination =
        new QLineEdit(mode == Mode::Fork ? QStringLiteral("project://pipelines/custom.json") : QString{}, this);
    destination->setObjectName(
        mode == Mode::Fork ? QStringLiteral("pelican.presetFork.destination") : QStringLiteral("pelican.regionReplacement.provider"));
    layout->addWidget(destination);
    if (mode == Mode::Region) {
      providers = new QComboBox(this);
      providers->setObjectName(QStringLiteral("pelican.regionReplacement.providerChoice"));
      providers->setToolTip(tr("The provider the engine resolves the region with, or the "
                               "region's authored nodes"));
      providers->hide();
      layout->addWidget(providers);
    }
    if (regions) connect(regions, &QComboBox::currentIndexChanged, this, [this] {
      const auto provider = regions->currentData().toJsonObject()["provider"].toString();
      destination->setText(provider);
      selectProvider(provider);
      controls();
    });
    auto *buttons = new QHBoxLayout;
    refresh = new QPushButton(tr("Refresh source"), this);
    apply = new QPushButton(mode == Mode::Fork ? tr("Fork preset") : tr("Apply"), this);
    apply->setObjectName(mode == Mode::Fork ? QStringLiteral("pelican.presetFork.apply") : QStringLiteral("pelican.regionReplacement.apply"));
    buttons->addWidget(refresh);
    buttons->addWidget(apply);
    layout->addLayout(buttons);
    connect(refresh, &QPushButton::clicked, this, [this] { read(); });
    connect(apply, &QPushButton::clicked, this, [this] {
      if (mode == Mode::Region) {
        const auto region = regions->currentData().toJsonObject();
        const auto provider = providers && providers->isVisibleTo(this)
                                  ? providers->currentData().toString()
                                  : destination->text().trimmed();
        request(Request::Fork, QStringLiteral("edit_region_replacement"),
                {{"base_source_digest", root_digest}, {"base_preset_digest", preset_digest},
                 {"graph", region["graph"]}, {"region", region["region"]},
                 {"provider", provider.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(provider)}});
        return;
      }
      request(Request::Fork, QStringLiteral("fork_render_pipeline"),
              {{"base_source_digest", root_digest},
               {"base_preset_digest", preset_digest},
               {"destination_reference", destination->text()}});
    });
    poll.setInterval(100);
    connect(&poll, &QTimer::timeout, this, [this] { result(); });
    connect(&viewport, &EmbeddedViewport::inspectorRpcSucceeded, this,
            [this](qint64 id, const QByteArray &bytes) { receive(id, bytes); });
    connect(&viewport, &EmbeddedViewport::inspectorRpcFailed, this,
            [this](qint64 id, const QString &error) {
              if (!id || id != request_id)
                return;
              request_id = 0;
              poll.stop();
              notice->setText(error);
              controls();
            });
    connect(
        &viewport, &EmbeddedViewport::engineRpcBecameUnavailable, this, [this] {
          request_id = 0;
          poll.stop();
          ticket.clear();
          root_digest.clear();
          preset_digest.clear();
          notice->setText(tr(
              "Engine disconnected. Refresh the source after reconnecting."));
          controls();
        });
    connect(&viewport, &EmbeddedViewport::engineRpcBecameAvailable, this,
            [this] { controls(); });
    read();
  }
};

class CallableRecordsDialog final : public QDialog, public RestartsEngine {
  EmbeddedViewport &viewport;
  QComboBox *task_box;
  QComboBox *record_box;
  QTableWidget *fields;
  QLabel *notice;
  QPushButton *apply;
  QPushButton *refresh;
  QPushButton *reset;
  QJsonArray tasks;
  QTimer poll;
  QString digest, ticket;
  qint64 request_id = 0;
  enum class Request { Read, Submit, Result } request_kind = Request::Read;
  void controls() {
    const bool idle = request_id == 0 && ticket.isEmpty();
    for (QWidget *widget :
         std::array<QWidget *, 4>{task_box, record_box, fields, reset})
      widget->setEnabled(idle);
    apply->setEnabled(idle && viewport.rpcReady() && !digest.isEmpty());
    refresh->setEnabled(viewport.rpcReady() && request_id == 0);
  }
  void request(Request kind, const QString &method, QJsonObject params = {}) {
    if (request_id)
      return;
    QString error;
    request_kind = kind;
    request_id = viewport.requestRpc(method, params, &error);
    if (!request_id) {
      poll.stop();
      notice->setText(error);
    }
    controls();
  }
  void read() {
    if (!ticket.isEmpty())
      result();
    else
      request(Request::Read, QStringLiteral("get_ray_callable_records"));
  }
  void result() {
    if (!ticket.isEmpty())
      request(Request::Result, QStringLiteral("get_edit_result"),
              {{"ticket", ticket}});
  }
  void showRecord() {
    fields->setRowCount(0);
    const int ti = task_box->currentIndex(), ri = record_box->currentIndex();
    if (ti < 0 || ri < 0)
      return;
    const auto task = tasks[ti].toObject();
    const auto record = task["records"].toArray()[ri].toObject();
    for (const auto &entry : task["schema"].toArray()) {
      const auto member = entry.toObject();
      const auto name = member["name"].toString(),
                 type = member["type"].toString();
      const auto value = record[name];
      const auto values = value.isArray() ? value.toArray() : QJsonArray{value};
      for (int c = 0; c < values.size(); ++c) {
        const int row = fields->rowCount();
        fields->insertRow(row);
        auto *label = new QTableWidgetItem(
            value.isArray()
                ? QStringLiteral("%1[%2] (%3)").arg(name).arg(c).arg(type)
                : name + " (" + type + ")");
        label->setFlags(label->flags() & ~Qt::ItemIsEditable);
        fields->setItem(row, 0, label);
        auto *input = new QLineEdit(
            values[c].isString()
                ? values[c].toString()
                : QString::number(values[c].toDouble(), 'g',
                                  type == "u32" || type == "i32" ? 17 : 9),
            fields);
        input->setObjectName(QStringLiteral("pelican.callableRecords.value"));
        fields->setCellWidget(row, 1, input);
        connect(input, &QLineEdit::textEdited, this,
                [this, ti, ri, name, c,
                 vector = value.isArray()](const QString &text) {
                  auto task = tasks[ti].toObject();
                  auto records = task["records"].toArray();
                  auto record = records[ri].toObject();
                  if (vector) {
                    auto values = record[name].toArray();
                    values[c] = text;
                    record[name] = values;
                  } else
                    record[name] = text;
                  records[ri] = record;
                  task["records"] = records;
                  tasks[ti] = task;
                });
      }
    }
  }
  void showTask() {
    const QSignalBlocker blocker(record_box);
    record_box->clear();
    if (task_box->currentIndex() >= 0) {
      const auto callables =
          tasks[task_box->currentIndex()].toObject()["callables"].toArray();
      for (int i = 0; i < callables.size(); ++i)
        record_box->addItem(
            QStringLiteral("%1: %2").arg(i).arg(callables[i].toString()));
    }
    showRecord();
  }
  void submit() {
    QJsonObject mapping;
    for (const auto &entry : tasks) {
      const auto task = entry.toObject();
      auto records = task["records"].toArray();
      for (int i = 0; i < records.size(); ++i) {
        auto record = records[i].toObject();
        for (const auto &entry : task["schema"].toArray()) {
          const auto member = entry.toObject();
          const auto name = member["name"].toString(),
                     type = member["type"].toString();
          const auto value = record[name];
          auto values = value.isArray() ? value.toArray() : QJsonArray{value};
          for (int c = 0; c < values.size(); ++c) {
            bool ok = true;
            const double number = values[c].isString()
                                      ? values[c].toString().toDouble(&ok)
                                      : values[c].toDouble();
            const bool integer = type == "i32" || type == "u32";
            const double minimum =
                type == "u32"   ? 0.0
                : type == "i32" ? -2147483648.0
                                : -double(std::numeric_limits<float>::max());
            const double maximum =
                type == "u32"   ? 4294967295.0
                : type == "i32" ? 2147483647.0
                                : double(std::numeric_limits<float>::max());
            if (!ok || !std::isfinite(number) || number < minimum ||
                number > maximum || (integer && std::floor(number) != number)) {
              notice->setText(
                  tr("Invalid %1 value in task %2, record %3, field %4.")
                      .arg(type, task["name"].toString())
                      .arg(i)
                      .arg(name));
              return;
            }
            values[c] = number;
          }
          record[name] =
              value.isArray() ? QJsonValue(values) : QJsonValue(values[0]);
        }
        records[i] = record;
      }
      if (records != task["authored_records"].toArray())
        mapping.insert(task["name"].toString(), records);
    }
    request(Request::Submit, QStringLiteral("edit_ray_callable_records"),
            {{"base_source_digest", digest}, {"task_records", mapping}});
  }
  void receive(qint64 id, const QByteArray &bytes) {
    if (!id || id != request_id)
      return;
    request_id = 0;
    QString error;
    const auto response = decodeObject(bytes, &error);
    if (!response) {
      poll.stop();
      notice->setText(error);
      controls();
      return;
    }
    if (request_kind == Request::Read) {
      const auto next_digest =
          response->value("source_digest").toObject()["hex"].toString();
      if (next_digest.isEmpty() || !response->value("tasks").isArray())
        notice->setText(tr("Invalid callable record document."));
      else {
        digest = next_digest;
        tasks = response->value("tasks").toArray();
        {
          const QSignalBlocker blocker(task_box);
          task_box->clear();
          for (const auto &task : tasks)
            task_box->addItem(task.toObject()["name"].toString());
        }
        showTask();
        notice->setText(tasks.isEmpty()
                            ? tr("No tasks declare callable records.")
                            : response->value("source_reference").toString());
      }
    } else if (request_kind == Request::Submit) {
      if (response->value("status").toString() == "accepted") {
        ticket = response->value("ticket").toString();
        if (!ticket.isEmpty()) {
          poll.start();
          result();
        } else
          notice->setText(tr("The engine returned no edit ticket."));
      } else
        notice->setText(responseError(*response));
    } else if (response->value("status").toString() != "pending") {
      poll.stop();
      ticket.clear();
      if (const auto completion = completeEdit(*response, restart);
          completion.restart_required)
        notice->setText(QString::fromStdString(completion.restart_line));
      else if (completion.committed)
        read();
      else
        notice->setText(responseError(*response));
    }
    controls();
  }

public:
  CallableRecordsDialog(EmbeddedViewport &embedded, QWidget *parent)
      : QDialog(parent), viewport(embedded) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("pelican.callableRecords"));
    setWindowTitle(tr("Callable record initial values"));
    resize(600, 440);
    auto *layout = new QVBoxLayout(this);
    const QString records_help =
        tr("Edit saved initial values. Live changes "
           "made by game logic are not shown here.");
    task_box = new QComboBox(this);
    record_box = new QComboBox(this);
    task_box->setObjectName(QStringLiteral("pelican.callableRecords.task"));
    record_box->setObjectName(QStringLiteral("pelican.callableRecords.record"));
    layout->addWidget(task_box);
    layout->addWidget(record_box);
    fields = new QTableWidget(0, 2, this);
    fields->setHorizontalHeaderLabels({tr("Field / type"), tr("Value")});
    fields->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    fields->setToolTip(records_help);
    layout->addWidget(fields);
    notice = new QLabel(this);
    PelicanStudio::applyPanelNoticeStyle(notice);
    layout->addWidget(notice);
    auto *buttons = new QHBoxLayout;
    reset = new QPushButton(tr("Reset task"), this);
    refresh = new QPushButton(tr("Refresh"), this);
    apply = new QPushButton(tr("Apply"), this);
    reset->setObjectName(QStringLiteral("pelican.callableRecords.reset"));
    refresh->setObjectName(QStringLiteral("pelican.callableRecords.refresh"));
    apply->setObjectName(QStringLiteral("pelican.callableRecords.apply"));
    for (auto *button : {reset, refresh, apply})
      buttons->addWidget(button);
    layout->addLayout(buttons);
    refresh->setToolTip(
        tr("Replace local edits with the saved initial values."));
    connect(task_box, &QComboBox::currentIndexChanged, this,
            [this] { showTask(); });
    connect(record_box, &QComboBox::currentIndexChanged, this,
            [this] { showRecord(); });
    connect(reset, &QPushButton::clicked, this, [this] {
      const int i = task_box->currentIndex();
      if (i < 0)
        return;
      auto task = tasks[i].toObject();
      task["records"] = task["authored_records"];
      tasks[i] = task;
      showRecord();
    });
    connect(refresh, &QPushButton::clicked, this, [this] { read(); });
    connect(apply, &QPushButton::clicked, this, [this] { submit(); });
    poll.setInterval(100);
    connect(&poll, &QTimer::timeout, this, [this] { result(); });
    connect(&viewport, &EmbeddedViewport::inspectorRpcSucceeded, this,
            [this](qint64 id, const QByteArray &bytes) { receive(id, bytes); });
    connect(&viewport, &EmbeddedViewport::inspectorRpcFailed, this,
            [this](qint64 id, const QString &error) {
              if (!id || id != request_id)
                return;
              request_id = 0;
              poll.stop();
              notice->setText(error);
              controls();
            });
    connect(
        &viewport, &EmbeddedViewport::engineRpcBecameUnavailable, this, [this] {
          request_id = 0;
          ticket.clear();
          digest.clear();
          poll.stop();
          notice->setText(tr("Engine disconnected. Local edits are retained."));
          controls();
        });
    connect(&viewport, &EmbeddedViewport::engineRpcBecameAvailable, this,
            [this] { controls(); });
    read();
  }
};

class SceneAccelerationStructuresDialog final : public QDialog, public RestartsEngine {
    EmbeddedViewport &viewport;
    QTableWidget *table;
    QTableWidget *connections;
    QLabel *notice;
    QComboBox *triangle_space;
    QComboBox *procedural_space;
    QLabel *procedural_source;
    QString original_procedural_space;
    bool procedural_editable = false;
    QLabel *triangle_source;
    QString layout_digest;
    QString original_space;
    bool geometry_editable = false;
    QPushButton *add;
    QPushButton *remove;
    QPushButton *apply;
    QPushButton *refresh;
    QTimer poll;
    QString digest;
    QString ticket;
    qint64 request_id = 0;
    enum class Request { Read, Submit, Result } request_kind = Request::Read;

    void controls() {
        const bool busy = request_id != 0 || !ticket.isEmpty();
        table->setEnabled(!busy);
        triangle_space->setEnabled(!busy && geometry_editable && !digest.isEmpty());
        procedural_space->setEnabled(!busy && procedural_editable && !digest.isEmpty());
        connections->setEnabled(!busy);
        add->setEnabled(!busy);
        remove->setEnabled(!busy);
        refresh->setEnabled(viewport.rpcReady() && request_id == 0);
        apply->setEnabled(viewport.rpcReady() && !busy && !digest.isEmpty());
    }
    void connectionOptions() {
        QStringList options{QStringLiteral("scene")};
        for (int i = 0; i < table->rowCount(); ++i)
            if (table->item(i, 0) && !table->item(i, 0)->text().isEmpty())
                options.append(table->item(i, 0)->text());
        options.removeDuplicates();
        for (int i = 0; i < connections->rowCount(); ++i) {
            auto *combo = static_cast<QComboBox *>(connections->cellWidget(i, 1));
            const auto selected = combo->currentText();
            combo->clear();
            combo->addItems(options);
            if (!options.contains(selected) && !selected.isEmpty()) combo->addItem(selected);
            combo->setCurrentText(selected);
        }
    }
    void row(const QString &name, int mask) {
        const int index = table->rowCount();
        table->insertRow(index);
        table->setItem(index, 0, new QTableWidgetItem(name));
        auto *value = new QSpinBox(table);
        value->setRange(0, 255);
        value->setValue(mask);
        table->setCellWidget(index, 1, value);
    }
    void request(Request kind, const QString &method, const QJsonObject &params = {}) {
        if (request_id != 0) return;
        QString error;
        request_id = viewport.requestRpc(method, params, &error);
        request_kind = kind;
        if (!request_id) {
            poll.stop();
            notice->setText(error);
        }
        controls();
    }
    void read() {
        if (!ticket.isEmpty()) {
            result();
            return;
        }
        request(Request::Read, QStringLiteral("get_scene_acceleration_structures"));
    }
    void result() {
        if (!ticket.isEmpty())
            request(Request::Result, QStringLiteral("get_edit_result"),
                    {{QStringLiteral("ticket"), ticket}});
    }
    void submit() {
        QJsonArray definitions;
        QSet<QString> names;
        for (int index = 0; index < table->rowCount(); ++index) {
            const auto name = table->item(index, 0)->text();
            if (name.isEmpty() || name == QStringLiteral("scene") || names.contains(name)) {
                notice->setText(tr("Use unique names. The name scene is reserved."));
                return;
            }
            names.insert(name);
            definitions.append(QJsonObject{{QStringLiteral("name"), name},
                {QStringLiteral("instance_mask"),
                 static_cast<QSpinBox *>(table->cellWidget(index, 1))->value()}});
        }
        QJsonObject task_connections;
        for (int i = 0; i < connections->rowCount(); ++i) {
            const auto selected = static_cast<QComboBox *>(connections->cellWidget(i, 1))->currentText();
            if (selected != connections->item(i, 0)->data(Qt::UserRole).toString())
                task_connections.insert(connections->item(i, 0)->text(), selected);
        }
        QJsonObject params{{QStringLiteral("base_source_digest"), digest},
             {QStringLiteral("definitions"), definitions},
             {QStringLiteral("task_connections"), task_connections}};
        if (geometry_editable && triangle_space->currentData().toString() != original_space) {
            params.insert(QStringLiteral("triangle_space"), triangle_space->currentData().toString());
            params.insert(QStringLiteral("base_layout_digest"), layout_digest);
        }
        if (procedural_editable && procedural_space->currentData().toString() != original_procedural_space) {
            params.insert(QStringLiteral("procedural_space"), procedural_space->currentData().toString());
            params.insert(QStringLiteral("base_layout_digest"), layout_digest);
        }
        request(Request::Submit, QStringLiteral("edit_scene_acceleration_structures"), params);
    }
    void receive(qint64 id, const QByteArray &bytes) {
        if (id != request_id || !request_id) return;
        request_id = 0;
        QString error;
        const auto response = decodeObject(bytes, &error);
        if (!response) {
            poll.stop();
            notice->setText(error);
            controls();
            return;
        }
        if (request_kind == Request::Read) {
            const auto next_digest = response->value(QStringLiteral("source_digest"))
                .toObject().value(QStringLiteral("hex")).toString();
            const auto definitions = response->value(QStringLiteral("definitions"));
            if (next_digest.isEmpty() || !definitions.isArray()) {
                notice->setText(tr("The engine did not return an editable scene AS document."));
            } else {
                digest = next_digest;
                const auto geometry = response->value(QStringLiteral("triangle_geometry")).toObject();
                original_space = geometry.value(QStringLiteral("space")).toString();
                layout_digest = geometry.value(QStringLiteral("source_digest")).toObject().value(QStringLiteral("hex")).toString();
                geometry_editable = geometry.value(QStringLiteral("editable")).toBool();
                triangle_space->setCurrentIndex(triangle_space->findData(original_space));
                triangle_source->setText(geometry_editable
                    ? geometry.value(QStringLiteral("source_reference")).toString()
                    : geometry.value(QStringLiteral("reason")).toString());
                const auto procedural = response->value(QStringLiteral("procedural_geometry")).toObject();
                original_procedural_space = procedural.value(QStringLiteral("space")).toString();
                procedural_editable = procedural.value(QStringLiteral("editable")).toBool();
                procedural_space->setCurrentIndex(procedural_space->findData(original_procedural_space));
                procedural_source->setText(procedural_editable
                    ? procedural.value(QStringLiteral("source_reference")).toString()
                    : procedural.value(QStringLiteral("reason")).toString());
                table->setRowCount(0);
                for (const auto &entry : definitions.toArray()) {
                    const auto definition = entry.toObject();
                    row(definition.value(QStringLiteral("name")).toString(),
                        definition.value(QStringLiteral("instance_mask")).toInt(255));
                }
                connections->setRowCount(0);
                for (const auto &entry : response->value(QStringLiteral("task_connections")).toArray()) {
                    const auto task = entry.toObject();
                    const auto index = connections->rowCount();
                    connections->insertRow(index);
                    auto *name = new QTableWidgetItem(task.value(QStringLiteral("name")).toString());
                    name->setFlags(name->flags() & ~Qt::ItemIsEditable);
                    name->setData(Qt::UserRole, task.value(QStringLiteral("authored_acceleration_structure")).toString());
                    name->setToolTip(tr("Authored AS: %1").arg(task.value(QStringLiteral("authored_acceleration_structure")).toString()));
                    connections->setItem(index, 0, name);
                    auto *combo = new QComboBox(connections);
                    combo->addItem(task.value(QStringLiteral("acceleration_structure")).toString());
                    connections->setCellWidget(index, 1, combo);
                }
                connectionOptions();
                notice->setText(response->value(QStringLiteral("source_reference")).toString());
            }
        } else if (request_kind == Request::Submit) {
            if (response->value(QStringLiteral("status")).toString() == QStringLiteral("accepted")) {
                ticket = response->value(QStringLiteral("ticket")).toString();
                if (!ticket.isEmpty()) {
                    notice->setText(tr("Applying scene acceleration structures..."));
                    poll.start();
                    result();
                } else notice->setText(tr("The engine returned no edit ticket."));
            } else notice->setText(responseError(*response));
        } else {
            const auto status = response->value(QStringLiteral("status")).toString();
            if (status != QStringLiteral("pending")) {
                poll.stop();
                ticket.clear();
                if (const auto completion = completeEdit(*response, restart);
                    completion.restart_required)
                    notice->setText(QString::fromStdString(completion.restart_line));
                else if (completion.committed) read();
                else notice->setText(responseError(*response));
            }
        }
        controls();
    }
  public:
    SceneAccelerationStructuresDialog(EmbeddedViewport &embedded, QWidget *parent)
        : QDialog(parent), viewport(embedded) {
        setAttribute(Qt::WA_DeleteOnClose);
        setObjectName(QStringLiteral("pelican.sceneAccelerationStructures"));
        setWindowTitle(tr("Scene acceleration structures"));
        resize(560, 380);
        auto *layout = new QVBoxLayout(this);
        const QString subsets_help = tr("Create named scene subsets using instance mask bits. The default scene includes all geometry.");
        triangle_space = new QComboBox(this);
        triangle_space->setObjectName(QStringLiteral("pelican.sceneAccelerationStructures.triangleSpace"));
        triangle_space->addItem(tr("Geometry space (per-instance transforms)"), QStringLiteral("geometry"));
        triangle_space->addItem(tr("World space (combine transformed triangles)"), QStringLiteral("world"));
        triangle_space->setToolTip(tr("World space bakes object transforms into triangles so compatible instances can share a BLAS. Triangle object-space shader coordinates become world-space coordinates. Procedural AABBs are unchanged."));
        auto *geometry_form = new QFormLayout;
        geometry_form->addRow(tr("Triangles"), triangle_space);
        layout->addLayout(geometry_form);
        triangle_source = new QLabel(this);
        triangle_source->setObjectName(QStringLiteral("pelican.sceneAccelerationStructures.triangleSource"));
        triangle_source->setWordWrap(true);
        layout->addWidget(triangle_source);
        procedural_space = new QComboBox(this);
        procedural_space->setObjectName(QStringLiteral("pelican.sceneAccelerationStructures.proceduralSpace"));
        procedural_space->addItem(tr("Geometry space (per-instance transforms)"), QStringLiteral("geometry"));
        procedural_space->addItem(tr("World space (combine transformed AABBs)"), QStringLiteral("world"));
        procedural_space->setToolTip(tr("World space combines compatible AABB instances. Shaders must use the procedural transform helpers to preserve local intersections. The layout must already declare procedural_transform; changing space preserves the shader record layout."));
        auto *procedural_form = new QFormLayout;
        procedural_form->addRow(tr("AABBs"), procedural_space);
        layout->addLayout(procedural_form);
        procedural_source = new QLabel(this);
        procedural_source->setObjectName(QStringLiteral("pelican.sceneAccelerationStructures.proceduralSource"));
        procedural_source->setWordWrap(true);
        layout->addWidget(procedural_source);
        table = new QTableWidget(0, 2, this);
        table->setObjectName(QStringLiteral("pelican.sceneAccelerationStructures.table"));
        table->setHorizontalHeaderLabels({tr("Name"), tr("Instance mask (0-255)")});
        table->setToolTip(subsets_help);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        layout->addWidget(table);
        connections = new QTableWidget(0, 2, this);
        connections->setObjectName(QStringLiteral("pelican.sceneAccelerationStructures.connections"));
        connections->setHorizontalHeaderLabels({tr("Ray task"), tr("Scene AS")});
        connections->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        layout->addWidget(connections);
        connect(table, &QTableWidget::itemChanged, this, [this] { connectionOptions(); });
        notice = new QLabel(this);
        notice->setWordWrap(true);
        layout->addWidget(notice);
        auto *buttons = new QHBoxLayout;
        add = new QPushButton(tr("Add"), this);
        remove = new QPushButton(tr("Remove"), this);
        refresh = new QPushButton(tr("Refresh"), this);
        apply = new QPushButton(tr("Apply"), this);
        add->setObjectName(QStringLiteral("pelican.sceneAccelerationStructures.add"));
        remove->setObjectName(QStringLiteral("pelican.sceneAccelerationStructures.remove"));
        refresh->setObjectName(QStringLiteral("pelican.sceneAccelerationStructures.refresh"));
        apply->setObjectName(QStringLiteral("pelican.sceneAccelerationStructures.apply"));
        refresh->setToolTip(tr("Reload from the engine and replace local edits."));
        for (auto *button : {add, remove, refresh, apply}) buttons->addWidget(button);
        layout->addLayout(buttons);
        poll.setInterval(100);
        connect(add, &QPushButton::clicked, this, [this] { row({}, 255); });
        connect(remove, &QPushButton::clicked, this, [this] {
            if (table->currentRow() >= 0) table->removeRow(table->currentRow());
            connectionOptions();
        });
        connect(refresh, &QPushButton::clicked, this, [this] { read(); });
        connect(apply, &QPushButton::clicked, this, [this] { submit(); });
        connect(&poll, &QTimer::timeout, this, [this] { result(); });
        connect(&viewport, &EmbeddedViewport::inspectorRpcSucceeded, this,
            [this](qint64 id, const QByteArray &bytes) { receive(id, bytes); });
        connect(&viewport, &EmbeddedViewport::inspectorRpcFailed, this,
            [this](qint64 id, const QString &message) {
                if (id != request_id || !id) return;
                request_id = 0;
                poll.stop();
                notice->setText(message);
                controls();
            });
        connect(&viewport, &EmbeddedViewport::engineRpcBecameUnavailable, this,
            [this](const QString &message) {
                request_id = 0;
                ticket.clear();
                digest.clear();
                poll.stop();
                notice->setText(message);
                controls();
            });
        connect(&viewport, &EmbeddedViewport::engineRpcBecameAvailable, this, [this] { controls(); });
        controls();
        if (viewport.rpcReady()) read();
        else notice->setText(tr("Connect the engine, then Refresh to load scene acceleration structures."));
    }
};

} // namespace

struct RenderFeaturesWidget::Impl {
    enum class RequestKind {
        CanEdit,
        Current,
        Catalog,
        Submit,
        Result,
        // LP-4f2: get_graph_templates, which features have a loop version.
        Templates,
        // UX-10, decision 1: what the open parameters' target and shader
        // rows pick from.
        Context,
        Shaders,
        // UX-12b: the reads and the edits of a feature added with its
        // targets, and of a target_ref's type fix.
        ChainContext,
        ChainGraph,
        ChainSubmit,
    };

    struct PendingRequest {
        RequestKind kind = RequestKind::Current;
        quint64 refresh_generation = 0;
    };

    struct StagedOperation {
        QString op;
        QString reference;
        QString display_name;
        int index = -1;
        QJsonObject parameters;
        QString base_digest;
        QString scope;
        QString preset_digest;
        QString feature_digest;
        QString destination;
        // LP-4f2 (op "apply_graph_template"): `reference` is the template,
        // this the feature it replaces.
        QString feature;
    };

    RenderFeaturesWidget &owner;
    EmbeddedViewport &viewport;
    // RU-1: the render history's Undo / Redo, handed to each editor opened.
    QList<QAction *> history_actions;
    void adoptHistoryActions(QWidget *editor) const { editor->addActions(history_actions); }
    // Q-10: get_render_features' source_reference, the rendering document
    // itself. The Task Editor refuses a task declared there by name.
    QString root_reference;
    // Q-12: handed to each Task Editor it opens.
    RenderFeaturesWidget::GraphNodeOpenAction graph_node_open_action;
    // LP-4b: handed to each Task Editor it opens.
    RenderFeaturesWidget::TaskViewAxesProvider task_view_axes;
    QListWidget *current = nullptr;
    QTreeWidget *catalog = nullptr;
    QPushButton *add = nullptr;
    QPushButton *remove = nullptr;
    QPushButton *parameters = nullptr;
    QPushButton *materialize = nullptr;
    QPushButton *apply = nullptr;
    QPushButton *refresh = nullptr;
    QLabel *notice = nullptr;
    QTimer *result_poll = nullptr;
    // --- LP-4f2: Replace with loop version.
    QPushButton *replace = nullptr;
    QAction *replace_action = nullptr;
    std::vector<GraphTemplateInfo> templates;
    bool have_templates = false;
    // The loop the accepted apply_graph_template made, handed on at commit.
    std::string applied_loop;
    std::function<void(const QString &loop)> loop_applied_action;
    QPointer<QMessageBox> replace_prompt;
    // The line a replacement left stays while the list is read again.
    bool notice_sticky = false;
    QHash<qint64, PendingRequest> pending;
    quint64 refresh_generation = 0;
    // --- UX-10, decision 1: the open parameters dialog, and the change it
    // gathered that is not sent yet.
    struct LiveParameters {
        QPointer<QDialog> dialog;
        FeatureParameterForm *form = nullptr;
        QString reference;
        int index = -1;
        QString scope;
        QJsonObject parameters;
        bool waiting = false;
    };
    LiveParameters live;
    QTimer *live_timer = nullptr;
    QStringList render_target_names;
    std::vector<std::pair<QString, QString>> engine_shaders;
    QString source_digest;
    QString active_ticket;
    std::optional<StagedOperation> staged;
    bool can_edit = false;
    bool have_current = false;
    bool have_catalog = false;
    // --- Q-13: the digest every panel that edits the document shares.
    std::shared_ptr<RenderConfigDigest> digest_hub;
    // Another panel moved the document while an edit here was in flight.
    bool refresh_when_idle = false;
    // The edit in flight, kept until it commits: a refusal for a moved
    // document refreshes and sends it once more against the new revision.
    std::optional<StagedOperation> submitted_staged;
    bool submitted_retried = false;
    bool retrying = false;
    bool retry_pending = false;
    // --- RSX, decision 1: the main window's engine restart, and whether this
    // panel (or an editor it opened) asked for the one on the way.
    EngineRestartAction engine_restart_action;
    bool restart_asked = false;
    // --- UX-12b, decisions 1, 2 and 4: the ways out a refusal (or the
    // catalog, before an add) names, under the notice, and the edits that
    // take them one after another.
    QWidget *offers = nullptr;
    QPushButton *add_with_targets = nullptr;
    QPushButton *place_anchor = nullptr;
    QPushButton *fix_target_ref = nullptr;
    std::function<bool(const QString &anchor)> place_anchor_action;
    // list_render_features' entries by reference: required_targets and
    // (engine FR-2) insertion.
    std::map<std::string, nlohmann::json> catalog_entries;
    QString offered_reference;
    QString offered_name;
    // A target a refusal named, kept beside what the catalog lists.
    std::optional<FeatureRequiredTarget> offered_target;
    QString offered_anchor;
    std::optional<TargetRefTypeFix> offered_fix;
    // The parameter change the target_ref refused, sent again after the fix.
    std::optional<StagedOperation> offered_resend;
    // --- UX-12d (engine FR-3), decisions 1 and 4: a writer for a target no
    // pass draws, and the edits that show a target nothing reads.
    QComboBox *writer_choice = nullptr;
    QPushButton *write_from = nullptr;
    QPushButton *show_target = nullptr;
    // The target a writer is offered for, the outputs that could write it,
    // and (before an add) the targets the question names with it.
    std::optional<FeatureRequiredTarget> offered_writer_target;
    std::vector<WriterCandidate> offered_writers;
    std::vector<FeatureRequiredTarget> offered_missing;
    QString offered_show_reference;
    struct TargetsEdit {
        enum class Step {
            Reading,
            // UX-12d, decision 2: the targets, their references (and a
            // writer) and the feature as one edit_batch; or the fix and the
            // change it was refused for.
            Batch,
            FixReading,
            FixBatch,
            // Decision 4: what is read to say whether a target is shown, the
            // read before the press is sent, then the anchor's colour and
            // present's input (UX-13d: one edit_batch).
            ShowCheck,
            ShowReading,
            ShowPresent,
        };
        Step step = Step::Reading;
        // Only decide whether to ask (Add), or send (Add with targets).
        bool ask_only = false;
        QString reference;
        QString name;
        bool have_context = false;
        bool have_graph = false;
        std::vector<std::string> declared;
        std::vector<std::string> passes;
        nlohmann::json instances = nlohmann::json::object();
        std::optional<FeatureRequiredTarget> extra;
        std::vector<FeatureRequiredTarget> missing;
        nlohmann::json reference_ops = nlohmann::json::array();
        std::optional<TargetRefTypeFix> fix;
        std::optional<StagedOperation> resend;
        // UX-12d: the writer connected with the targets, what each edit of
        // the batch does (said with a refusal's batch_index), the authoring
        // context as read, and the show plan being sent.
        std::optional<FeatureRequiredTarget> writer_target;
        std::optional<WriterCandidate> writer;
        std::vector<QString> batch_steps;
        nlohmann::json context = nlohmann::json::object();
        std::optional<ShowTargetPlan> show;
        QString line;
    };
    std::optional<TargetsEdit> chain;

    // A commit the engine runs only after a restart: the window restarts it
    // (asking about unsaved scene edits first), and the panel says so. The
    // line goes up after the call, so it is what stays while the engine
    // stops.
    void restartEngine(const QString &line) {
        restart_asked = true;
        if (engine_restart_action) engine_restart_action(line);
        showInformation(line);
    }
    EngineRestartAction restartAction() {
        return [this](const QString &line) { restartEngine(line); };
    }

    Impl(RenderFeaturesWidget &widget,
         EmbeddedViewport &embedded_viewport)
        : owner{widget}, viewport{embedded_viewport} {
        owner.setObjectName(
            QStringLiteral("pelican.renderFeatures"));
        owner.setFont(studioPanelFont());
        auto *layout = new QVBoxLayout(&owner);

        // N-7 rule 1: the sentence that said what this panel does is behind
        // the `?`; the panel shows the two lists and what it can do to them.
        auto *heading = new QHBoxLayout;
        auto *current_label = new QLabel(owner.tr("Features"), &owner);
        current_label->setToolTip(owner.tr(
            "Edit the authored render config features. Changes are "
            "preflighted and applied at a frame boundary."));
        heading->addWidget(current_label, 1);
        heading->addWidget(makePanelHelpButton(
            &owner, "pelican.renderFeatures.help", ManualRenderFeaturesAnchor,
            owner.tr("Edit the authored render config features. Changes are "
                     "preflighted and applied at a frame boundary.")));
        layout->addLayout(heading);
        current = new QListWidget(&owner);
        current->setObjectName(
            QStringLiteral("pelican.renderFeatures.current"));
        current->setSelectionMode(
            QAbstractItemView::SingleSelection);
        current->setEditTriggers(
            QAbstractItemView::NoEditTriggers);
        layout->addWidget(current, 1);

        auto *remove_row = new QHBoxLayout;
        remove = new QPushButton(owner.tr("Remove"), &owner);
        remove->setObjectName(
            QStringLiteral("pelican.renderFeatures.remove"));
        remove_row->addWidget(remove);
        parameters = new QPushButton(owner.tr("Parameters"), &owner);
        parameters->setObjectName("pelican.renderFeatures.parameters");
        remove_row->addWidget(parameters);
        QObject::connect(parameters, &QPushButton::clicked, &owner, [this] { editParameters(); });
        materialize = new QPushButton(owner.tr("Make editable"), &owner);
        materialize->setObjectName("pelican.renderFeatures.materialize");
        remove_row->addWidget(materialize);
        QObject::connect(materialize, &QPushButton::clicked, &owner, [this] { stageMaterialize(); });
        // LP-4f2. A feature the engine ships a decomposed graph for offers it
        // on its row: the button under the list and the row's right-click
        // fire the same named action.
        replace_action = new QAction(owner.tr("Replace with loop version\u2026"), &owner);
        replace_action->setObjectName(
            QStringLiteral("pelican.renderFeatures.replaceWithLoopVersion"));
        owner.addAction(replace_action);
        QObject::connect(replace_action, &QAction::triggered, &owner, [this] {
            auto *item = current->currentItem();
            if (item == nullptr) {
                showError(owner.tr("Select the feature to replace first."));
                return;
            }
            replaceWithLoopVersion(item);
        });
        replace = new QPushButton(owner.tr("Replace with loop version\u2026"), &owner);
        replace->setObjectName(QStringLiteral("pelican.renderFeatures.replaceWithLoop"));
        replace->hide();
        remove_row->addWidget(replace);
        QObject::connect(replace, &QPushButton::clicked, replace_action, &QAction::trigger);
        current->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(current, &QWidget::customContextMenuRequested, &owner,
                         [this](const QPoint &point) {
                             auto *item = current->itemAt(point);
                             if (item == nullptr) return;
                             current->setCurrentItem(item);
                             if (templateFor(item) == nullptr) return;
                             updateControls();
                             QMenu menu(&owner);
                             menu.setToolTipsVisible(true);
                             menu.addAction(replace_action);
                             menu.exec(current->viewport()->mapToGlobal(point));
                         });
        remove_row->addStretch();
        layout->addLayout(remove_row);

        auto *catalog_label = new QLabel(owner.tr("Catalog"), &owner);
        catalog_label->setToolTip(
            owner.tr("The features this engine build offers."));
        layout->addWidget(catalog_label);
        catalog = new QTreeWidget(&owner);
        catalog->setObjectName(
            QStringLiteral("pelican.renderFeatures.catalog"));
        catalog->setColumnCount(2);
        catalog->setHeaderLabels(
            {owner.tr("Feature"), owner.tr("Availability")});
        catalog->header()->setStretchLastSection(false);
        catalog->header()->setSectionResizeMode(
            0, QHeaderView::Stretch);
        catalog->header()->setSectionResizeMode(
            1, QHeaderView::ResizeToContents);
        catalog->setSelectionMode(
            QAbstractItemView::SingleSelection);
        catalog->setEditTriggers(
            QAbstractItemView::NoEditTriggers);
        layout->addWidget(catalog, 1);

        auto *add_row = new QHBoxLayout;
        add = new QPushButton(owner.tr("Add"), &owner);
        add->setObjectName(
            QStringLiteral("pelican.renderFeatures.add"));
        add_row->addWidget(add);
        add_row->addStretch();
        layout->addLayout(add_row);

        notice = new QLabel(&owner);
        notice->setObjectName(
            QStringLiteral("pelican.renderFeatures.notice"));
        applyPanelNoticeStyle(notice);
        notice->hide();
        layout->addWidget(notice);
        // UX-12b: the one-press ways out of what the notice says.
        offers = new QWidget(&owner);
        offers->setObjectName(QStringLiteral("pelican.renderFeatures.offers"));
        auto *offer_row = new QHBoxLayout(offers);
        offer_row->setContentsMargins(0, 0, 0, 0);
        add_with_targets = new QPushButton(owner.tr("Add with targets"), offers);
        add_with_targets->setObjectName(QStringLiteral("pelican.renderFeatures.addWithTargets"));
        add_with_targets->setToolTip(owner.tr(
            "Declare the render targets the feature needs, add a render.target_ref of each "
            "to the Graph and add the feature, as one edit: nothing is written if the engine "
            "refuses it."));
        offer_row->addWidget(add_with_targets);
        // UX-12d, decision 1: the outputs that could write a target nothing
        // draws (a combo when there are two or more), and the press that
        // connects the one picked.
        writer_choice = new QComboBox(offers);
        writer_choice->setObjectName(QStringLiteral("pelican.renderFeatures.writer"));
        writer_choice->setToolTip(owner.tr("The output that draws the target"));
        offer_row->addWidget(writer_choice);
        write_from = new QPushButton(offers);
        write_from->setObjectName(QStringLiteral("pelican.renderFeatures.writeFrom"));
        write_from->setToolTip(owner.tr(
            "Connect the output to the target's render.target_ref in the Graph and add the "
            "feature again, as one edit."));
        offer_row->addWidget(write_from);
        // Decision 4: a target the feature draws that nothing reads, shown.
        show_target = new QPushButton(offers);
        show_target->setObjectName(QStringLiteral("pelican.renderFeatures.showTarget"));
        show_target->setToolTip(owner.tr(
            "The pass that draws to the display reads the target in place of its first input, "
            "and the pass the feature goes after draws its colour into it first."));
        offer_row->addWidget(show_target);
        place_anchor = new QPushButton(offers);
        place_anchor->setObjectName(QStringLiteral("pelican.renderFeatures.placeAnchor"));
        offer_row->addWidget(place_anchor);
        fix_target_ref = new QPushButton(offers);
        fix_target_ref->setObjectName(QStringLiteral("pelican.renderFeatures.fixTargetRef"));
        fix_target_ref->setToolTip(owner.tr(
            "Write the type the engine expects into the render.target_ref, then send the "
            "refused change again."));
        offer_row->addWidget(fix_target_ref);
        offer_row->addStretch();
        layout->addWidget(offers);
        hideOffers();
        QObject::connect(add_with_targets, &QPushButton::clicked, &owner, [this] {
            const auto reference = offered_reference;
            const auto name = offered_name;
            const auto extra = offered_target;
            // UX-12d: the writer the question named goes in the same edit.
            const auto writer_target = offered_writer_target;
            const auto writer = pickedWriter();
            hideOffers();
            staged.reset();
            readForTargets(reference, name, false, extra, writer ? writer_target : std::nullopt, writer);
        });
        QObject::connect(writer_choice, &QComboBox::currentIndexChanged, &owner, [this] { writerPicked(); });
        QObject::connect(write_from, &QPushButton::clicked, &owner, [this] {
            const auto reference = offered_reference;
            const auto name = offered_name;
            const auto writer_target = offered_writer_target;
            const auto writer = pickedWriter();
            if (!writer_target || !writer) return;
            hideOffers();
            staged.reset();
            readForTargets(reference, name, false, std::nullopt, writer_target, writer);
        });
        QObject::connect(show_target, &QPushButton::clicked, &owner, [this] { startShow(); });
        QObject::connect(place_anchor, &QPushButton::clicked, &owner, [this] { placeAnchor(); });
        QObject::connect(fix_target_ref, &QPushButton::clicked, &owner, [this] { startFix(); });

        auto *actions = new QHBoxLayout;
        refresh = new QPushButton(owner.tr("Refresh"), &owner);
        refresh->setObjectName(
            QStringLiteral("pelican.renderFeatures.refresh"));
        apply = new QPushButton(owner.tr("Apply"), &owner);
        apply->setObjectName(
            QStringLiteral("pelican.renderFeatures.apply"));
        actions->addWidget(refresh);
        auto *procedural_assets = new QPushButton(owner.tr("Assets"), &owner);
        procedural_assets->setObjectName(QStringLiteral("pelican.renderFeatures.proceduralAssets"));
        actions->addWidget(procedural_assets);
        QObject::connect(procedural_assets, &QPushButton::clicked, &owner, [this] {
            auto *dialog = new ProceduralAssetDialog(viewport, &owner, [this] {
                requestRefresh();
                emit owner.renderConfigCommitted();
            });
            dialog->show();
        });
        auto *tasks = new QPushButton(owner.tr("Tasks"), &owner);
        tasks->setObjectName(QStringLiteral("pelican.renderFeatures.tasks"));
        actions->addWidget(tasks);
        QObject::connect(tasks, &QPushButton::clicked, &owner, [this] { owner.editComputeTask({}); });
        auto *fork_preset = new QPushButton(owner.tr("Fork"), &owner);
        fork_preset->setObjectName(QStringLiteral("pelican.renderFeatures.forkPreset"));
        actions->addWidget(fork_preset);
        QObject::connect(fork_preset, &QPushButton::clicked, &owner, [this] {
            auto *dialog = new PresetEditDialog(viewport, &owner, [this] { requestRefresh(); });
            dialog->setEngineRestartAction(restartAction());
            adoptHistoryActions(dialog);
            dialog->show();
        });
        auto *region_replacements = new QPushButton(owner.tr("Regions"), &owner);
        region_replacements->setObjectName(QStringLiteral("pelican.renderFeatures.regionReplacements"));
        actions->addWidget(region_replacements);
        QObject::connect(region_replacements, &QPushButton::clicked, &owner, [this] {
            auto *dialog = new PresetEditDialog(viewport, &owner, [this] {
                requestRefresh();
                emit owner.renderConfigCommitted();
            }, PresetEditDialog::Mode::Region);
            dialog->setEngineRestartAction(restartAction());
            adoptHistoryActions(dialog);
            dialog->show();
        });
        auto *scene_as = new QPushButton(owner.tr("Scene AS"), &owner);
        auto *callable_records = new QPushButton(owner.tr("Records"), &owner);
        // RTL-2s-c: the Graph's ray tracing section opens this one too.
        callable_records->setObjectName(QStringLiteral("pelican.renderFeatures.callableRecords"));
        actions->addWidget(callable_records);
        QObject::connect(callable_records, &QPushButton::clicked, &owner, [this] {
            auto *dialog = new CallableRecordsDialog(viewport, &owner);
            dialog->setEngineRestartAction(restartAction());
            adoptHistoryActions(dialog);
            dialog->show();
        });
        scene_as->setObjectName(QStringLiteral("pelican.renderFeatures.sceneAccelerationStructures"));
        actions->addWidget(scene_as);
        QObject::connect(scene_as, &QPushButton::clicked, &owner, [this] {
            auto *dialog = new SceneAccelerationStructuresDialog(viewport, &owner);
            dialog->setEngineRestartAction(restartAction());
            adoptHistoryActions(dialog);
            dialog->show();
        });
        actions->addStretch();
        actions->addWidget(apply);
        layout->addLayout(actions);

        result_poll = new QTimer(&owner);
        result_poll->setInterval(40);
        live_timer = new QTimer(&owner);
        live_timer->setSingleShot(true);
        QObject::connect(live_timer, &QTimer::timeout, &owner, [this] { flushLiveParameters(); });

        QObject::connect(
            refresh, &QPushButton::clicked, &owner,
            [this] { requestRefresh(); });
        QObject::connect(
            add, &QPushButton::clicked, &owner,
            [this] { stageAdd(); });
        QObject::connect(
            remove, &QPushButton::clicked, &owner,
            [this] { stageRemove(); });
        QObject::connect(
            apply, &QPushButton::clicked, &owner,
            [this] { submit(); });
        QObject::connect(
            current, &QListWidget::itemSelectionChanged,
            &owner, [this] { updateControls(); });
        QObject::connect(
            catalog, &QTreeWidget::itemSelectionChanged,
            &owner, [this] { updateControls(); });
        QObject::connect(
            result_poll, &QTimer::timeout, &owner,
            [this] { requestResult(); });
        QObject::connect(
            &viewport,
            &EmbeddedViewport::engineRpcBecameAvailable,
            &owner, [this] {
                pending.clear();
                active_ticket.clear();
                result_poll->stop();
                requestRefresh();
            });
        QObject::connect(
            &viewport,
            &EmbeddedViewport::engineRpcBecameUnavailable,
            &owner, [this](const QString &message) {
                pending.clear();
                active_ticket.clear();
                result_poll->stop();
                can_edit = false;
                restart_asked = false;
                showError(message);
                updateControls();
            });
        QObject::connect(
            &viewport,
            &EmbeddedViewport::inspectorRpcSucceeded,
            &owner,
            [this](qint64 request_id,
                   const QByteArray &result_json) {
                receiveSuccess(request_id, result_json);
            });
        QObject::connect(
            &viewport,
            &EmbeddedViewport::inspectorRpcFailed,
            &owner,
            [this](qint64 request_id,
                   const QString &message) {
                receiveFailure(request_id, message);
            });

        updateControls();
        if (viewport.rpcReady()) {
            requestRefresh();
        } else {
            showInformation(owner.tr(
                "Open a project to query the engine feature catalog."));
        }
    }

    // N-7 rule 2: one line, hidden when there is nothing to say. The whole
    // message stays reachable as the row's tooltip, and the Output dock
    // still receives everything the panel logs.
    void showNotice(const QString &colour, const QString &message) {
        notice_sticky = false;
        hideOffers();
        notice->setStyleSheet(colour);
        setPanelNotice(notice, oneLine(message), message);
    }

    void showError(const QString &message) {
        showNotice(QStringLiteral("color: #d94c3d;"), message);
    }

    void showInformation(const QString &message) {
        showNotice(QStringLiteral("color: #4f7cac;"), message);
    }

    void showSuccess(const QString &message) {
        showNotice(QStringLiteral("color: #388e3c;"), message);
    }

    // UX-12b: a line of two sentences (what is missing, what to do) shown
    // whole; oneLine would keep the first.
    void showWhole(const QString &colour, const QString &line) {
        showNotice(colour, line);
        setPanelNotice(notice, line, line);
    }

    qint64 request(RequestKind kind, const QString &method,
                   const QJsonObject &params,
                   quint64 generation = 0) {
        QString error;
        const auto id = viewport.requestRpc(
            method, params, &error);
        if (id == 0) {
            showError(error);
            return 0;
        }
        pending.insert(id, PendingRequest{
                               .kind = kind,
                               .refresh_generation = generation,
                           });
        return id;
    }

    void requestRefresh() {
        if (!viewport.rpcReady()) {
            showError(owner.tr(
                "The engine RPC connection is not ready."));
            return;
        }
        ++refresh_generation;
        have_current = false;
        have_catalog = false;
        const auto generation = refresh_generation;
        (void)request(RequestKind::CanEdit,
                      QStringLiteral("can_edit"), {},
                      generation);
        (void)request(RequestKind::Current,
                      QStringLiteral("get_render_features"), {},
                      generation);
        (void)request(RequestKind::Catalog,
                      QStringLiteral("list_render_features"), {},
                      generation);
        // LP-4f2: which features have a loop version -- the engine's answer,
        // not a table of Studio's.
        (void)request(RequestKind::Templates,
                      QStringLiteral("get_graph_templates"), {},
                      generation);
        showInformation(owner.tr(
            "Refreshing authored features and engine catalog..."));
        updateControls();
    }

    void stageAdd() {
        const auto selected = catalog->selectedItems();
        if (selected.isEmpty()) return;
        const auto *item = selected.front();
        const auto name =
            item->data(0, FeatureNameRole).toString();
        const auto reference =
            item->data(0, ReferenceRole).toString();
        if (!item->data(0, AvailableRole).toBool()) {
            showError(item->data(
                0, UnavailableReasonRole).toString());
            staged.reset();
            updateControls();
            return;
        }
        staged = StagedOperation{
            .op = QStringLiteral("add"),
            .reference = reference,
            .display_name = name,
        };
        showInformation(owner.tr("Pending add: %1")
                            .arg(name));
        updateControls();
        // UX-12b, decision 1: what it needs, looked at before it is sent.
        checkRequiredTargets(reference, name);
    }

    // --- UX-12b, decisions 1, 2 and 4.
    void hideOffers() {
        if (offers == nullptr) return;
        add_with_targets->hide();
        place_anchor->hide();
        fix_target_ref->hide();
        writer_choice->hide();
        write_from->hide();
        show_target->hide();
        offers->hide();
    }

    // --- UX-12d, decision 1.
    std::optional<WriterCandidate> pickedWriter() const {
        if (!offered_writer_target || offered_writers.empty()) return std::nullopt;
        const auto index = writer_choice->currentIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= offered_writers.size())
            return offered_writers.front();
        return offered_writers[static_cast<std::size_t>(index)];
    }

    void fillWriters(const FeatureRequiredTarget &target, const std::vector<WriterCandidate> &writers) {
        offered_writer_target = target;
        offered_writers = writers;
        const QSignalBlocker blocker{writer_choice};
        writer_choice->clear();
        for (const auto &writer : writers)
            writer_choice->addItem(QString::fromStdString(writerCandidateLabel(writer)));
        writer_choice->setCurrentIndex(0);
        writer_choice->setVisible(writers.size() > 1);
        writer_choice->setEnabled(can_edit);
    }

    // The combo's pick: the button, and the question that names it.
    void writerPicked() {
        const auto writer = pickedWriter();
        if (!writer || !offered_writer_target) return;
        const auto label = QString::fromStdString(writerCandidateLabel(*writer));
        write_from->setText(owner.tr("Write %1 from %2")
                                .arg(QString::fromStdString(offered_writer_target->name), label));
        if (add_with_targets->isVisibleTo(&owner) && !offered_missing.empty()) {
            const auto line = QString::fromStdString(
                requiredTargetsQuestion(offered_name.toStdString(), offered_missing,
                                        offered_writer_target->name, *writer));
            setPanelNotice(notice, line, line);
        }
    }

    // A refusal that names the outputs that could write the target: Write
    // <target> from <output> adds the feature again with it connected.
    void offerWriters(const QString &reference, const QString &name, const FeatureRequiredTarget &target,
                      const std::vector<WriterCandidate> &writers) {
        offered_reference = reference;
        offered_name = name;
        offered_missing.clear();
        fillWriters(target, writers);
        offers->show();
        write_from->show();
        write_from->setEnabled(can_edit);
        writerPicked();
    }

    static nlohmann::json jsonOf(const QJsonObject &object) {
        return nlohmann::json::parse(
            QJsonDocument{object}.toJson(QJsonDocument::Compact).toStdString(), nullptr, false);
    }
    static QJsonObject qtOf(const nlohmann::json &object) {
        return QJsonDocument::fromJson(QByteArray::fromStdString(object.dump())).object();
    }

    QString referenceNamed(const std::string &name) const {
        for (const auto &[reference, entry] : catalog_entries)
            if (entry.is_object() && entry.value("name", std::string{}) == name)
                return QString::fromStdString(reference);
        return {};
    }

    void checkRequiredTargets(const QString &reference, const QString &name) {
        const auto entry = catalog_entries.find(reference.toStdString());
        if (entry == catalog_entries.end()) return;
        if (featureRequiredTargets(entry->second).empty() && !entry->second.contains("insertion"))
            return;
        readForTargets(reference, name, true);
    }

    // The authoring context (render targets, passes) and the Graph's
    // nodes, then chainRead.
    void readForTargets(const QString &reference, const QString &name, bool ask_only,
                        std::optional<FeatureRequiredTarget> extra = std::nullopt,
                        std::optional<FeatureRequiredTarget> writer_target = std::nullopt,
                        std::optional<WriterCandidate> writer = std::nullopt) {
        chain = TargetsEdit{};
        chain->ask_only = ask_only;
        chain->reference = reference;
        chain->name = name;
        chain->extra = std::move(extra);
        chain->writer_target = std::move(writer_target);
        chain->writer = std::move(writer);
        if (request(RequestKind::ChainContext, QStringLiteral("get_render_authoring_context"), {}) ==
                0 ||
            request(RequestKind::ChainGraph, QStringLiteral("get_graph_instances"), {}) == 0) {
            chain.reset();
            return;
        }
        if (!ask_only)
            showInformation(owner.tr("Adding %1 with its targets: reading the document...").arg(name));
        updateControls();
    }

    void chainReceive(RequestKind kind, const QJsonObject &response) {
        if (!chain) return;
        if (kind == RequestKind::ChainSubmit) {
            chainSubmitted(response);
            return;
        }
        const auto json = jsonOf(response);
        if (kind == RequestKind::ChainContext) {
            chain->context = json.is_object() ? json : nlohmann::json::object();
            chain->declared.clear();
            chain->passes.clear();
            for (const auto &target : json.value("render_targets", nlohmann::json::array()))
                if (target.is_object() && target.contains("name") && target.at("name").is_string())
                    chain->declared.push_back(target.at("name").get<std::string>());
            for (const auto &graph : json.value("graphs", nlohmann::json::array())) {
                if (!graph.is_object()) continue;
                for (const auto &pass : graph.value("passes", nlohmann::json::array()))
                    if (pass.is_object() && pass.contains("name") && pass.at("name").is_string())
                        chain->passes.push_back(pass.at("name").get<std::string>());
            }
            chain->have_context = true;
        } else {
            chain->instances = json.is_object() ? json : nlohmann::json::object();
            chain->have_graph = true;
        }
        if (chain->step == TargetsEdit::Step::Reading && chain->have_context && chain->have_graph)
            chainRead();
        else if (chain->step == TargetsEdit::Step::FixReading && chain->have_graph)
            fixRead();
        else if ((chain->step == TargetsEdit::Step::ShowCheck ||
                  chain->step == TargetsEdit::Step::ShowReading) &&
                 chain->have_context && chain->have_graph)
            showRead();
    }

    void chainRead() {
        const auto found = catalog_entries.find(chain->reference.toStdString());
        const auto entry = found != catalog_entries.end() ? found->second : nlohmann::json::object();
        auto required = featureRequiredTargets(entry);
        if (chain->extra && std::ranges::none_of(required, [&](const auto &target) {
                return target.name == chain->extra->name;
            }))
            required.push_back(*chain->extra);
        chain->missing = missingRequiredTargets(
            required, chain->declared, chain->instances.value("nodes", nlohmann::json::array()));
        if (chain->ask_only) {
            auto asked = std::exchange(chain, std::nullopt);
            updateControls();
            // An engine that lists where a feature goes (FR-2): the place
            // first, since the targets alone would still be refused.
            // The passes are the composed config's (the authoring context)
            // and the Graph's: a graph pass lands under its node's id, and the
            // context's rendering_passes is read before the Graph composes.
            auto passes = asked->passes;
            for (const auto &node : asked->instances.value("nodes", nlohmann::json::array()))
                if (node.is_object() && node.contains("id") && node.at("id").is_string())
                    passes.push_back(node.at("id").get<std::string>());
            if (const auto anchors = missingInsertionAnchors(entry, passes); !anchors.empty()) {
                offerAnchor(asked->name, anchors.front());
                return;
            }
            if (asked->missing.empty()) return;
            offerTargets(asked->reference, asked->name, asked->missing);
            // UX-12d, decision 1 (engine FR-3): a target the feature only
            // reads (needs_writer) that the document does not declare has no
            // writer either; an output of the Graph that draws its kind is
            // named in the question and connected in the same edit.
            const auto nodes = asked->instances.value("nodes", nlohmann::json::array());
            for (const auto &target : asked->missing) {
                if (!target.needs_writer || !target.declare) continue;
                const auto writers = graphWriterCandidates(target.type, nodes);
                if (writers.empty()) continue;
                offered_missing = asked->missing;
                fillWriters(target, writers);
                writerPicked();
                break;
            }
            return;
        }
        sendAddBatch();
    }

    // UX-12d, decision 2: what Add with targets sends -- the targets, their
    // target_refs (and the writer's connection) and the feature -- as one
    // edit_batch: one ticket, nothing written when it is refused, one undo.
    void sendAddBatch() {
        const auto nodes = chain->instances.value("nodes", nlohmann::json::array());
        auto edits = nlohmann::json::array();
        chain->batch_steps.clear();
        if (const auto ops = requiredTargetAddOps(chain->missing); !ops.empty()) {
            edits.push_back({{"method", "edit_render_targets"}, {"params", {{"ops", ops}}}});
            chain->batch_steps.push_back(owner.tr("declaring the render targets"));
        }
        chain->reference_ops = requiredTargetReferenceOps(chain->missing, nodes);
        auto graph_ops = chain->reference_ops;
        if (chain->writer_target && chain->writer)
            for (auto &op : writerConnectOps(*chain->writer_target, *chain->writer, nodes, chain->reference_ops))
                graph_ops.push_back(std::move(op));
        if (!graph_ops.empty()) {
            edits.push_back({{"method", "edit_graph_document"}, {"params", {{"ops", graph_ops}}}});
            chain->batch_steps.push_back(chain->writer ? owner.tr("the Graph's target references and %1's writer")
                                                             .arg(QString::fromStdString(chain->writer_target->name))
                                                       : owner.tr("the Graph's target references"));
        }
        edits.push_back({{"method", "edit_render_features"},
                         {"params",
                          {{"operations",
                            nlohmann::json::array({{{"op", "add"}, {"feature", chain->reference.toStdString()}}})}}}});
        chain->batch_steps.push_back(owner.tr("adding the feature"));
        chain->step = TargetsEdit::Step::Batch;
        chainSubmit(QStringLiteral("edit_batch"),
                    {{"base_source_digest", source_digest.toStdString()}, {"edits", std::move(edits)}},
                    chain->writer ? owner.tr("Adding %1 with its targets, %2 writing %3...")
                                        .arg(chain->name,
                                             QString::fromStdString(writerCandidateLabel(*chain->writer)),
                                             QString::fromStdString(chain->writer_target->name))
                                  : owner.tr("Adding %1 with its targets...").arg(chain->name));
    }

    void chainSubmit(const QString &method, const nlohmann::json &params, const QString &line) {
        if (request(RequestKind::ChainSubmit, method, qtOf(params)) == 0) {
            chain.reset();
            updateControls();
            return;
        }
        showInformation(line);
        updateControls();
    }

    void chainSubmitted(const QJsonObject &response) {
        if (response.value(QStringLiteral("status")).toString() != QStringLiteral("accepted")) {
            chainRefused(response);
            return;
        }
        active_ticket = response.value(QStringLiteral("ticket")).toString();
        if (active_ticket.isEmpty()) {
            chain.reset();
            showError(owner.tr("The engine accepted the edit without returning a ticket."));
            return;
        }
        result_poll->start();
        requestResult();
    }

    void chainResult(const QJsonObject &response) {
        if (response.value(QStringLiteral("status")).toString() != QStringLiteral("committed") ||
            !response.value(QStringLiteral("committed")).toBool(false)) {
            chainRefused(response);
            return;
        }
        const auto digest =
            response.value(QStringLiteral("source_digest")).toObject().value(QStringLiteral("hex")).toString();
        if (!digest.isEmpty()) {
            source_digest = digest;
            if (digest_hub) digest_hub->publish(digest.toStdString(), &owner);
        }
        if (completeEdit(response, restartAction()).restart_required) {
            chain.reset();
            return;
        }
        using Step = TargetsEdit::Step;
        switch (chain->step) {
        case Step::Batch: {
            const auto name = chain->name;
            const auto reference = chain->reference;
            const auto line =
                chain->writer ? owner.tr("Added %1 with the render targets it needs; %2 writes %3.")
                                    .arg(name, QString::fromStdString(writerCandidateLabel(*chain->writer)),
                                         QString::fromStdString(chain->writer_target->name))
                              : owner.tr("Added %1 with the render targets it needs.").arg(name);
            chain.reset();
            requestRefresh();
            showWhole(QStringLiteral("color: #388e3c;"), line);
            notice_sticky = true;
            emit owner.renderConfigCommitted();
            checkShowTarget(reference, name, line);
            return;
        }
        case Step::FixBatch: {
            const auto node = chain->name;
            chain.reset();
            requestRefresh();
            showSuccess(owner.tr("%1 takes the type the engine expects.").arg(node));
            notice_sticky = true;
            emit owner.renderConfigCommitted();
            return;
        }
        case Step::ShowPresent: {
            const auto plan = *chain->show;
            chain.reset();
            requestRefresh();
            showSuccess(owner.tr("%1 shows %2.").arg(QString::fromStdString(plan.present),
                                                     QString::fromStdString(plan.target)));
            notice_sticky = true;
            emit owner.renderConfigCommitted();
            return;
        }
        case Step::Reading:
        case Step::FixReading:
        case Step::ShowCheck:
        case Step::ShowReading:
            return;
        }
    }

    // UX-12d, decision 2: the engine took nothing of a refused batch back
    // because it wrote nothing; the line names the edit the refusal met
    // (batch_index), then the way out the refusal names.
    void chainRefused(const QJsonObject &response) {
        using Step = TargetsEdit::Step;
        const auto name = chain->name;
        const auto error = jsonOf(response).value("error", nlohmann::json::object());
        switch (chain->step) {
        case Step::Batch: {
            const auto reference = chain->reference;
            QString line = responseError(response);
            if (const auto index = batchIndexOf(error); index && *index < chain->batch_steps.size())
                line = owner.tr("%1 was refused at %2: %3").arg(name, chain->batch_steps[*index], line);
            chain.reset();
            showError(line);
            notice_sticky = true;
            StagedOperation added{.op = QStringLiteral("add"), .reference = reference, .display_name = name};
            offerFixes(response, added, name);
            return;
        }
        case Step::ShowPresent: {
            // UX-13d: one edit_batch -- a refusal wrote nothing, so there is
            // nothing to take back; the line names the edit it met.
            const auto plan = *chain->show;
            QString line = responseError(response);
            if (const auto index = batchIndexOf(error); index && *index < chain->batch_steps.size())
                line = owner.tr("Connecting %1 to %2 was refused at %3: %4")
                           .arg(QString::fromStdString(plan.target), QString::fromStdString(plan.present),
                                chain->batch_steps[*index], line);
            chain.reset();
            showError(line);
            notice_sticky = true;
            return;
        }
        default: {
            const auto resend = chain->resend;
            QString line = responseError(response);
            if (const auto index = batchIndexOf(error);
                chain->step == Step::FixBatch && index && *index < chain->batch_steps.size())
                line = owner.tr("Fixing %1 was refused at %2: %3").arg(name, chain->batch_steps[*index], line);
            chain.reset();
            showError(line);
            offerFixes(response, resend, name);
            return;
        }
        }
    }

    void offerTargets(const QString &reference, const QString &name,
                      const std::vector<FeatureRequiredTarget> &missing) {
        offered_reference = reference;
        offered_name = name;
        offered_target.reset();
        offered_writer_target.reset();
        offered_writers.clear();
        offered_missing.clear();
        showWhole(QStringLiteral("color: #4f7cac;"),
                  QString::fromStdString(requiredTargetsQuestion(name.toStdString(), missing)));
        notice_sticky = true;
        offers->show();
        add_with_targets->show();
        add_with_targets->setEnabled(can_edit);
    }

    void offerAnchor(const QString &feature, const std::string &anchor) {
        const auto kind = fullscreenShaderTemplateKindFromName(anchor);
        const auto label = kind ? std::string{fullscreenShaderTemplateKindLabel(*kind)} : anchor;
        showWhole(QStringLiteral("color: #d94c3d;"),
                  QString::fromStdString(insertionAnchorLine(feature.toStdString(), anchor, label)));
        notice_sticky = true;
        offered_anchor = QString::fromStdString(anchor);
        place_anchor->setText(owner.tr("Place %1 in the Graph").arg(QString::fromStdString(label)));
        place_anchor->setEnabled(static_cast<bool>(place_anchor_action));
        place_anchor->setToolTip(place_anchor_action
                                     ? owner.tr("A fullscreen pass named %1 in the Graph, with "
                                                "New\u2026 open on its shader template")
                                           .arg(offered_anchor)
                                     : owner.tr("The Graph tab is not open"));
        offers->show();
        place_anchor->show();
    }

    // The way out a refusal names, under the line it put up (decisions 1, 2
    // and 4). `refused` is the edit that was refused, `name` its feature.
    void offerFixes(const QJsonObject &response, const std::optional<StagedOperation> &refused,
                    const QString &name) {
        const auto error = jsonOf(response).value("error", nlohmann::json::object());
        // UX-12d, decision 1 (engine FR-3): a target declared and drawn by
        // nobody, with the outputs that could draw it.
        if (const auto unwritten = requiredTargetUnwritten(error)) {
            const auto feature =
                unwritten->feature.empty() ? name : QString::fromStdString(unwritten->feature);
            showWhole(QStringLiteral("color: #d94c3d;"),
                      QString::fromStdString(requiredTargetUnwrittenLine(*unwritten)));
            notice_sticky = true;
            const auto reference = refused && refused->op == QStringLiteral("add")
                                       ? refused->reference
                                       : referenceNamed(unwritten->feature);
            // A pass of the rendering section is no graph port to connect.
            std::vector<WriterCandidate> writers;
            for (const auto &candidate : unwritten->candidates)
                if (!candidate.node.empty()) writers.push_back(candidate);
            if (reference.isEmpty() || writers.empty()) return;
            offerWriters(reference, feature, unwritten->target, writers);
            return;
        }
        if (const auto target = requiredTargetFromRefusal(error)) {
            const auto feature = error.value("feature", std::string{});
            const auto reference = refused && refused->op == QStringLiteral("add")
                                       ? refused->reference
                                       : referenceNamed(feature);
            if (reference.isEmpty()) return;
            offerTargets(reference, feature.empty() ? name : QString::fromStdString(feature),
                         {*target});
            offered_target = *target;
            return;
        }
        if (const auto anchor = insertionAnchorMissing(error)) {
            offerAnchor(anchor->feature.empty() ? name : QString::fromStdString(anchor->feature),
                        anchor->anchor);
            return;
        }
        if (const auto fix = targetRefTypeFix(error)) {
            offered_fix = *fix;
            offered_resend.reset();
            if (refused && refused->op == QStringLiteral("set_parameters")) offered_resend = refused;
            fix_target_ref->setText(QString::fromStdString(targetRefTypeFixLabel(*fix)));
            fix_target_ref->setEnabled(can_edit);
            notice_sticky = true;
            offers->show();
            fix_target_ref->show();
        }
    }

    void placeAnchor() {
        const auto anchor = offered_anchor;
        if (anchor.isEmpty() || !place_anchor_action) return;
        hideOffers();
        if (!place_anchor_action(anchor)) {
            showError(owner.tr("The Graph could not place %1 now.").arg(anchor));
            return;
        }
        showInformation(owner.tr("%1 is placed in the Graph: create its shader there, then add the "
                                 "feature again.")
                            .arg(anchor));
    }

    // Decision 4: the target_ref's node as the document has it, its type
    // replaced, with the ports that read it (fixRead), and the refused change
    // once more -- UX-12d, decision 3: one edit_batch, the change first.
    void startFix() {
        if (!offered_fix) return;
        chain = TargetsEdit{};
        chain->step = TargetsEdit::Step::FixReading;
        chain->fix = offered_fix;
        chain->resend = offered_resend;
        chain->name = QString::fromStdString(offered_fix->node);
        hideOffers();
        if (request(RequestKind::ChainGraph, QStringLiteral("get_graph_instances"), {}) == 0) {
            chain.reset();
            return;
        }
        showInformation(owner.tr("Fixing %1: reading the Graph...").arg(chain->name));
        updateControls();
    }

    void fixRead() {
        const auto ops = targetRefTypeFixOps(*chain->fix, chain->instances);
        if (ops.empty()) {
            const auto name = chain->name;
            chain.reset();
            showError(owner.tr("The Graph has no render.target_ref %1 now.").arg(name));
            updateControls();
            return;
        }
        auto edits = nlohmann::json::array();
        chain->batch_steps.clear();
        if (chain->resend) {
            const auto &again = *chain->resend;
            nlohmann::json operation{{"op", "set_parameters"},
                                     {"index", again.index},
                                     {"parameters", jsonOf(again.parameters)},
                                     {"scope", again.scope.toStdString()}};
            if (again.scope == QStringLiteral("preset"))
                operation["base_preset_digest"] = again.preset_digest.toStdString();
            edits.push_back({{"method", "edit_render_features"},
                             {"params", {{"operations", nlohmann::json::array({operation})}}}});
            chain->batch_steps.push_back(owner.tr("%1's change").arg(again.display_name));
        }
        edits.push_back({{"method", "edit_graph_document"}, {"params", {{"ops", ops}}}});
        chain->batch_steps.push_back(owner.tr("the type in the Graph"));
        chain->step = TargetsEdit::Step::FixBatch;
        chainSubmit(QStringLiteral("edit_batch"),
                    {{"base_source_digest", source_digest.toStdString()}, {"edits", std::move(edits)}},
                    chain->resend ? owner.tr("Fixing %1: the type the engine expects, with %2's change...")
                                        .arg(chain->name, chain->resend->display_name)
                                  : owner.tr("Fixing %1: writing the type the engine expects...").arg(chain->name));
    }

    // --- UX-12d, decision 4: a target the feature draws that nothing reads
    // (a sky drawn into a colour target on a Graph-built pipeline), and the
    // press that shows it.
    void checkShowTarget(const QString &reference, const QString &name, const QString &line) {
        const auto entry = catalog_entries.find(reference.toStdString());
        if (entry == catalog_entries.end() || featureRequiredTargets(entry->second).empty()) return;
        chain = TargetsEdit{};
        chain->step = TargetsEdit::Step::ShowCheck;
        chain->reference = reference;
        chain->name = name;
        chain->line = line;
        if (request(RequestKind::ChainContext, QStringLiteral("get_render_authoring_context"), {}) == 0 ||
            request(RequestKind::ChainGraph, QStringLiteral("get_graph_instances"), {}) == 0)
            chain.reset();
        updateControls();
    }

    void startShow() {
        const auto reference = offered_show_reference;
        const auto name = offered_name;
        if (reference.isEmpty()) return;
        hideOffers();
        chain = TargetsEdit{};
        chain->step = TargetsEdit::Step::ShowReading;
        chain->reference = reference;
        chain->name = name;
        if (request(RequestKind::ChainContext, QStringLiteral("get_render_authoring_context"), {}) == 0 ||
            request(RequestKind::ChainGraph, QStringLiteral("get_graph_instances"), {}) == 0) {
            chain.reset();
            return;
        }
        showInformation(owner.tr("Showing what %1 draws: reading the document...").arg(name));
        updateControls();
    }

    void showRead() {
        const auto found = catalog_entries.find(chain->reference.toStdString());
        const auto entry = found != catalog_entries.end() ? found->second : nlohmann::json::object();
        auto plan = showTargetPlan(entry, chain->reference.toStdString(), chain->context,
                                   chain->instances.value("nodes", nlohmann::json::array()));
        if (chain->step == TargetsEdit::Step::ShowCheck) {
            const auto checked = std::exchange(chain, std::nullopt);
            updateControls();
            if (!plan) return;
            const auto line = (checked->line.isEmpty() ? QString{} : checked->line + QStringLiteral(" ")) +
                              QString::fromStdString(showTargetLine(*plan));
            showWhole(QStringLiteral("color: #4f7cac;"), line);
            notice_sticky = true;
            offered_show_reference = checked->reference;
            offered_name = checked->name;
            show_target->setText(owner.tr("Connect %1 to %2")
                                     .arg(QString::fromStdString(plan->target), QString::fromStdString(plan->present)));
            show_target->setEnabled(can_edit);
            offers->show();
            show_target->show();
            return;
        }
        if (!plan) {
            const auto name = chain->name;
            chain.reset();
            showInformation(owner.tr("What %1 draws is read already.").arg(name));
            return;
        }
        // present's edit is made from the context as read; the anchor's
        // colour goes in first, then present reads the target -- UX-13d
        // (engine FR-4): one edit_batch, one ticket, one undo.
        const auto authoring = framePlanAuthoringFromContext(chain->context);
        std::string fragment;
        if (const auto *row = authoring.find(plan->graph, plan->present)) {
            const auto shader = row->editable_declaration.value("shader", nlohmann::json::object());
            if (shader.is_object()) fragment = shader.value("fragment", std::string{});
        }
        auto edit = framePlanPassInputsAndShaderRequest(authoring, plan->graph, plan->present,
                                                        plan->present_inputs, fragment);
        if (!edit.sendable()) {
            chain.reset();
            showError(QString::fromStdString(edit.refusal));
            updateControls();
            return;
        }
        chain->show = std::move(plan);
        sendShow(std::move(edit.params));
    }

    // UX-13d, decision 1 (engine FR-4): the anchor's colour into the target
    // and present's inputs as one edit_batch. Every digest is the one read
    // with the plan: the batch's is the source's, and present's preset (or
    // feature) digest stays the one before the batch -- the engine reads the
    // preset as the graph edit left it.
    void sendShow(nlohmann::json present) {
        const auto &plan = *chain->show;
        const auto read_digest = present.value("base_source_digest", std::string{});
        const auto base = read_digest.empty() ? source_digest.toStdString() : read_digest;
        chain->step = TargetsEdit::Step::ShowPresent;
        chain->batch_steps.clear();
        const auto line = owner.tr("Connecting %1 to %2...")
                              .arg(QString::fromStdString(plan.target), QString::fromStdString(plan.present));
        if (plan.graph_ops.empty()) {
            // Nothing for the Graph to draw first: present's edit alone.
            present["base_source_digest"] = base;
            chainSubmit(QStringLiteral("edit_preset_pass"), present, line);
            return;
        }
        present.erase("base_source_digest"); // the batch's is the only one
        auto edits = nlohmann::json::array(
            {{{"method", "edit_graph_document"}, {"params", {{"ops", plan.graph_ops}}}},
             {{"method", "edit_preset_pass"}, {"params", std::move(present)}}});
        chain->batch_steps.push_back(owner.tr("the pass before it drawing into %1").arg(QString::fromStdString(plan.target)));
        chain->batch_steps.push_back(owner.tr("%1 reading it").arg(QString::fromStdString(plan.present)));
        chainSubmit(QStringLiteral("edit_batch"), {{"base_source_digest", base}, {"edits", std::move(edits)}}, line);
    }

    void stageMaterialize() {
        const auto *item = current->currentItem();
        if (!item || staged) return;
        const auto instance = item->data(InstanceRole).toJsonObject();
        if (!instance.value("materializable").toBool()) return;
        const auto base_digest = source_digest;
        const auto display_name = item->text();
        QDialog dialog(&owner);
        dialog.setObjectName("pelican.featureMaterialize");
        dialog.setWindowTitle(owner.tr("Make feature editable: %1").arg(display_name));
        auto *layout = new QVBoxLayout(&dialog);
        const QString materialize_help = owner.tr("Create a project feature with the current settings resolved into editable declarations. Apply in Render features to save and switch to it.");
        auto *form = new QFormLayout;
        auto *destination = new QLineEdit(&dialog);
        destination->setObjectName("pelican.featureMaterialize.destination");
        auto filename = instance.value("reference").toString().section('/', -1);
        if (filename.endsWith(".json")) filename.chop(5);
        destination->setText("project://passes/" + filename + "_editable.json");
        destination->setToolTip(materialize_help);
        form->addRow(owner.tr("New project file"), destination);
        layout->addLayout(form);
        auto *error = new QLabel(&dialog);
        layout->addWidget(error);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            const auto reference = destination->text().trimmed();
            if (!reference.startsWith("project://") || reference.size() <= 10 || reference.contains('#')) {
                error->setText(owner.tr("Enter a new project:// file path."));
                return;
            }
            dialog.accept();
        });
        if (dialog.exec() != QDialog::Accepted) return;
        if (source_digest != base_digest || !can_edit || !have_current) {
            showError(owner.tr("The feature list changed. Select the feature again."));
            return;
        }
        staged = StagedOperation{.op = "materialize", .reference = instance.value("reference").toString(),
            .display_name = display_name, .index = instance.value("index").toInt(-1), .base_digest = base_digest,
            .scope = instance.value("scope").toString("root"),
            .preset_digest = instance.value("source_digest").toObject().value("hex").toString(),
            .feature_digest = instance.value("feature_document_digest").toObject().value("hex").toString(),
            .destination = destination->text().trimmed()};
        showInformation(owner.tr("Pending editable feature: %1 → %2").arg(display_name, staged->destination));
        updateControls();
    }

    // UX-10, decision 1: the feature's parameters as fields made from its
    // schema (FeatureParameterForm). A change is sent at once, as one
    // set_parameters -- the render edit history's, so Undo takes it back --
    // and while a spin or a slider moves the changes are gathered for 150 ms
    // and sent once the edit before them has landed. The dialog stays open
    // (open, not exec) so the panel and a scenario keep going under it.
    void editParameters() {
        const auto *item = current->currentItem();
        if (!item) return;
        const auto instance = item->data(InstanceRole).toJsonObject();
        if (!instance.value("editable").toBool()) {
            showInformation(instance.value("reason").toString());
            return;
        }
        if (staged) {
            showInformation(owner.tr("Apply the pending edit first."));
            return;
        }
        if (live.dialog) {
            if (live.index == instance.value("index").toInt(-1) &&
                live.scope == instance.value("scope").toString("root")) {
                live.dialog->raise();
                live.dialog->activateWindow();
                return;
            }
            flushLiveParameters(true);
            live.dialog->close();
        }
        live = LiveParameters{};
        live.reference = instance.value("reference").toString();
        live.index = instance.value("index").toInt(-1);
        live.scope = instance.value("scope").toString("root");
        live.parameters = instance.value("parameters").toObject();
        auto *dialog = new QDialog(&owner);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(owner.tr("Feature parameters: %1").arg(item->text()));
        dialog->setObjectName("pelican.featureParameters");
        dialog->resize(560, 480);
        auto *layout = new QVBoxLayout(dialog);
        auto *hint = new QLabel(owner.tr("A change is applied at once; Undo takes it back."), dialog);
        hint->setToolTip(owner.tr("Reset puts a value back to the feature's default."));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        auto *scroll = new QScrollArea(dialog);
        scroll->setWidgetResizable(true);
        auto *form = new FeatureParameterForm(instance.value("parameter_schema").toObject(),
                                              live.parameters,
                                              instance.value("bound_parameters").toObject(), scroll);
        scroll->setWidget(form);
        layout->addWidget(scroll);
        form->setShaders(shaderChoices());
        form->setRenderTargets(render_target_names);
        form->setEditedCallback([this, form](bool continuous) {
            live.parameters = form->authored();
            live.waiting = true;
            live_timer->start(continuous ? 150 : 0);
        });
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        buttons->button(QDialogButtonBox::Close)->setObjectName("pelican.featureParameters.close");
        // Return in a field gives its value; it does not close the dialog.
        buttons->button(QDialogButtonBox::Close)->setAutoDefault(false);
        buttons->button(QDialogButtonBox::Close)->setDefault(false);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        // A change still gathering when the dialog closes is sent all the same.
        QObject::connect(dialog, &QObject::destroyed, &owner, [this, form] {
            // Another feature's parameters may be open by now.
            if (live.form != form) return;
            live.form = nullptr;
            if (live_timer->isActive()) {
                live_timer->stop();
                flushLiveParameters(true);
            }
        });
        live.dialog = dialog;
        live.form = form;
        adoptHistoryActions(dialog);
        // The lists the target and shader rows pick from.
        (void)request(RequestKind::Context, QStringLiteral("get_render_authoring_context"), {});
        (void)request(RequestKind::Shaders, QStringLiteral("get_engine_shaders"), {});
        dialog->show();
    }

    std::vector<std::pair<QString, QString>> shaderChoices() const {
        auto shaders = projectShaderReferences(viewport.projectRoot().toStdWString());
        shaders.insert(shaders.end(), engine_shaders.begin(), engine_shaders.end());
        return shaders;
    }

    // Nothing of this panel in flight: an edit sent now is built on the
    // revision the panel holds.
    bool liveIdle() const {
        if (!active_ticket.isEmpty() || retry_pending || chain) return false;
        for (auto it = pending.cbegin(); it != pending.cend(); ++it)
            if (it->kind == RequestKind::Submit || it->kind == RequestKind::Current ||
                it->kind == RequestKind::CanEdit || it->kind == RequestKind::Result)
                return false;
        return true;
    }

    // Sends the gathered change once the timer has run out (or `now`) and
    // the panel is idle; otherwise it waits for receiveSuccess / Failure.
    void flushLiveParameters(bool now = false) {
        if (!live.waiting || (!now && live_timer->isActive())) return;
        if (!can_edit || !have_current || !liveIdle()) return;
        if (staged && staged->op != QStringLiteral("set_parameters")) {
            showInformation(owner.tr("Apply the pending edit first."));
            return;
        }
        QJsonObject instance;
        for (int row = 0; row < current->count(); ++row) {
            const auto candidate = current->item(row)->data(InstanceRole).toJsonObject();
            if (candidate.value("index").toInt(-1) == live.index &&
                candidate.value("scope").toString("root") == live.scope &&
                candidate.value("reference").toString() == live.reference) {
                instance = candidate;
                break;
            }
        }
        live.waiting = false;
        if (instance.isEmpty()) {
            showError(owner.tr("The feature list changed. Open the feature's parameters again."));
            return;
        }
        if (instance.value("parameters").toObject() == live.parameters) return;
        staged = StagedOperation{.op = "set_parameters", .reference = live.reference,
            .display_name = current->currentItem() ? current->currentItem()->text() : live.reference,
            .index = live.index, .parameters = live.parameters, .base_digest = source_digest,
            .scope = live.scope,
            .preset_digest = instance.value("source_digest").toObject().value("hex").toString()};
        submit();
    }

    // The list was read again (a commit, another panel's, an Undo): the open
    // parameters show what the document holds, unless a change waits.
    void followLiveParameters() {
        if (!live.form || live.waiting || live_timer->isActive() || !liveIdle()) return;
        for (int row = 0; row < current->count(); ++row) {
            const auto candidate = current->item(row)->data(InstanceRole).toJsonObject();
            if (candidate.value("index").toInt(-1) != live.index ||
                candidate.value("scope").toString("root") != live.scope ||
                candidate.value("reference").toString() != live.reference)
                continue;
            const auto parameters = candidate.value("parameters").toObject();
            if (parameters != live.form->authored()) live.form->setAuthored(parameters);
            live.parameters = parameters;
            return;
        }
    }

    void stageRemove() {
        const auto selected = current->selectedItems();
        if (selected.isEmpty()) return;
        const auto *item = selected.front();
        staged = StagedOperation{
            .op = QStringLiteral("remove"),
            .reference =
                item->data(ReferenceRole).toString(),
            .display_name = item->text(),
        };
        showInformation(owner.tr("Pending removal: %1")
                            .arg(item->text()));
        updateControls();
    }

    void submit() {
        if (!staged || source_digest.isEmpty() ||
            !active_ticket.isEmpty()) {
            return;
        }
        if (staged->op == QStringLiteral("apply_graph_template")) {
            submitGraphTemplate();
            return;
        }
        QJsonObject operation{
            {QStringLiteral("op"), staged->op},
            {QStringLiteral("feature"), staged->reference},
        };
        const bool instance_edit = staged->op == "set_parameters" || staged->op == "materialize";
        if (instance_edit) {
            operation.remove("feature");
            operation["index"] = staged->index;
            if (staged->op == "set_parameters") operation["parameters"] = staged->parameters;
            else {
                operation["destination_reference"] = staged->destination;
                operation["base_feature_digest"] = staged->feature_digest;
            }
            operation["scope"] = staged->scope;
            if (staged->scope == "preset") operation["base_preset_digest"] = staged->preset_digest;
        }
        QJsonObject params{
            {QStringLiteral("base_source_digest"),
             instance_edit ? staged->base_digest : source_digest},
            {QStringLiteral("operations"),
             QJsonArray{operation}},
        };
        if (request(RequestKind::Submit,
                    QStringLiteral("edit_render_features"),
                    params) != 0) {
            submitted_staged = staged;
            submitted_retried = std::exchange(retrying, false);
            showInformation(owner.tr(
                "Submitting the render feature edit..."));
            updateControls();
        }
    }

    void requestResult() {
        if (active_ticket.isEmpty()) return;
        for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
            if (it->kind == RequestKind::Result) return;
        }
        (void)request(
            RequestKind::Result,
            QStringLiteral("get_edit_result"),
            {{QStringLiteral("ticket"), active_ticket}});
    }

    void receiveSuccess(qint64 request_id,
                        const QByteArray &result_json) {
        const auto found = pending.find(request_id);
        if (found == pending.end()) return;
        const auto request = found.value();
        pending.erase(found);
        QString error;
        const auto decoded = decodeObject(
            result_json, &error);
        if (!decoded) {
            showError(error);
            updateControls();
            return;
        }
        if (request.refresh_generation != 0 &&
            request.refresh_generation != refresh_generation) {
            return;
        }
        if (request.kind == RequestKind::Catalog) {
            owner.receiveCatalogResult(result_json);
            return;
        }
        if (request.kind == RequestKind::Templates) {
            templates = graphTemplatesFromResponse(nlohmann::json::parse(
                result_json.toStdString(), nullptr, false));
            have_templates = true;
            updateControls();
            return;
        }
        if (request.kind == RequestKind::Context || request.kind == RequestKind::Shaders) {
            consumeChoices(request.kind, *decoded);
            return;
        }
        if (request.kind == RequestKind::ChainContext || request.kind == RequestKind::ChainGraph ||
            request.kind == RequestKind::ChainSubmit) {
            chainReceive(request.kind, *decoded);
            updateControls();
            flushLiveParameters();
            return;
        }
        switch (request.kind) {
        case RequestKind::CanEdit:
            consumeCanEdit(*decoded);
            break;
        case RequestKind::Current:
            consumeCurrent(*decoded);
            break;
        case RequestKind::Catalog:
            consumeCatalog(*decoded);
            break;
        case RequestKind::Submit:
            consumeSubmit(*decoded);
            break;
        case RequestKind::Result:
            consumeResult(*decoded);
            break;
        case RequestKind::Templates:
        case RequestKind::Context:
        case RequestKind::Shaders:
        case RequestKind::ChainContext:
        case RequestKind::ChainGraph:
        case RequestKind::ChainSubmit:
            break;
        }
        followWhenIdle();
        updateControls();
        flushLiveParameters();
        followLiveParameters();
    }

    // UX-10, decision 1: the authoring context's render targets and the
    // engine's shaders, for the open parameters' combos.
    void consumeChoices(RequestKind kind, const QJsonObject &response) {
        if (kind == RequestKind::Context) {
            render_target_names.clear();
            for (const auto &value : response.value("render_targets").toArray()) {
                const auto name = value.toObject().value("name").toString();
                if (!name.isEmpty() && !render_target_names.contains(name))
                    render_target_names << name;
            }
            if (live.form) live.form->setRenderTargets(render_target_names);
            return;
        }
        engine_shaders.clear();
        const auto list = response.contains("shaders") ? response.value("shaders").toArray()
                                                       : response.value("engine_shaders").toArray();
        for (const auto &value : list) {
            const auto shader = value.toObject();
            const auto reference = shader.value("reference").toString();
            if (!reference.isEmpty())
                engine_shaders.emplace_back(reference, shader.value("stage").toString());
        }
        if (live.form) live.form->setShaders(shaderChoices());
    }

    void receiveFailure(qint64 request_id,
                        const QString &message) {
        const auto found = pending.find(request_id);
        if (found == pending.end()) return;
        const auto kind = found->kind;
        pending.erase(found);
        // LP-4f2: an engine without get_graph_templates offers no loop
        // version; the panel is otherwise as it was.
        if (kind == RequestKind::Templates) {
            templates.clear();
            have_templates = false;
            updateControls();
            return;
        }
        // UX-10: without the lists the combos offer what is written.
        if (kind == RequestKind::Context || kind == RequestKind::Shaders) return;
        // UX-12b: a read or an edit of the chain failed in transport; what
        // was added stays (Undo takes it back), and the line says why.
        if (kind == RequestKind::ChainContext || kind == RequestKind::ChainGraph ||
            kind == RequestKind::ChainSubmit) {
            const bool asked =
                chain && (chain->ask_only || chain->step == TargetsEdit::Step::ShowCheck);
            chain.reset();
            if (!asked) showError(message);
            updateControls();
            return;
        }
        if (kind == RequestKind::Result) {
            result_poll->stop();
            active_ticket.clear();
        }
        showError(message);
        updateControls();
        flushLiveParameters();
    }

    void consumeCanEdit(const QJsonObject &response) {
        can_edit = response.value(
            QStringLiteral("can_edit")).toBool(false);
        if (!can_edit) {
            QStringList reasons;
            for (const auto value : response.value(
                     QStringLiteral("reasons")).toArray()) {
                reasons.push_back(value.toString());
            }
            showError(owner.tr("Editing is unavailable: %1")
                          .arg(reasons.join(
                              QStringLiteral(", "))));
        }
    }

    void consumeCurrent(const QJsonObject &response) {
        const auto digest = response.value(
            QStringLiteral("source_digest")).toObject();
        source_digest = digest.value(
            QStringLiteral("hex")).toString();
        if (digest_hub) digest_hub->publish(source_digest.toStdString(), &owner);
        root_reference = response.value(
            QStringLiteral("source_reference")).toString();
        current->clear();
        for (const auto &value : response.value("feature_instances").toArray()) {
                const auto instance = value.toObject();
                const auto reference = instance.value("reference").toString();
                auto *item = new QListWidgetItem(reference, current);
                item->setData(ReferenceRole, reference);
                item->setData(InstanceRole, instance);
                item->setToolTip(instance.value("reason").toString());
                if (instance.value("scope").toString() == "preset")
                    item->setText(owner.tr("%1 (preset)").arg(reference));
        }
        have_current = true;
        if (have_catalog) applyCatalogNamesToCurrent();
        resumeRetry();
        followLiveParameters();
    }

    void receiveCatalogResult(const QByteArray &result_json) {
        QString error;
        const auto decoded = decodeObject(
            result_json, &error);
        if (!decoded) {
            showError(error);
            updateControls();
            return;
        }
        consumeCatalog(*decoded);
        updateControls();
    }

    void consumeCatalog(const QJsonObject &response) {
        catalog->clear();
        catalog_entries.clear();
        for (const auto value : response.value(
                 QStringLiteral("features")).toArray()) {
            const auto feature = value.toObject();
            const auto name = feature.value(
                QStringLiteral("name")).toString();
            const auto reference = feature.value(
                QStringLiteral("reference")).toString();
            const bool available = feature.value(
                QStringLiteral("available")).toBool(false);
            const auto unavailable_reason = feature.value(
                QStringLiteral("unavailable_reason")).toString();
            auto *item = new QTreeWidgetItem(catalog);
            item->setText(0, name);
            item->setText(1,
                          available
                              ? owner.tr("Available")
                              : unavailable_reason);
            item->setData(0, ReferenceRole, reference);
            item->setData(0, FeatureNameRole, name);
            item->setData(0, AvailableRole, available);
            item->setData(0, UnavailableReasonRole,
                          unavailable_reason);
            catalog_entries[reference.toStdString()] = jsonOf(feature);
            if (!available) {
                item->setToolTip(0, unavailable_reason);
                item->setToolTip(1, unavailable_reason);
            }
        }
        have_catalog = true;
        if (have_current) applyCatalogNamesToCurrent();
    }

    void applyCatalogNamesToCurrent() {
        QHash<QString, QString> names;
        for (int i = 0; i < catalog->topLevelItemCount();
             ++i) {
            const auto *item = catalog->topLevelItem(i);
            names.insert(
                item->data(0, ReferenceRole).toString(),
                item->data(0, FeatureNameRole).toString());
        }
        for (int i = 0; i < current->count(); ++i) {
            auto *item = current->item(i);
            const auto reference =
                item->data(ReferenceRole).toString();
            const auto name = names.value(reference);
            item->setText(name.isEmpty() ? reference : name);
            const auto instance = item->data(InstanceRole).toJsonObject();
            if (instance.value("scope").toString() == "preset") item->setText(owner.tr("%1 (preset)").arg(item->text()));
            item->setToolTip(reference + "\n" + instance.value("source_reference").toString() + "\n" + instance.value("reason").toString());
        }
        if (pending.isEmpty() && active_ticket.isEmpty() && !notice_sticky) {
            showInformation(owner.tr(
                "Select one catalog entry to add, or one current feature to remove."));
        }
    }

    // --- Q-13.
    bool submitting() const {
        for (auto it = pending.cbegin(); it != pending.cend(); ++it)
            if (it->kind == RequestKind::Submit) return true;
        return false;
    }

    // Another panel read or committed the document at `digest`.
    void documentMoved(const std::string &digest) {
        if (source_digest.isEmpty() ||
            source_digest == QString::fromStdString(digest))
            return;
        if (!active_ticket.isEmpty() || submitting()) {
            refresh_when_idle = true;
            return;
        }
        requestRefresh();
    }

    void followWhenIdle() {
        if (!refresh_when_idle || !active_ticket.isEmpty() || submitting() ||
            retry_pending)
            return;
        refresh_when_idle = false;
        requestRefresh();
    }

    // The engine refused the edit because the document moved since this
    // panel read it: read it again and send the edit once more against the
    // new revision. The second refusal says so and leaves it to Refresh.
    bool refusedForMovedDocument(const QJsonObject &response) {
        if (response.value(QStringLiteral("error")).toObject()
                .value(QStringLiteral("code")).toString() !=
            QStringLiteral("external_modification") ||
            !submitted_staged)
            return false;
        if (submitted_retried) {
            submitted_staged.reset();
            showError(owner.tr("The document changed outside this panel. "
                               "Press Refresh to read it again. (%1)")
                          .arg(responseError(response)));
            return true;
        }
        retry_pending = true;
        showInformation(owner.tr("The document changed since this panel read "
                                 "it; reading it again..."));
        requestRefresh();
        return true;
    }

    void resumeRetry() {
        if (!retry_pending || !have_current) return;
        retry_pending = false;
        auto again = std::exchange(submitted_staged, std::nullopt);
        if (!again) return;
        // The instance edits carry the revision they were staged against;
        // this is the one they are now built on.
        again->base_digest = source_digest;
        staged = std::move(again);
        retrying = true;
        submit();
        if (std::exchange(retrying, false))
            showError(owner.tr("The document changed outside this panel. "
                               "Press Refresh to read it again."));
    }

    void consumeSubmit(const QJsonObject &response) {
        if (response.value(QStringLiteral("status")).toString() !=
            QStringLiteral("accepted")) {
            if (refusedForMovedDocument(response)) return;
            const bool graph_template = submittingGraphTemplate();
            const auto refused = std::exchange(submitted_staged, std::nullopt);
            // It was sent at once, not staged for Apply: nothing stays pending.
            // UX-10: nor does a parameter change, which is sent at once too.
            if (graph_template || (staged && staged->op == QStringLiteral("set_parameters")))
                staged.reset();
            showError(graph_template ? graphTemplateError(response) : responseError(response));
            // UX-12b: and the way out the refusal names.
            if (!graph_template) offerFixes(response, refused, refused ? refused->display_name : QString{});
            return;
        }
        // LP-4f2: what it made is named here, not in the commit.
        if (submittingGraphTemplate())
            if (const auto *info = templateByReference(submitted_staged->reference))
                applied_loop = graphTemplateAppliedLoop(*info, toJson(response));
        active_ticket = response.value(
            QStringLiteral("ticket")).toString();
        if (active_ticket.isEmpty()) {
            showError(owner.tr(
                "The engine accepted the edit without returning a ticket."));
            return;
        }
        staged.reset();
        showInformation(owner.tr(
            "Edit accepted; waiting for the frame-boundary result..."));
        result_poll->start();
        requestResult();
    }

    void consumeResult(const QJsonObject &response) {
        const auto status = response.value(
            QStringLiteral("status")).toString();
        if (status == QStringLiteral("pending")) return;
        result_poll->stop();
        active_ticket.clear();
        // UX-12b: a ticket of a feature added with its targets, or of a fix.
        if (chain) {
            chainResult(response);
            return;
        }
        const bool graph_template = submittingGraphTemplate();
        const auto template_reference =
            graph_template ? submitted_staged->reference : QString{};
        if (status != QStringLiteral("committed") ||
            !response.value(QStringLiteral("committed"))
                 .toBool(false)) {
            if (refusedForMovedDocument(response)) return;
            const auto refused = std::exchange(submitted_staged, std::nullopt);
            applied_loop.clear();
            showError(graph_template ? graphTemplateError(response) : responseError(response));
            // UX-12b: and the way out the refusal names.
            if (!graph_template) offerFixes(response, refused, refused ? refused->display_name : QString{});
            return;
        }
        // UX-12d, decision 4: a feature added is looked at for a target it
        // draws that nothing reads.
        const auto added = submitted_staged && submitted_staged->op == QStringLiteral("add")
                               ? std::optional<std::pair<QString, QString>>{std::pair{
                                     submitted_staged->reference, submitted_staged->display_name}}
                               : std::nullopt;
        submitted_staged.reset();
        // Q-13: every other panel learns the revision this commit made.
        if (digest_hub)
            digest_hub->publish(response.value(QStringLiteral("source_digest"))
                                    .toObject()
                                    .value(QStringLiteral("hex"))
                                    .toString()
                                    .toStdString(),
                                &owner);
        // RSX, decision 1: written, but run only after a restart -- the
        // window restarts the engine and the notice says so instead of
        // "Applied without restarting". The list is read from the engine that
        // comes back.
        const auto completion = completeEdit(response, restartAction());
        if (completion.restart_required) {
            applied_loop.clear();
            return;
        }
        const auto generation = response.value(
            QStringLiteral("published_generation"))
                                    .toVariant()
                                    .toULongLong();
        if (graph_template) {
            // LP-4f2: the feature's place is the graph's now: the Graph tab
            // comes forward with the loop it added, and the other panels
            // read the document the commit wrote.
            requestRefresh();
            if (const auto *info = templateByReference(template_reference)) {
                // Both sentences on the line -- the second is the way back --
                // and kept there while the list is read again.
                const auto line = QString::fromStdString(graphTemplateAppliedLine(*info));
                notice->setStyleSheet(QStringLiteral("color: #388e3c;"));
                setPanelNotice(notice, line, line);
                notice_sticky = true;
            }
            emit owner.renderConfigCommitted();
            const auto loop = std::exchange(applied_loop, {});
            if (!loop.empty() && loop_applied_action)
                loop_applied_action(QString::fromStdString(loop));
            return;
        }
        // RSX: said only for a commit that ran live, and kept while the list
        // is read again (the refresh's own line used to replace it at once).
        requestRefresh();
        const auto applied = owner.tr("Applied without restarting (runtime generation %1).").arg(generation);
        showSuccess(applied);
        notice_sticky = true;
        if (added) checkShowTarget(added->first, added->second, applied);
    }

    // --- LP-4f2: Replace with loop version.

    static nlohmann::json toJson(const QJsonObject &object) {
        return nlohmann::json::parse(
            QJsonDocument{object}.toJson(QJsonDocument::Compact).toStdString(), nullptr, false);
    }

    bool submittingGraphTemplate() const {
        return submitted_staged && submitted_staged->op == QStringLiteral("apply_graph_template");
    }

    QString graphTemplateError(const QJsonObject &response) const {
        return QString::fromStdString(
            graphTemplateRejection(toJson(response).value("error", nlohmann::json::object())));
    }

    const GraphTemplateInfo *templateByReference(const QString &reference) const {
        const auto wanted = reference.toStdString();
        const auto found = std::ranges::find(templates, wanted, &GraphTemplateInfo::reference);
        return found == templates.end() ? nullptr : &*found;
    }

    const GraphTemplateInfo *templateFor(const QListWidgetItem *item) const {
        if (item == nullptr || !have_templates) return nullptr;
        return graphTemplateForFeature(templates, item->data(ReferenceRole).toString().toStdString());
    }

    // Why the row's loop version cannot be taken in now; empty when it can.
    QString loopVersionRefusal(const QListWidgetItem *item) const {
        if (item == nullptr) return owner.tr("Select the feature to replace first.");
        const auto instance = item->data(InstanceRole).toJsonObject();
        if (const auto refusal = graphTemplateRefusal(
                item->data(ReferenceRole).toString().toStdString(),
                instance.value("scope").toString("root").toStdString(),
                instance.value("source_reference").toString().toStdString());
            !refusal.empty())
            return QString::fromStdString(refusal);
        if (!can_edit) return owner.tr("Editing is unavailable.");
        if (staged) return owner.tr("Apply the pending edit first.");
        return {};
    }

    QListWidgetItem *itemForFeature(const QString &reference) const {
        // The root's own entry first: that is the one a person reads first.
        QListWidgetItem *found = nullptr;
        for (int index = 0; index < current->count(); ++index) {
            auto *item = current->item(index);
            if (item->data(ReferenceRole).toString() != reference) continue;
            if (item->data(InstanceRole).toJsonObject().value("scope").toString("root") ==
                QStringLiteral("root"))
                return item;
            if (found == nullptr) found = item;
        }
        return found;
    }

    // Confirm, then send. The request is built when it is sent, from the
    // list as it is then.
    void replaceWithLoopVersion(QListWidgetItem *item) {
        const auto *info = templateFor(item);
        if (info == nullptr) {
            showError(owner.tr("The engine ships no loop version of %1.")
                          .arg(item != nullptr ? item->text() : QString{}));
            return;
        }
        if (!active_ticket.isEmpty() || submitting()) {
            showInformation(owner.tr("Wait for the current change to finish."));
            return;
        }
        if (const auto refusal = loopVersionRefusal(item); !refusal.isEmpty()) {
            showError(refusal);
            return;
        }
        if (replace_prompt) {
            replace_prompt->raise();
            return;
        }
        auto *prompt = new QMessageBox(QMessageBox::Question,
                                       owner.tr("Replace with loop version"),
                                       QString::fromStdString(graphTemplateConfirmation(*info)),
                                       QMessageBox::NoButton, &owner);
        prompt->setObjectName(QStringLiteral("pelican.renderFeatures.replaceConfirm"));
        // Named, so a scenario answers the way a person does.
        auto *accept = prompt->addButton(owner.tr("Replace"), QMessageBox::AcceptRole);
        accept->setObjectName(QStringLiteral("pelican.renderFeatures.replaceConfirm.ok"));
        auto *cancel = prompt->addButton(QMessageBox::Cancel);
        cancel->setObjectName(QStringLiteral("pelican.renderFeatures.replaceConfirm.cancel"));
        prompt->setDefaultButton(cancel);
        prompt->setEscapeButton(cancel);
        prompt->setAttribute(Qt::WA_DeleteOnClose);
        replace_prompt = prompt;
        const auto feature = item->data(ReferenceRole).toString();
        const auto scope =
            item->data(InstanceRole).toJsonObject().value("scope").toString("root");
        const auto template_reference = QString::fromStdString(info->reference);
        QObject::connect(prompt, &QMessageBox::buttonClicked, &owner,
                         [this, accept, feature, scope, template_reference](QAbstractButton *button) {
                             if (button != accept) {
                                 showInformation(owner.tr("%1 is left as it is.")
                                                     .arg(QString::fromStdString(
                                                         graphTemplateFeatureName(
                                                             feature.toStdString()))));
                                 return;
                             }
                             staged = StagedOperation{
                                 .op = QStringLiteral("apply_graph_template"),
                                 .reference = template_reference,
                                 .display_name = feature,
                                 .scope = scope,
                                 .feature = feature,
                             };
                             submit();
                             if (!submitting()) staged.reset();
                             updateControls();
                         });
        prompt->open();
    }

    void submitGraphTemplate() {
        const auto *info = templateByReference(staged->reference);
        if (info == nullptr) {
            staged.reset();
            showError(owner.tr("The engine no longer lists that graph; press Refresh."));
            return;
        }
        // The preset's digest goes when the project's preset lists the
        // feature, read from the list as it is now (a retry reads it again).
        QString preset_digest;
        if (staged->scope == QStringLiteral("preset"))
            for (int index = 0; index < current->count(); ++index) {
                const auto instance = current->item(index)->data(InstanceRole).toJsonObject();
                if (current->item(index)->data(ReferenceRole).toString() == staged->feature &&
                    instance.value("scope").toString() == QStringLiteral("preset")) {
                    preset_digest =
                        instance.value("source_digest").toObject().value("hex").toString();
                    break;
                }
            }
        QJsonObject params;
        try {
            params = exactJsonRpcObject(graphTemplateApplyParams(
                *info, source_digest.toStdString(), preset_digest.toStdString()));
        } catch (const std::exception &error) {
            staged.reset();
            showError(QString::fromUtf8(error.what()));
            return;
        }
        if (request(RequestKind::Submit, QStringLiteral("apply_graph_template"), params) != 0) {
            submitted_staged = staged;
            submitted_retried = std::exchange(retrying, false);
            applied_loop.clear();
            showInformation(owner.tr("Replacing %1 with %2...")
                                .arg(QString::fromStdString(graphTemplateFeatureName(
                                         staged->feature.toStdString())),
                                     QString::fromStdString(info->title)));
            updateControls();
        }
    }

    void updateControls() {
        bool submit_pending = false;
        for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
            submit_pending = submit_pending ||
                             it->kind == RequestKind::Submit;
        }
        const bool idle = active_ticket.isEmpty() &&
                          !submit_pending && !chain;
        parameters->setEnabled(can_edit && idle && current->currentItem() &&
            current->currentItem()->data(InstanceRole).toJsonObject().value("editable").toBool());
        const auto instance = current->currentItem() ? current->currentItem()->data(InstanceRole).toJsonObject() : QJsonObject{};
        materialize->setEnabled(can_edit && idle && have_current && !staged && instance.value("materializable").toBool());
        materialize->setToolTip(staged ? owner.tr("Apply the pending edit first.") : instance.value("materialize_reason").toString());
        refresh->setEnabled(viewport.rpcReady() && idle);
        current->setEnabled(have_current && idle);
        catalog->setEnabled(have_catalog && idle);
        remove->setEnabled(
            can_edit && idle &&
            !current->selectedItems().isEmpty() && current->selectedItems().front()->data(InstanceRole).toJsonObject().value("removable").toBool());
        const auto catalog_selection =
            catalog->selectedItems();
        add->setEnabled(
            can_edit && idle &&
            !catalog_selection.isEmpty() &&
            catalog_selection.front()
                ->data(0, AvailableRole).toBool());
        apply->setEnabled(
            can_edit && idle && staged.has_value() &&
            !source_digest.isEmpty());
        // LP-4f2: offered on a row whose feature has a loop version, and on
        // no other.
        const auto *row = current->currentItem();
        const auto *info = templateFor(row);
        replace->setVisible(info != nullptr);
        const auto refusal = info != nullptr ? loopVersionRefusal(row) : QString{};
        const bool offered = info != nullptr && idle && have_current && refusal.isEmpty();
        replace->setEnabled(offered);
        replace_action->setEnabled(offered);
        const auto tip = info == nullptr ? QString{}
                         : !refusal.isEmpty()
                             ? refusal
                             : owner.tr("Take in %1 in its place: the same picture, drawn by "
                                        "graph nodes you can edit.")
                                   .arg(QString::fromStdString(info->title));
        replace->setToolTip(tip);
        replace_action->setToolTip(tip);
    }
};

RenderFeaturesWidget::RenderFeaturesWidget(
    EmbeddedViewport *viewport, QWidget *parent)
    : QWidget{parent} {
    if (viewport == nullptr) {
        throw std::invalid_argument(
            "RenderFeaturesWidget requires an embedded viewport");
    }
    impl_ = std::make_unique<Impl>(*this, *viewport);
}

RenderFeaturesWidget::~RenderFeaturesWidget() = default;

void RenderFeaturesWidget::editComputeTask(const QString &name) {
    editComputeTaskConnection(name, {});
}

void RenderFeaturesWidget::editComputeTaskConnection(const QString &name, const QStringList &outputs) {
    auto *dialog = new ComputeTaskDialog(impl_->viewport, this, name, [this] {
        impl_->requestRefresh();
        emit renderConfigCommitted();
    }, outputs, [this] { return impl_->root_reference; });
    dialog->setGraphNodeOpenAction(impl_->graph_node_open_action);
    dialog->setViewAxesProvider(impl_->task_view_axes);
    // Q-15: the Task Editor follows the other panels' commits too.
    dialog->setRenderConfigDigest(impl_->digest_hub);
    dialog->setEngineRestartAction(impl_->restartAction());
    impl_->adoptHistoryActions(dialog);
    dialog->show();
}

void RenderFeaturesWidget::editFrameNodeOrder(const QString &graph, const QStringList &names, quint64 generation) {
    auto *dialog = new FrameNodeOrderDialog(impl_->viewport, this, graph, names, generation, [this] {
        impl_->requestRefresh();
        emit renderConfigCommitted();
    });
    dialog->setEngineRestartAction(impl_->restartAction());
    impl_->adoptHistoryActions(dialog);
    dialog->show();
}

void RenderFeaturesWidget::receiveCatalogResult(
    const QByteArray &result_json) {
    impl_->receiveCatalogResult(result_json);
}

void RenderFeaturesWidget::setRenderConfigDigest(
    std::shared_ptr<RenderConfigDigest> digest) {
    impl_->digest_hub = std::move(digest);
    if (impl_->digest_hub)
        impl_->digest_hub->subscribe(this, [impl = impl_.get()](const std::string &moved) {
            impl->documentMoved(moved);
        });
}

void RenderFeaturesWidget::setRenderHistoryActions(const QList<QAction *> &actions) {
    impl_->history_actions = actions;
    addActions(actions);
}

void RenderFeaturesWidget::setGraphNodeOpenAction(GraphNodeOpenAction action) {
    impl_->graph_node_open_action = std::move(action);
}

void RenderFeaturesWidget::setTaskViewAxesProvider(TaskViewAxesProvider provider) {
    impl_->task_view_axes = std::move(provider);
}

void RenderFeaturesWidget::replaceWithLoopVersion(const QString &feature_reference) {
    auto *item = impl_->itemForFeature(feature_reference);
    if (item == nullptr) {
        impl_->showError(tr("The feature list does not hold %1; press Refresh.")
                             .arg(feature_reference));
        return;
    }
    impl_->current->setCurrentItem(item);
    impl_->replaceWithLoopVersion(item);
}

std::optional<QString> RenderFeaturesWidget::loopVersionOffered(
    const QString &feature_reference) const {
    auto *item = impl_->itemForFeature(feature_reference);
    if (item == nullptr || impl_->templateFor(item) == nullptr) return std::nullopt;
    return impl_->loopVersionRefusal(item);
}

void RenderFeaturesWidget::setEngineRestartAction(
    std::function<void(const QString &reason)> action) {
    impl_->engine_restart_action = std::move(action);
}

void RenderFeaturesWidget::engineRestartDeclined(const QString &message) {
    if (!std::exchange(impl_->restart_asked, false)) return;
    // The running engine stays; the list is read from it, and the line says
    // why nothing restarted.
    impl_->requestRefresh();
    impl_->showInformation(message);
    impl_->notice_sticky = true;
}

void RenderFeaturesWidget::setPlaceInsertionAnchorAction(
    std::function<bool(const QString &anchor)> action) {
    impl_->place_anchor_action = std::move(action);
}

void RenderFeaturesWidget::setLoopVersionAppliedAction(
    std::function<void(const QString &loop)> action) {
    impl_->loop_applied_action = std::move(action);
}

} // namespace PelicanStudio
