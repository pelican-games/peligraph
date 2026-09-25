#include "renderfeatureswidget.hpp"
#include "frameplanedit.hpp"
#include "panelhelp.hpp"
#include "renderconfigdigest.hpp"
#include "proceduralassetdialog.hpp"
#include "jsonrpcrequest.hpp"
#include "taskresourceconnections.hpp"
#include "taskconnectiongraphics.hpp"

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

class FrameNodeOrderDialog final : public QDialog {
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
  void submit(bool dependency_order) {
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
        notice->setText(responseError(*response));
    } else if (response->value("status").toString() != "pending") {
      poll.stop();
      ticket.clear();
      if (response->value("status").toString() == "committed" &&
          response->value("committed").toBool()) {
        committed();
        close();
        return;
      }
      notice->setText(responseError(*response));
    }
    controls();
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

class ComputeTaskDialog final : public QDialog {
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
  void initializeNewDraft(int index) {
    if (!new_drafts[index].root_digest.isEmpty())
      return;
    QJsonObject task{{"name", index == 0 ? "new_compute" : "new_ray_tracing"}};
    if (index == 0) {
      task.insert("shader", "project://shaders/compute");
      task.insert("dispatch", QJsonObject{{"groups", QJsonArray{1, 1, 1}}});
    } else {
      task.insert(
          "ray_tracing",
          QJsonObject{{"raygen", "project://shaders/raygen"},
                      {"miss", QJsonArray{"project://shaders/miss"}},
                      {"closesthit", QJsonArray{"project://shaders/hit"}}});
      task.insert("dispatch", QJsonObject{{"rays", QJsonArray{1, 1, 1}}});
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

private:
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
      loadDispatch();
      loadConnections();
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
      if (response->value("status").toString() == "committed" &&
          response->value("committed").toBool()) {
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
        committed();
        read();
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

class PresetEditDialog final : public QDialog {
public:
  enum class Mode { Fork, Region };
private:
  Mode mode;
  EmbeddedViewport &viewport;
  std::function<void()> on_committed;
  QLabel *notice;
  QLineEdit *destination;
  QComboBox *regions = nullptr;
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
      if (response->value("status").toString() == "committed" &&
          response->value("committed").toBool()) {
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
    layout->addWidget(new QLabel(mode == Mode::Fork ? tr("New project preset reference") : tr("Replacement provider (empty = authored nodes)"), this));
    destination =
        new QLineEdit(mode == Mode::Fork ? QStringLiteral("project://pipelines/custom.json") : QString{}, this);
    destination->setObjectName(
        mode == Mode::Fork ? QStringLiteral("pelican.presetFork.destination") : QStringLiteral("pelican.regionReplacement.provider"));
    layout->addWidget(destination);
    if (regions) connect(regions, &QComboBox::currentIndexChanged, this, [this] {
      destination->setText(regions->currentData().toJsonObject()["provider"].toString());
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
        const auto provider = destination->text().trimmed();
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

class CallableRecordsDialog final : public QDialog {
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
      if (response->value("committed").toBool() &&
          response->value("status").toString() == "committed")
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

class SceneAccelerationStructuresDialog final : public QDialog {
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
                if (status == QStringLiteral("committed") &&
                    response->value(QStringLiteral("committed")).toBool()) read();
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
    };

    RenderFeaturesWidget &owner;
    EmbeddedViewport &viewport;
    // Q-10: get_render_features' source_reference, the rendering document
    // itself. The Task Editor refuses a task declared there by name.
    QString root_reference;
    // Q-12: handed to each Task Editor it opens.
    RenderFeaturesWidget::GraphNodeOpenAction graph_node_open_action;
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
    QHash<qint64, PendingRequest> pending;
    quint64 refresh_generation = 0;
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
            dialog->show();
        });
        auto *scene_as = new QPushButton(owner.tr("Scene AS"), &owner);
        auto *callable_records = new QPushButton(owner.tr("Records"), &owner);
        actions->addWidget(callable_records);
        QObject::connect(callable_records, &QPushButton::clicked, &owner, [this] {
            auto *dialog = new CallableRecordsDialog(viewport, &owner);
            dialog->show();
        });
        scene_as->setObjectName(QStringLiteral("pelican.renderFeatures.sceneAccelerationStructures"));
        actions->addWidget(scene_as);
        QObject::connect(scene_as, &QPushButton::clicked, &owner, [this] {
            auto *dialog = new SceneAccelerationStructuresDialog(viewport, &owner);
            dialog->show();
        });
        actions->addStretch();
        actions->addWidget(apply);
        layout->addLayout(actions);

        result_poll = new QTimer(&owner);
        result_poll->setInterval(40);

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

    void editParameters() {
        const auto *item = current->currentItem();
        if (!item) return;
        const auto instance = item->data(InstanceRole).toJsonObject();
        const auto display_name = item->text();
        const auto base_digest = source_digest;
        if (!instance.value("editable").toBool()) {
            showInformation(instance.value("reason").toString());
            return;
        }
        QDialog dialog(&owner);
        dialog.setWindowTitle(owner.tr("Feature parameters: %1").arg(item->text()));
        dialog.setObjectName("pelican.featureParameters");
        dialog.resize(560, 480);
        auto *layout = new QVBoxLayout(&dialog);
        auto *hint = new QLabel(owner.tr("Overrides"), &dialog);
        hint->setToolTip(owner.tr("Enable an override to change its value. Uncheck it to use the feature default. Apply in Render features to save."));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        auto *scroll = new QScrollArea(&dialog);
        scroll->setWidgetResizable(true);
        auto *body = new QWidget(scroll);
        auto *form = new QFormLayout(body);
        scroll->setWidget(body);
        layout->addWidget(scroll);
        struct Field { QString name, type; QCheckBox *override_value; QWidget *value; QJsonArray range; };
        std::vector<Field> fields;
        const auto schema = instance.value("parameter_schema").toObject();
        const auto authored = staged && staged->op == "set_parameters" && staged->base_digest == base_digest &&
            staged->scope == instance.value("scope").toString("root") &&
            staged->preset_digest == instance.value("source_digest").toObject().value("hex").toString() &&
            staged->index == instance.value("index").toInt(-1) ? staged->parameters : instance.value("parameters").toObject();
        for (const auto *category : {"scalars", "render_targets", "shader_assets"}) {
            for (const auto &encoded : schema.value(category).toArray()) {
                const auto declaration = encoded.toObject();
                const auto name = declaration.value("name").toString();
                const auto type = QString::fromLatin1(category) == "scalars" ? declaration.value("type").toString() : QStringLiteral("string");
                const auto value = authored.contains(name) ? authored.value(name) : declaration.value("default");
                auto *enabled = new QCheckBox(name, &dialog);
                enabled->setChecked(authored.contains(name));
                QWidget *editor;
                if (type == "bool") {
                    auto *check = new QCheckBox(&dialog);
                    check->setChecked(value.toBool());
                    editor = check;
                } else {
                    auto *line = new QLineEdit(&dialog);
                    if (value.isString()) line->setText(value.toString());
                    else if (value.isDouble()) line->setText(QString::number(value.toDouble(), 'g', 17));
                    line->setPlaceholderText(type == "string" ? owner.tr("Target name or shader reference") : type);
                    editor = line;
                }
                const auto range = declaration.value("range").toArray();
                editor->setToolTip(range.size() == 2 ? owner.tr("Range: %1 to %2").arg(range[0].toDouble()).arg(range[1].toDouble()) : declaration.value("stage").toString());
                editor->setObjectName("pelican.featureParameters." + name);
                editor->setEnabled(enabled->isChecked());
                QObject::connect(enabled, &QCheckBox::toggled, editor, &QWidget::setEnabled);
                form->addRow(enabled, editor);
                fields.push_back({name, type, enabled, editor, range});
            }
        }
        auto *error = new QLabel(&dialog);
        error->setWordWrap(true);
        layout->addWidget(error);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QJsonObject edited;
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            edited = authored;
            for (const auto &field : fields) {
                if (!field.override_value->isChecked()) { edited.remove(field.name); continue; }
                if (field.type == "bool") edited[field.name] = static_cast<QCheckBox *>(field.value)->isChecked();
                else {
                    const auto text = static_cast<QLineEdit *>(field.value)->text().trimmed();
                    if (field.type == "string") { edited[field.name] = text; continue; }
                    bool valid = false;
                    const double value = text.toDouble(&valid);
                    if (!valid || !std::isfinite(value) || (field.type == "int" && std::trunc(value) != value) ||
                        (field.range.size() == 2 && (value < field.range[0].toDouble() || value > field.range[1].toDouble()))) {
                        error->setText(owner.tr("Enter a valid %1 value for %2 within its declared range.").arg(field.type, field.name));
                        return;
                    }
                    edited[field.name] = value;
                }
            }
            dialog.accept();
        });
        if (dialog.exec() != QDialog::Accepted) return;
        if (source_digest != base_digest || !can_edit) {
            showError(owner.tr("The feature list changed while editing. Reopen its parameters."));
            return;
        }
        staged = StagedOperation{.op = "set_parameters", .reference = instance.value("reference").toString(),
            .display_name = display_name, .index = instance.value("index").toInt(-1), .parameters = edited, .base_digest = base_digest,
            .scope = instance.value("scope").toString("root"), .preset_digest = instance.value("source_digest").toObject().value("hex").toString()};
        showInformation(owner.tr("Pending parameter changes: %1").arg(display_name));
        updateControls();
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
        }
        followWhenIdle();
        updateControls();
    }

    void receiveFailure(qint64 request_id,
                        const QString &message) {
        const auto found = pending.find(request_id);
        if (found == pending.end()) return;
        const auto kind = found->kind;
        pending.erase(found);
        if (kind == RequestKind::Result) {
            result_poll->stop();
            active_ticket.clear();
        }
        showError(message);
        updateControls();
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
        if (pending.isEmpty() && active_ticket.isEmpty()) {
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
            submitted_staged.reset();
            showError(responseError(response));
            return;
        }
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
        if (status != QStringLiteral("committed") ||
            !response.value(QStringLiteral("committed"))
                 .toBool(false)) {
            if (refusedForMovedDocument(response)) return;
            submitted_staged.reset();
            showError(responseError(response));
            return;
        }
        submitted_staged.reset();
        // Q-13: every other panel learns the revision this commit made.
        if (digest_hub)
            digest_hub->publish(response.value(QStringLiteral("source_digest"))
                                    .toObject()
                                    .value(QStringLiteral("hex"))
                                    .toString()
                                    .toStdString(),
                                &owner);
        const auto generation = response.value(
            QStringLiteral("published_generation"))
                                    .toVariant()
                                    .toULongLong();
        showSuccess(owner.tr(
            "Applied without restarting (runtime generation %1).")
                        .arg(generation));
        requestRefresh();
    }

    void updateControls() {
        bool submit_pending = false;
        for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
            submit_pending = submit_pending ||
                             it->kind == RequestKind::Submit;
        }
        const bool idle = active_ticket.isEmpty() &&
                          !submit_pending;
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
    // Q-15: the Task Editor follows the other panels' commits too.
    dialog->setRenderConfigDigest(impl_->digest_hub);
    dialog->show();
}

void RenderFeaturesWidget::editFrameNodeOrder(const QString &graph, const QStringList &names, quint64 generation) {
    auto *dialog = new FrameNodeOrderDialog(impl_->viewport, this, graph, names, generation, [this] {
        impl_->requestRefresh();
        emit renderConfigCommitted();
    });
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

void RenderFeaturesWidget::setGraphNodeOpenAction(GraphNodeOpenAction action) {
    impl_->graph_node_open_action = std::move(action);
}

} // namespace PelicanStudio
