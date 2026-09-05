#include "renderfeatureswidget.hpp"

#include "../viewport/embeddedviewport.hpp"

#include <QAbstractItemView>
#include <QDialog>
#include <QComboBox>
#include <QSpinBox>
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
  QComboBox *task_box;
  QPlainTextEdit *editor;
  QLabel *source;
  QLabel *notice;
  QPushButton *apply;
  QPushButton *refresh;
  QPushButton *new_compute;
  QPushButton *new_ray_tracing;
  QPushButton *remove;
  QLineEdit *destination;
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
    if (index == 1) {
      for (const auto &entry : tasks) {
        const auto declaration =
            entry.toObject().value("declaration").toObject();
        if (!declaration.contains("ray_tracing"))
          continue;
        task = declaration;
        task.insert("name", "new_ray_tracing");
        break;
      }
    }
    new_drafts[index] = {
        QString::fromUtf8(QJsonDocument(task).toJson(QJsonDocument::Indented)),
        root_digest,
        {}};
    destinations[index] = index == 0
                              ? "project://features/new_compute.json"
                              : "project://features/new_ray_tracing.json";
  }
  QJsonObject selected() const {
    const int index = task_box->currentIndex();
    return index < 0 || index >= tasks.size() ? QJsonObject{}
                                              : tasks[index].toObject();
  }
  void controls() {
    const bool idle = request_id == 0 && ticket.isEmpty();
    task_box->setEnabled(idle);
    const bool creating = creationIndex() >= 0;
    const bool editable = creating || selected().value("editable").toBool();
    destination->setVisible(creating);
    destination->setEnabled(idle);
    apply->setText(creating ? tr("Create task") : tr("Apply task"));
    new_compute->setEnabled(idle && viewport.rpcReady() &&
                            !root_digest.isEmpty());
    new_ray_tracing->setEnabled(new_compute->isEnabled());
    remove->setEnabled(idle && viewport.rpcReady() && !creating && editable &&
                       !root_digest.isEmpty());
    editor->setReadOnly(!idle || !editable);
    apply->setEnabled(idle && viewport.rpcReady() && editable &&
                      !root_digest.isEmpty());
    refresh->setEnabled(viewport.rpcReady() && request_id == 0);
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
      controls();
      return;
    }
    editor->setPlainText(drafts.value(task.value("name").toString()).text);
    source->setText(tr("%1 (%2)\nSource: %3")
                        .arg(task.value("name").toString(),
                             task.value("kind").toString(),
                             task.value("source_reference").toString()));
    notice->setText(
        task.value("editable").toBool()
            ? tr("Edit the task declaration. Keep its name unchanged.")
            : task.value("read_only_reason")
                  .toString(tr("This task source is read-only.")));
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
    submitted_creation = creationIndex();
    submitted_name = document.object().value("name").toString();
    if (submitted_creation >= 0) {
      request(
          Request::Submit, QStringLiteral("add_compute_task"),
          {{"base_source_digest", new_drafts[submitted_creation].root_digest},
           {"destination_reference", destination->text()},
           {"task", document.object()}});
      return;
    }
    if (document.object().value("name") != task.value("name")) {
      notice->setText(tr("The task name cannot be changed."));
      return;
    }
    const auto draft = drafts.value(task.value("name").toString());
    request(Request::Submit, QStringLiteral("edit_compute_task"),
            {{"base_source_digest", draft.root_digest},
             {"base_task_source_digest", draft.source_digest},
             {"task_name", task.value("name")},
             {"task", document.object()}});
  }
  void removeTask() {
    const auto task = selected();
    if (!task.value("editable").toBool())
      return;
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
        discard_new_drafts_on_read = false;
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
        notice->setText(
            tr("The engine returned an invalid task authoring context."));
      } else {
        const auto previous =
            requested_task.isEmpty() ? task_box->currentText() : requested_task;
        root_digest = digest;
        tasks = response->value("tasks").toArray();
        drafts.clear();
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
            drafts.insert(
                name,
                retained_drafts.value(
                    name,
                    TaskDraft{
                        QString::fromUtf8(
                            QJsonDocument(task.value("declaration").toObject())
                                .toJson(QJsonDocument::Indented)),
                        root_digest,
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
        retained_drafts.clear();
        showTask();
        if (!requested_task.isEmpty() && task_box->findText(requested_task) < 0)
          notice->setText(tr("Select the authored task corresponding to %1.")
                              .arg(requested_task));
        requested_task.clear();
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
      if (response->value("status").toString() == "committed" &&
          response->value("committed").toBool()) {
        for (const auto &entry : tasks) {
          const auto task = entry.toObject();
          const auto name = task.value("name").toString();
          const auto original = QString::fromUtf8(
              QJsonDocument(task.value("declaration").toObject())
                  .toJson(QJsonDocument::Indented));
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
      } else
        notice->setText(responseError(*response));
    }
    controls();
  }

public:
  ComputeTaskDialog(EmbeddedViewport &embedded, QWidget *parent, QString task,
                    std::function<void()> on_committed)
      : QDialog(parent), viewport(embedded), committed(std::move(on_committed)),
        requested_task(std::move(task)) {
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
    notice = new QLabel(this);
    notice->setWordWrap(true);
    layout->addWidget(notice);
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
                discard_new_drafts_on_read = false;
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

class PresetForkDialog final : public QDialog {
  EmbeddedViewport &viewport;
  std::function<void()> on_committed;
  QLabel *notice;
  QLineEdit *destination;
  QPushButton *apply;
  QPushButton *refresh;
  QTimer poll;
  QString root_digest, preset_digest, ticket, committed_reference;
  qint64 request_id = 0;
  enum class Request { Context, Fork, Result } request_kind = Request::Context;
  void controls() {
    const bool idle = request_id == 0 && ticket.isEmpty();
    destination->setEnabled(idle);
    apply->setEnabled(idle && viewport.rpcReady() && !root_digest.isEmpty() &&
                      !preset_digest.isEmpty());
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
        notice->setText(
            tr("Preset copied and selected: %1").arg(committed_reference));
        on_committed();
      } else
        notice->setText(responseError(*response));
    }
    controls();
  }

public:
  PresetForkDialog(EmbeddedViewport &embedded, QWidget *parent,
                   std::function<void()> committed)
      : QDialog(parent), viewport(embedded),
        on_committed(std::move(committed)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName(QStringLiteral("pelican.presetFork"));
    setWindowTitle(tr("Fork preset into project"));
    resize(560, 210);
    auto *layout = new QVBoxLayout(this);
    notice = new QLabel(tr("Loading preset..."), this);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    layout->addWidget(new QLabel(tr("New project preset reference"), this));
    destination =
        new QLineEdit(QStringLiteral("project://pipelines/custom.json"), this);
    destination->setObjectName(
        QStringLiteral("pelican.presetFork.destination"));
    layout->addWidget(destination);
    auto *buttons = new QHBoxLayout;
    refresh = new QPushButton(tr("Refresh source"), this);
    apply = new QPushButton(tr("Fork preset"), this);
    apply->setObjectName(QStringLiteral("pelican.presetFork.apply"));
    buttons->addWidget(refresh);
    buttons->addWidget(apply);
    layout->addLayout(buttons);
    connect(refresh, &QPushButton::clicked, this, [this] { read(); });
    connect(apply, &QPushButton::clicked, this, [this] {
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
    auto *description = new QLabel(tr("Edit saved initial values. Live changes "
                                      "made by game logic are not shown here."),
                                   this);
    description->setWordWrap(true);
    layout->addWidget(description);
    task_box = new QComboBox(this);
    record_box = new QComboBox(this);
    task_box->setObjectName(QStringLiteral("pelican.callableRecords.task"));
    record_box->setObjectName(QStringLiteral("pelican.callableRecords.record"));
    layout->addWidget(task_box);
    layout->addWidget(record_box);
    fields = new QTableWidget(0, 2, this);
    fields->setHorizontalHeaderLabels({tr("Field / type"), tr("Value")});
    fields->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(fields);
    notice = new QLabel(this);
    notice->setWordWrap(true);
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
        request(Request::Submit, QStringLiteral("edit_scene_acceleration_structures"),
            {{QStringLiteral("base_source_digest"), digest},
             {QStringLiteral("definitions"), definitions},
             {QStringLiteral("task_connections"), task_connections}});
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
        auto *description = new QLabel(tr("Create named scene subsets using instance mask bits. The default scene includes all geometry."), this);
        description->setWordWrap(true);
        layout->addWidget(description);
        table = new QTableWidget(0, 2, this);
        table->setObjectName(QStringLiteral("pelican.sceneAccelerationStructures.table"));
        table->setHorizontalHeaderLabels({tr("Name"), tr("Instance mask (0-255)")});
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
    };

    RenderFeaturesWidget &owner;
    EmbeddedViewport &viewport;
    QListWidget *current = nullptr;
    QTreeWidget *catalog = nullptr;
    QPushButton *add = nullptr;
    QPushButton *remove = nullptr;
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

    Impl(RenderFeaturesWidget &widget,
         EmbeddedViewport &embedded_viewport)
        : owner{widget}, viewport{embedded_viewport} {
        owner.setObjectName(
            QStringLiteral("pelican.renderFeatures"));
        auto *layout = new QVBoxLayout(&owner);

        auto *description = new QLabel(
            owner.tr("Edit the authored render config features. Changes are preflighted and applied at a frame boundary."),
            &owner);
        description->setWordWrap(true);
        layout->addWidget(description);

        auto *current_label = new QLabel(
            owner.tr("Current authored features"), &owner);
        layout->addWidget(current_label);
        current = new QListWidget(&owner);
        current->setObjectName(
            QStringLiteral("pelican.renderFeatures.current"));
        current->setSelectionMode(
            QAbstractItemView::SingleSelection);
        current->setEditTriggers(
            QAbstractItemView::NoEditTriggers);
        layout->addWidget(current, 1);

        auto *remove_row = new QHBoxLayout;
        remove = new QPushButton(owner.tr("Remove selected"), &owner);
        remove->setObjectName(
            QStringLiteral("pelican.renderFeatures.remove"));
        remove_row->addWidget(remove);
        remove_row->addStretch();
        layout->addLayout(remove_row);

        auto *catalog_label = new QLabel(
            owner.tr("Engine feature catalog"), &owner);
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
        add = new QPushButton(owner.tr("Add selected"), &owner);
        add->setObjectName(
            QStringLiteral("pelican.renderFeatures.add"));
        add_row->addWidget(add);
        add_row->addStretch();
        layout->addLayout(add_row);

        notice = new QLabel(&owner);
        notice->setObjectName(
            QStringLiteral("pelican.renderFeatures.notice"));
        notice->setWordWrap(true);
        layout->addWidget(notice);

        auto *actions = new QHBoxLayout;
        refresh = new QPushButton(owner.tr("Refresh"), &owner);
        refresh->setObjectName(
            QStringLiteral("pelican.renderFeatures.refresh"));
        apply = new QPushButton(owner.tr("Apply"), &owner);
        apply->setObjectName(
            QStringLiteral("pelican.renderFeatures.apply"));
        actions->addWidget(refresh);
        auto *tasks = new QPushButton(owner.tr("Tasks..."), &owner);
        tasks->setObjectName(QStringLiteral("pelican.renderFeatures.tasks"));
        actions->addWidget(tasks);
        QObject::connect(tasks, &QPushButton::clicked, &owner, [this] { owner.editComputeTask({}); });
        auto *fork_preset = new QPushButton(owner.tr("Fork preset..."), &owner);
        fork_preset->setObjectName(QStringLiteral("pelican.renderFeatures.forkPreset"));
        actions->addWidget(fork_preset);
        QObject::connect(fork_preset, &QPushButton::clicked, &owner, [this] {
            auto *dialog = new PresetForkDialog(viewport, &owner, [this] { requestRefresh(); });
            dialog->show();
        });
        auto *scene_as = new QPushButton(owner.tr("Scene AS..."), &owner);
        auto *callable_records = new QPushButton(owner.tr("Callable Records..."), &owner);
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

    void showError(const QString &message) {
        notice->setStyleSheet(
            QStringLiteral("color: #d94c3d;"));
        notice->setText(message);
    }

    void showInformation(const QString &message) {
        notice->setStyleSheet(
            QStringLiteral("color: #4f7cac;"));
        notice->setText(message);
    }

    void showSuccess(const QString &message) {
        notice->setStyleSheet(
            QStringLiteral("color: #388e3c;"));
        notice->setText(message);
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
        QJsonObject params{
            {QStringLiteral("base_source_digest"),
             source_digest},
            {QStringLiteral("operations"),
             QJsonArray{operation}},
        };
        if (request(RequestKind::Submit,
                    QStringLiteral("edit_render_features"),
                    params) != 0) {
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
        current->clear();
        for (const auto value : response.value(
                 QStringLiteral("features")).toArray()) {
            const auto reference = value.toString();
            auto *item = new QListWidgetItem(
                reference, current);
            item->setData(ReferenceRole, reference);
        }
        have_current = true;
        if (have_catalog) applyCatalogNamesToCurrent();
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
            item->setToolTip(reference);
        }
        if (pending.isEmpty() && active_ticket.isEmpty()) {
            showInformation(owner.tr(
                "Select one catalog entry to add, or one current feature to remove."));
        }
    }

    void consumeSubmit(const QJsonObject &response) {
        if (response.value(QStringLiteral("status")).toString() !=
            QStringLiteral("accepted")) {
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
            showError(responseError(response));
            return;
        }
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
        refresh->setEnabled(viewport.rpcReady() && idle);
        current->setEnabled(have_current && idle);
        catalog->setEnabled(have_catalog && idle);
        remove->setEnabled(
            can_edit && idle &&
            !current->selectedItems().isEmpty());
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
    auto *dialog = new ComputeTaskDialog(impl_->viewport, this, name, [this] {
        impl_->requestRefresh();
        emit renderConfigCommitted();
    });
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

} // namespace PelicanStudio
