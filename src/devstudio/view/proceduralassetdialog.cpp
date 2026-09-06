#include "proceduralassetdialog.hpp"
#include "../viewport/embeddedviewport.hpp"
#include "jsonrpcrequest.hpp"
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <array>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>

namespace PelicanStudio {
struct ProceduralAssetDialog::Impl {
  using Json = nlohmann::json;
  struct Draft {
    std::array<QString, 6> bounds;
    QString record;
    std::string digest;
    bool dirty = false;
    bool is_new = false;
  };
  ProceduralAssetDialog &owner;
  EmbeddedViewport &viewport;
  std::function<void()> committed;
  QComboBox *assets;
  std::array<QLineEdit *, 6> bounds;
  QPlainTextEdit *record;
  QLabel *notice;
  QPushButton *apply;
  QPushButton *refresh;
  QPushButton *reset;
  QPushButton *create;
  std::string catalog_digest;
  QTimer poll;
  std::map<std::string, Draft> drafts;
  std::map<std::string, Draft> latest;
  QString ticket;
  std::string submitted;
  bool editable = false;
  bool loading = false;
  qint64 request_id = 0;
  enum class Request { Read, Submit, Result } request_kind = Request::Read;

  std::string selected() const { return assets->currentText().toStdString(); }
  void controls() {
    const bool idle = !request_id && ticket.isEmpty();
    const bool writable = idle && editable && drafts.contains(selected());
    assets->setEnabled(idle);
    const auto draft = drafts.find(selected());
    const bool is_new = draft != drafts.end() && draft->second.is_new;
    reset->setEnabled(idle && (latest.contains(selected()) || is_new));
    create->setEnabled(idle && editable && viewport.rpcReady() &&
                       !catalog_digest.empty());
    apply->setText(is_new ? owner.tr("Create asset") : owner.tr("Apply"));
    for (auto *field : bounds)
      field->setReadOnly(!writable);
    record->setReadOnly(!writable);
    apply->setEnabled(writable && viewport.rpcReady());
    refresh->setEnabled(viewport.rpcReady() && !request_id);
  }
  void changed() {
    if (loading || !drafts.contains(selected()))
      return;
    auto &draft = drafts.at(selected());
    for (std::size_t index = 0; index < bounds.size(); ++index)
      draft.bounds[index] = bounds[index]->text();
    draft.record = record->toPlainText();
    draft.dirty = true;
  }
  void show() {
    loading = true;
    const auto found = drafts.find(selected());
    for (std::size_t index = 0; index < bounds.size(); ++index)
      bounds[index]->setText(
          found == drafts.end() ? QString{} : found->second.bounds[index]);
    record->setPlainText(found == drafts.end() ? QString{}
                                               : found->second.record);
    loading = false;
    controls();
  }
  void newAsset() {
    bool accepted = false;
    const auto name =
        QInputDialog::getText(&owner, owner.tr("New procedural asset"),
                              owner.tr("Asset name"), QLineEdit::Normal, {},
                              &accepted)
            .trimmed()
            .toStdString();
    if (!accepted)
      return;
    if (name.empty() || drafts.contains(name) || latest.contains(name)) {
      notice->setText(owner.tr("Choose a nonempty name that is not already "
                               "saved or used by a draft."));
      return;
    }
    Draft draft;
    if (const auto found = drafts.find(selected()); found != drafts.end())
      draft = found->second;
    else {
      draft.bounds = {"-1", "-1", "-1", "1", "1", "1"};
      draft.record = "{}";
    }
    draft.digest = catalog_digest;
    draft.dirty = true;
    draft.is_new = true;
    drafts.emplace(name, std::move(draft));
    assets->addItem(QString::fromStdString(name));
    assets->setCurrentIndex(assets->findText(QString::fromStdString(name)));
    show();
    notice->setText(owner.tr(
        "New asset draft. Create asset saves it; Reset selected discards it."));
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
      request(Request::Result, "get_edit_result", {{"ticket", ticket}});
  }
  void read() {
    if (!ticket.isEmpty())
      result();
    else
      request(Request::Read, "get_procedural_assets");
  }
  void submit() {
    try {
      const auto &draft = drafts.at(selected());
      auto aabb = Json::object();
      for (int side = 0; side < 2; ++side) {
        auto values = Json::array();
        for (int axis = 0; axis < 3; ++axis) {
          auto value = Json::parse(draft.bounds[side * 3 + axis].toStdString(),
                                   nullptr, false);
          if (!value.is_number() || !std::isfinite(value.get<double>()))
            throw std::invalid_argument("Bounds must contain finite numbers.");
          values.push_back(value);
        }
        aabb[side == 0 ? "min" : "max"] = values;
      }
      const auto data = Json::parse(draft.record.toStdString(), nullptr, false);
      if (!data.is_object())
        throw std::invalid_argument("Record JSON must be a valid object.");
      Json params{{"base_source_digest", draft.digest},
                  {"asset_name", selected()},
                  {"aabb", aabb},
                  {"record", data}};
      if (draft.is_new)
        params["operation"] = "create";
      const auto wire = exactJsonRpcObject(params);
      submitted = selected();
      request(Request::Submit, "edit_procedural_asset", wire);
    } catch (const std::exception &error) {
      notice->setText(QString::fromUtf8(error.what()));
    }
  }
  QString errorMessage(const Json &response) const {
    const auto error = response.find("error");
    if (error != response.end() && error->is_object()) {
      const auto message = error->value("message", std::string{});
      if (!message.empty())
        return QString::fromStdString(message);
    }
    return owner.tr("The asset edit was rejected.");
  }
  void receive(qint64 id, const QByteArray &bytes) {
    if (!id || id != request_id)
      return;
    request_id = 0;
    try {
      const auto response = Json::parse(bytes.toStdString());
      if (request_kind == Request::Read) {
        const auto digest =
            response.at("source_digest").at("hex").get<std::string>();
        const auto declarations =
            Json::parse(response.at("assets_json").get<std::string>());
        if (digest.empty() || !declarations.is_array())
          throw std::invalid_argument("Invalid procedural asset response.");
        const bool next_editable = response.at("editable").get<bool>();
        auto next = std::map<std::string, Draft>{};
        auto next_latest = std::map<std::string, Draft>{};
        for (const auto &asset : declarations) {
          const auto name = asset.at("name").get<std::string>();
          Draft draft;
          draft.digest = digest;
          for (int side = 0; side < 2; ++side) {
            const auto &values = asset.at("aabb").at(side == 0 ? "min" : "max");
            if (!values.is_array() || values.size() != 3)
              throw std::invalid_argument("Invalid asset bounds.");
            for (int axis = 0; axis < 3; ++axis)
              draft.bounds[side * 3 + axis] =
                  QString::fromStdString(values[axis].dump());
          }
          draft.record = QString::fromStdString(
              asset.value("record", Json::object()).dump(2));
          next_latest.emplace(name, draft);
          const auto previous = drafts.find(name);
          next.emplace(
              name, previous != drafts.end() &&
                            (previous->second.dirty || previous->second.is_new)
                        ? previous->second
                        : std::move(draft));
        }
        for (const auto &[name, draft] : drafts)
          if (draft.is_new)
            next.insert_or_assign(name, draft);
        catalog_digest = digest;
        const auto previous = assets->currentText();
        drafts = std::move(next);
        latest = std::move(next_latest);
        editable = next_editable;
        {
          const QSignalBlocker blocker(assets);
          assets->clear();
          for (const auto &[name, draft] : drafts)
            assets->addItem(QString::fromStdString(name));
          const int index = assets->findText(previous);
          if (index >= 0)
            assets->setCurrentIndex(index);
        }
        show();
        notice->setText(
            drafts.empty() ? owner.tr("No procedural assets are registered.")
            : editable
                ? owner.tr(
                      "Edit bounds and initial record values. Refresh keeps "
                      "unsaved edits based on their original saved version.")
                : owner.tr("Editing is unavailable. Reload externally modified "
                           "assets before editing."));
      } else if (request_kind == Request::Submit) {
        if (response.value("status", "") == "accepted") {
          ticket =
              QString::fromStdString(response.at("ticket").get<std::string>());
          if (ticket.isEmpty())
            throw std::invalid_argument("Missing edit ticket.");
          poll.start();
          result();
        } else
          notice->setText(errorMessage(response));
      } else if (response.value("status", "") != "pending") {
        poll.stop();
        ticket.clear();
        if (response.value("status", "") == "committed" &&
            response.value("committed", false)) {
          drafts.erase(submitted);
          committed();
          read();
        } else
          notice->setText(errorMessage(response));
      }
    } catch (const std::exception &error) {
      poll.stop();
      notice->setText(QString::fromUtf8(error.what()));
    }
    controls();
  }
  Impl(ProceduralAssetDialog &dialog, EmbeddedViewport &embedded,
       std::function<void()> callback)
      : owner(dialog), viewport(embedded), committed(std::move(callback)) {
    owner.setAttribute(Qt::WA_DeleteOnClose);
    owner.setWindowTitle(owner.tr("Procedural assets"));
    owner.setObjectName("pelican.proceduralAssets");
    owner.resize(580, 480);
    auto *layout = new QVBoxLayout(&owner);
    assets = new QComboBox(&owner);
    assets->setObjectName("pelican.proceduralAssets.asset");
    layout->addWidget(assets);
    auto *form = new QFormLayout;
    for (int side = 0; side < 2; ++side) {
      auto *row = new QWidget(&owner);
      auto *horizontal = new QHBoxLayout(row);
      horizontal->setContentsMargins(0, 0, 0, 0);
      for (int axis = 0; axis < 3; ++axis) {
        const int index = side * 3 + axis;
        bounds[index] = new QLineEdit(row);
        bounds[index]->setObjectName(
            QString("pelican.proceduralAssets.bound%1").arg(index));
        horizontal->addWidget(bounds[index]);
        QObject::connect(bounds[index], &QLineEdit::textChanged, &owner,
                         [this] { changed(); });
      }
      form->addRow(side == 0 ? owner.tr("Min X / Y / Z")
                             : owner.tr("Max X / Y / Z"),
                   row);
    }
    layout->addLayout(form);
    layout->addWidget(new QLabel(owner.tr("Record JSON"), &owner));
    record = new QPlainTextEdit(&owner);
    record->setObjectName("pelican.proceduralAssets.record");
    layout->addWidget(record);
    notice = new QLabel(&owner);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *buttons = new QHBoxLayout;
    create = new QPushButton(owner.tr("New..."), &owner);
    create->setObjectName("pelican.proceduralAssets.new");
    apply = new QPushButton(owner.tr("Apply"), &owner);
    reset = new QPushButton(owner.tr("Reset selected"), &owner);
    reset->setObjectName("pelican.proceduralAssets.reset");
    reset->setToolTip(owner.tr(
        "Discard selected asset edits and use the "
        "latest successfully refreshed values, or discard a new asset draft."));
    refresh = new QPushButton(owner.tr("Refresh / retry"), &owner);
    apply->setObjectName("pelican.proceduralAssets.apply");
    refresh->setObjectName("pelican.proceduralAssets.refresh");
    buttons->addWidget(create);
    buttons->addWidget(refresh);
    buttons->addWidget(reset);
    buttons->addWidget(apply);
    layout->addLayout(buttons);
    QObject::connect(assets, &QComboBox::currentIndexChanged, &owner,
                     [this] { show(); });
    QObject::connect(record, &QPlainTextEdit::textChanged, &owner,
                     [this] { changed(); });
    QObject::connect(create, &QPushButton::clicked, &owner,
                     [this] { newAsset(); });
    QObject::connect(reset, &QPushButton::clicked, &owner, [this] {
      const auto found = latest.find(selected());
      if (found == latest.end()) {
        const auto draft = drafts.find(selected());
        if (draft == drafts.end() || !draft->second.is_new)
          return;
        drafts.erase(draft);
        assets->removeItem(assets->currentIndex());
        show();
        notice->setText(owner.tr("New asset draft discarded."));
        return;
      }
      drafts[selected()] = found->second;
      show();
      notice->setText(owner.tr("Selected edits discarded. Values are from the "
                               "latest successful refresh."));
    });
    QObject::connect(apply, &QPushButton::clicked, &owner,
                     [this] { submit(); });
    QObject::connect(refresh, &QPushButton::clicked, &owner,
                     [this] { read(); });
    poll.setInterval(100);
    QObject::connect(&poll, &QTimer::timeout, &owner, [this] { result(); });
    QObject::connect(
        &viewport, &EmbeddedViewport::inspectorRpcSucceeded, &owner,
        [this](qint64 id, const QByteArray &bytes) { receive(id, bytes); });
    QObject::connect(&viewport, &EmbeddedViewport::inspectorRpcFailed, &owner,
                     [this](qint64 id, const QString &error) {
                       if (!id || id != request_id)
                         return;
                       request_id = 0;
                       poll.stop();
                       notice->setText(error);
                       controls();
                     });
    QObject::connect(&viewport, &EmbeddedViewport::engineRpcBecameUnavailable,
                     &owner, [this] {
                       request_id = 0;
                       ticket.clear();
                       editable = false;
                       poll.stop();
                       notice->setText(
                           owner.tr("Disconnected. Drafts are retained; "
                                    "refresh after reconnecting."));
                       controls();
                     });
    QObject::connect(&viewport, &EmbeddedViewport::engineRpcBecameAvailable,
                     &owner, [this] { controls(); });
    read();
  }
};
ProceduralAssetDialog::ProceduralAssetDialog(EmbeddedViewport &viewport,
                                             QWidget *parent,
                                             std::function<void()> committed)
    : QDialog(parent),
      impl_(std::make_unique<Impl>(*this, viewport, std::move(committed))) {}
ProceduralAssetDialog::~ProceduralAssetDialog() = default;
} // namespace PelicanStudio
