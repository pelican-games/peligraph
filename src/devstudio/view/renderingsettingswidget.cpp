#include "renderingsettingswidget.hpp"

#include "panelhelp.hpp"
#include "renderconfigdigest.hpp"
#include "rendereditcompletion.hpp"
#include "studiotheme.hpp"
#include "passforms/shaderdefinesform.hpp"
#include "../viewport/embeddedviewport.hpp"

#include <QAction>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

// A combo entry's data is its value as JSON text; empty for "the engine's
// default" (the key left out of the section).
QString text(const std::string &value) { return QString::fromStdString(value); }

QString compact(const Json &value) { return text(value.dump()); }

QJsonObject toQt(const Json &object) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(object.dump())).object();
}

// What each combo offers. The engine lists none of these (EL-3 takes each
// value whole and judges it by composing the candidate); they are the values
// docs/manual/06_rendering.md names. A value the root holds that is not here
// is added as an entry of its own, so the combo always shows what is written.
struct Choice {
    QString label;
    std::optional<Json> value;
};

std::vector<Choice> choicesFor(const std::string &key) {
    if (key == "draw_sort") {
        const auto policy = [](const char *xr) {
            return Json{{"opaque", {{"provider", "state_batched_v1"}}},
                        {"transparent", {{"provider", "back_to_front_v1"}}},
                        {"xr_view_policy", xr}};
        };
        return {{QObject::tr("Engine default"), std::nullopt},
                {QObject::tr("State batched / back to front"), policy("logical_view_center")},
                {QObject::tr("State batched / back to front, sorted per view (XR)"),
                 policy("per_view")}};
    }
    if (key == "render_strategy")
        return {{QObject::tr("Engine default (builtin.authored_config_v1)"), std::nullopt}};
    if (key == "resolver_version")
        return {{QObject::tr("Engine default (2)"), std::nullopt}, {QStringLiteral("2"), Json(2)}};
    if (key == "multisampling")
        return {{QObject::tr("Off (engine default)"), std::nullopt},
                {QObject::tr("2 samples"), Json{{"samples", 2}}},
                {QObject::tr("4 samples"), Json{{"samples", 4}}},
                {QObject::tr("8 samples"), Json{{"samples", 8}}},
                {QObject::tr("Automatic"), Json{{"mode", "automatic"}}}};
    return {};
}

const std::vector<std::string> &comboKeys() {
    static const std::vector<std::string> keys{"draw_sort", "render_strategy",
                                               "resolver_version", "multisampling"};
    return keys;
}

const std::vector<std::string> &jsonKeys() {
    static const std::vector<std::string> keys{"snapshots", "graph_transforms", "xr"};
    return keys;
}

// EL-3: the reasons a root that selects a preset refuses a key, as the engine
// gives them (renderconfigeditor.cpp, renderingKeyRefusal).
std::optional<QString> presetRootRefusal(const std::string &key) {
    if (key == "multisampling")
        return QObject::tr("preset_root_takes_pipeline_settings_msaa: a rendering section "
                           "that selects a pipeline preset takes multisampling only as "
                           "pipeline.settings.msaa");
    if (key == "resolver_version")
        return QObject::tr("not_in_preset_root_keys: a rendering section that selects a "
                           "pipeline preset does not take resolver_version; it belongs in "
                           "the preset's config or in a direct root's rendering section");
    return std::nullopt;
}

QString refusalLine(const Json &response) {
    const auto error = response.is_object() ? response.value("error", Json::object())
                                            : Json::object();
    const auto message = error.is_object() ? error.value("message", std::string{}) : std::string{};
    const auto code = error.is_object() ? error.value("code", std::string{}) : std::string{};
    if (!message.empty() && !code.empty()) return text(message + " (" + code + ")");
    if (!message.empty()) return text(message);
    if (!code.empty()) return text(code);
    return QObject::tr("The engine refused the rendering settings edit.");
}

} // namespace

const std::vector<std::string> &RenderingSettingsWidget::editableKeys() {
    // EL-3's eight, in the order the panel shows them.
    static const std::vector<std::string> keys{
        "shader_defines", "draw_sort",  "render_strategy",  "resolver_version",
        "multisampling",  "snapshots", "graph_transforms", "xr"};
    return keys;
}

struct RenderingSettingsWidget::Impl {
    enum class Request { None, Read, Submit, Result };

    // One edit_rendering_keys: one key, in `set` or in `remove`.
    struct Edit {
        std::string key;
        std::optional<Json> value;
    };

    RenderingSettingsWidget &owner;
    EmbeddedViewport &viewport;
    std::shared_ptr<RenderConfigDigest> digest_hub;
    std::function<void(const QString &)> engine_restart_action;
    bool restart_asked = false;

    QLabel *notice = nullptr;
    QPushButton *refresh_button = nullptr;
    ShaderDefinesForm *defines = nullptr;
    std::map<std::string, QComboBox *> combos;
    std::map<std::string, QPlainTextEdit *> json_fields;
    std::map<std::string, QPushButton *> json_apply;
    QTimer *poll = nullptr;

    // --- What was read: the root's digest and its rendering section.
    std::string digest;
    Json rendering = Json::object();
    bool have_root = false;
    bool selects_preset = false;
    // Keys the engine refused here (rendering_key_not_allowed_here): the
    // reason it gave.
    std::map<std::string, QString> refused_here;

    qint64 request_id = 0;
    Request request_kind = Request::None;
    QString ticket;
    std::optional<Edit> submitted;
    // A change made while another was in flight: sent once that one is done
    // and the root read again (the latest change of the field wins).
    std::optional<Edit> queued;
    bool retried = false;
    bool read_when_idle = false;
    bool filling = false;

    Impl(RenderingSettingsWidget &widget, EmbeddedViewport &embedded)
        : owner{widget}, viewport{embedded} {
        owner.setObjectName(QStringLiteral("pelican.renderingSettings"));
        owner.setFont(studioPanelFont());
        auto *outer = new QVBoxLayout(&owner);
        outer->setContentsMargins(0, 0, 0, 0);
        auto *scroll = new QScrollArea(&owner);
        scroll->setObjectName(QStringLiteral("pelican.renderingSettings.scroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *body = new QWidget(scroll);
        auto *layout = new QVBoxLayout(body);

        auto *heading = new QHBoxLayout;
        auto *title = new QLabel(owner.tr("Rendering settings"), body);
        title->setToolTip(owner.tr(
            "The root's rendering section keys no other panel writes. A change is "
            "saved at once and applied at a frame boundary."));
        heading->addWidget(title, 1);
        heading->addWidget(makePanelHelpButton(
            body, "pelican.renderingSettings.help", ManualRenderFeaturesAnchor,
            owner.tr("The root's rendering section keys no other panel writes "
                     "(edit_rendering_keys). A change is saved at once.")));
        layout->addLayout(heading);

        auto *defines_label = new QLabel(owner.tr("Shader defines"), body);
        defines_label->setToolTip(owner.tr(
            "rendering.shader_defines: added to the compile of every shader."));
        layout->addWidget(defines_label);
        defines = new ShaderDefinesForm(QStringLiteral("pelican.renderingSettings"), body);
        defines->setRenderingSection(true);
        layout->addWidget(defines);
        QObject::connect(defines, &PassForm::changed, &owner, [this] { definesChanged(); });

        auto *form = new QFormLayout;
        const std::map<std::string, QString> labels{
            {"draw_sort", owner.tr("Draw sort")},
            {"render_strategy", owner.tr("Render strategy")},
            {"resolver_version", owner.tr("Resolver version")},
            {"multisampling", owner.tr("Multisampling")},
            {"snapshots", owner.tr("Snapshots")},
            {"graph_transforms", owner.tr("Graph transforms")},
            {"xr", owner.tr("XR")},
        };
        for (const auto &key : comboKeys()) {
            auto *combo = new QComboBox(body);
            combo->setObjectName(QStringLiteral("pelican.renderingSettings.") + text(key));
            combo->setToolTip(QStringLiteral("rendering.") + text(key));
            combos[key] = combo;
            form->addRow(labels.at(key), combo);
            QObject::connect(combo, &QComboBox::currentIndexChanged, &owner,
                             [this, key] { comboChanged(key); });
        }
        // Large shapes: JSON for now, one Apply each.
        for (const auto &key : jsonKeys()) {
            auto *row = new QWidget(body);
            auto *row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(0, 0, 0, 0);
            auto *field = new QPlainTextEdit(row);
            field->setObjectName(QStringLiteral("pelican.renderingSettings.") + text(key));
            field->setPlaceholderText(owner.tr("Engine default (empty)"));
            field->setToolTip(owner.tr("rendering.%1 as JSON; empty leaves it out.")
                                  .arg(text(key)));
            field->setMaximumHeight(70);
            auto *apply = new QPushButton(owner.tr("Apply"), row);
            apply->setObjectName(QStringLiteral("pelican.renderingSettings.") + text(key) +
                                 QStringLiteral(".apply"));
            row_layout->addWidget(field, 1);
            row_layout->addWidget(apply, 0, Qt::AlignTop);
            json_fields[key] = field;
            json_apply[key] = apply;
            form->addRow(labels.at(key), row);
            QObject::connect(apply, &QPushButton::clicked, &owner, [this, key] { jsonApplied(key); });
        }
        layout->addLayout(form);
        layout->addStretch(1);

        notice = new QLabel(body);
        notice->setObjectName(QStringLiteral("pelican.renderingSettings.notice"));
        applyPanelNoticeStyle(notice);
        notice->hide();
        layout->addWidget(notice);

        auto *actions = new QHBoxLayout;
        refresh_button = new QPushButton(owner.tr("Refresh"), body);
        refresh_button->setObjectName(QStringLiteral("pelican.renderingSettings.refresh"));
        actions->addWidget(refresh_button);
        actions->addStretch(1);
        layout->addLayout(actions);
        QObject::connect(refresh_button, &QPushButton::clicked, &owner, [this] { read(true); });

        scroll->setWidget(body);
        outer->addWidget(scroll);

        poll = new QTimer(&owner);
        poll->setInterval(25);
        QObject::connect(poll, &QTimer::timeout, &owner, [this] { requestResult(); });

        QObject::connect(&viewport, &EmbeddedViewport::engineRpcBecameAvailable, &owner, [this] {
            forget();
            read(false);
        });
        QObject::connect(&viewport, &EmbeddedViewport::engineRpcBecameUnavailable, &owner,
                         [this](const QString &message) {
                             forget();
                             have_root = false;
                             restart_asked = false;
                             showError(message);
                             controls();
                         });
        QObject::connect(&viewport, &EmbeddedViewport::inspectorRpcSucceeded, &owner,
                         [this](qint64 id, const QByteArray &bytes) { receive(id, bytes); });
        QObject::connect(&viewport, &EmbeddedViewport::inspectorRpcFailed, &owner,
                         [this](qint64 id, const QString &message) { fail(id, message); });

        fill();
        if (viewport.rpcReady())
            read(false);
        else
            showInformation(owner.tr("Open a project to read its rendering settings."));
    }

    // --- The notice row.
    void showNotice(const QString &colour, const QString &message) {
        notice->setStyleSheet(colour);
        setPanelNotice(notice, oneLine(message), message);
    }
    void showError(const QString &message) { showNotice(QStringLiteral("color: #d94c3d;"), message); }
    void showInformation(const QString &message) {
        showNotice(QStringLiteral("color: #4f7cac;"), message);
    }
    void showSuccess(const QString &message) { showNotice(QStringLiteral("color: #388e3c;"), message); }

    [[nodiscard]] bool busy() const { return request_id != 0 || !ticket.isEmpty(); }

    void forget() {
        request_id = 0;
        request_kind = Request::None;
        ticket.clear();
        poll->stop();
        submitted.reset();
        retried = false;
    }

    bool send(Request kind, const QString &method, const Json &params) {
        QString error;
        const auto id = viewport.requestRpc(method, toQt(params), &error);
        if (id == 0) {
            showError(error);
            return false;
        }
        request_id = id;
        request_kind = kind;
        controls();
        return true;
    }

    // --- Reading the root.
    void read(bool say) {
        if (!viewport.rpcReady()) {
            showError(owner.tr("The engine RPC connection is not ready."));
            return;
        }
        if (busy()) {
            read_when_idle = true;
            return;
        }
        read_when_idle = false;
        if (send(Request::Read, QStringLiteral("get_render_documents"), Json::object()) && say)
            showInformation(owner.tr("Reading the rendering section..."));
    }

    void consumeRead(const Json &response) {
        if (response.value("status", std::string{}) == "rejected") {
            showError(refusalLine(response));
            return;
        }
        const auto documents = response.value("documents", Json::array());
        const Json *root = nullptr;
        if (documents.is_array())
            for (const auto &document : documents)
                if (document.is_object() && document.value("kind", std::string{}) == "root") {
                    root = &document;
                    break;
                }
        if (root == nullptr) {
            showError(owner.tr("The engine answered no root rendering document."));
            return;
        }
        const auto content =
            Json::parse(root->value("content", std::string{}), nullptr, false);
        rendering = Json::object();
        if (content.is_object()) {
            const auto section = content.find("rendering");
            if (section != content.end() && section->is_object()) rendering = *section;
        }
        // A rendering section that selects a preset names it as `pipeline`
        // (EL-3, editRenderingKeys).
        selects_preset = rendering.contains("pipeline");
        digest = root->value("digest", std::string{});
        have_root = !digest.empty();
        if (digest_hub && have_root) digest_hub->publish(digest, &owner);
        fill();
        if (queued) {
            auto edit = std::move(*queued);
            queued.reset();
            submit(std::move(edit));
        }
    }

    // --- The fields.
    std::optional<QString> refusal(const std::string &key) const {
        if (const auto found = refused_here.find(key); found != refused_here.end())
            return found->second;
        if (selects_preset) return presetRootRefusal(key);
        return std::nullopt;
    }

    const Json *current(const std::string &key) const {
        const auto found = rendering.find(key);
        return found != rendering.end() ? &*found : nullptr;
    }

    void fill() {
        filling = true;
        for (const auto &[key, combo] : combos) {
            combo->clear();
            for (const auto &choice : choicesFor(key))
                combo->addItem(choice.label,
                               choice.value ? compact(*choice.value) : QString{});
            // The first entry is the key left out.
            const auto *value = current(key);
            const QString wanted = value ? compact(*value) : QString{};
            int index = value ? combo->findData(wanted) : 0;
            if (index < 0) {
                // Written by hand or by another tool: shown as it is.
                combo->addItem(owner.tr("Custom: %1").arg(wanted), wanted);
                index = combo->count() - 1;
            }
            combo->setCurrentIndex(index);
        }
        defines->load(current("shader_defines") ? *current("shader_defines") : Json(nullptr));
        for (const auto &[key, field] : json_fields) {
            const auto *value = current(key);
            field->setPlainText(value ? text(value->dump(2)) : QString{});
        }
        filling = false;
        controls();
    }

    void controls() {
        const bool ready = viewport.rpcReady() && have_root;
        const auto gate = [this, ready](QWidget *widget, const std::string &key) {
            const auto refused = refusal(key);
            widget->setEnabled(ready && !refused);
            if (refused)
                widget->setToolTip(*refused);
            else
                widget->setToolTip(QStringLiteral("rendering.") + text(key));
        };
        for (const auto &[key, combo] : combos) gate(combo, key);
        for (const auto &[key, field] : json_fields) {
            gate(field, key);
            json_apply.at(key)->setEnabled(field->isEnabled());
        }
        defines->setEnabled(ready);
        refresh_button->setEnabled(viewport.rpcReady() && !busy());
    }

    void comboChanged(const std::string &key) {
        if (filling) return;
        const auto data = combos.at(key)->currentData().toString();
        edit(key, data.isEmpty() ? std::nullopt
                                 : std::optional<Json>{Json::parse(data.toStdString())});
    }

    void definesChanged() {
        if (filling) return;
        if (const auto refused = defines->refusal(); !refused.isEmpty()) {
            showError(refused);
            return;
        }
        const auto value = defines->defines();
        edit("shader_defines", value.is_null() ? std::nullopt : std::optional<Json>{value});
    }

    void jsonApplied(const std::string &key) {
        const auto typed = json_fields.at(key)->toPlainText().trimmed();
        if (typed.isEmpty()) {
            edit(key, std::nullopt);
            return;
        }
        auto value = Json::parse(typed.toStdString(), nullptr, false);
        if (value.is_discarded()) {
            showError(owner.tr("rendering.%1 is not JSON.").arg(text(key)));
            return;
        }
        edit(key, std::move(value));
    }

    // --- Sending.
    void edit(const std::string &key, std::optional<Json> value) {
        const auto *now = current(key);
        if ((now == nullptr && !value) || (now != nullptr && value && *now == *value)) return;
        Edit change{key, std::move(value)};
        if (busy() || !have_root) {
            queued = std::move(change);
            return;
        }
        submit(std::move(change));
    }

    void submit(Edit change) {
        if (refusal(change.key)) return;
        Json params{{"base_source_digest", digest}};
        if (change.value)
            params["set"] = Json{{change.key, *change.value}};
        else
            params["remove"] = Json::array({change.key});
        const auto key = text(change.key);
        submitted = std::move(change);
        if (send(Request::Submit, QStringLiteral("edit_rendering_keys"), params))
            showInformation(owner.tr("Saving %1...").arg(key));
        else
            submitted.reset();
    }

    void consumeSubmit(const Json &response) {
        const auto found = response.find("ticket");
        if (response.value("status", std::string{}) == "accepted" && found != response.end() &&
            found->is_string() && !found->get<std::string>().empty()) {
            ticket = text(found->get<std::string>());
            poll->start();
            requestResult();
            return;
        }
        // A committed answer at once is read as a ticket's last answer.
        if (readRenderEditCompletion(response).committed) {
            consumeResult(response);
            return;
        }
        refused(response);
    }

    void requestResult() {
        if (ticket.isEmpty() || request_id != 0) return;
        (void)send(Request::Result, QStringLiteral("get_edit_result"),
                   Json{{"ticket", ticket.toStdString()}});
    }

    void consumeResult(const Json &response) {
        if (response.value("status", std::string{}) == "pending") return;
        poll->stop();
        ticket.clear();
        // RSX, decision 1: the reading every panel uses. restart_required
        // hands the restart to the window before this returns.
        const auto completion = completeRenderEdit(response, [this](const std::string &line) {
            restart_asked = true;
            if (engine_restart_action) engine_restart_action(text(line));
            showInformation(text(line));
        });
        if (!completion.committed) {
            refused(response);
            return;
        }
        const auto change = std::exchange(submitted, std::nullopt);
        retried = false;
        if (!completion.source_digest.empty()) {
            digest = completion.source_digest;
            if (digest_hub) digest_hub->publish(digest, &owner);
        }
        // The root is read from the engine that comes back.
        if (completion.restart_required) return;
        const auto key = change ? text(change->key) : QString{};
        if (completion.no_change)
            showSuccess(owner.tr("%1 was already so.").arg(key));
        else
            showSuccess(owner.tr("Saved %1.").arg(key));
        read(false);
    }

    void refused(const Json &response) {
        const auto error = response.value("error", Json::object());
        const auto code = error.is_object() ? error.value("code", std::string{}) : std::string{};
        // Q-13: another panel moved the document since it was read. Read it
        // again and send the change once more against the new revision.
        if (code == "external_modification" && submitted && !retried) {
            retried = true;
            queued = std::move(submitted);
            submitted.reset();
            showInformation(owner.tr(
                "The document changed since this panel read it; reading it again..."));
            read(false);
            return;
        }
        if (code == "rendering_key_not_allowed_here" && error.is_object()) {
            const auto key = error.value("key", std::string{});
            const auto reason = error.value("reason", std::string{});
            const auto message = error.value("message", std::string{});
            if (!key.empty())
                refused_here[key] = text(reason.empty() ? message : reason + ": " + message);
        }
        submitted.reset();
        retried = false;
        showError(refusalLine(response));
        // The fields go back to what the root holds.
        read(false);
    }

    // --- Replies.
    void receive(qint64 id, const QByteArray &bytes) {
        if (id == 0 || id != request_id) return;
        const auto kind = std::exchange(request_kind, Request::None);
        request_id = 0;
        const auto response =
            Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
        if (!response.is_object()) {
            if (kind == Request::Result) {
                ticket.clear();
                poll->stop();
            }
            submitted.reset();
            showError(owner.tr("The engine answered something that is not an object."));
        } else if (kind == Request::Read) {
            consumeRead(response);
        } else if (kind == Request::Submit) {
            consumeSubmit(response);
        } else if (kind == Request::Result) {
            consumeResult(response);
        }
        followWhenIdle();
        controls();
    }

    void fail(qint64 id, const QString &message) {
        if (id == 0 || id != request_id) return;
        const auto kind = std::exchange(request_kind, Request::None);
        request_id = 0;
        if (kind == Request::Result) {
            ticket.clear();
            poll->stop();
        }
        if (kind != Request::Read) submitted.reset();
        showError(message);
        if (kind == Request::Submit) read(false);
        followWhenIdle();
        controls();
    }

    void followWhenIdle() {
        if (!busy() && read_when_idle) read(false);
    }

    // Another panel read or committed the document at `moved`.
    void documentMoved(const std::string &moved) {
        if (moved == digest) return;
        if (busy()) {
            read_when_idle = true;
            return;
        }
        read(false);
    }
};

RenderingSettingsWidget::RenderingSettingsWidget(EmbeddedViewport *viewport, QWidget *parent)
    : QWidget{parent} {
    if (viewport == nullptr)
        throw std::invalid_argument("RenderingSettingsWidget requires an embedded viewport");
    impl_ = std::make_unique<Impl>(*this, *viewport);
}

RenderingSettingsWidget::~RenderingSettingsWidget() = default;

void RenderingSettingsWidget::setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> digest) {
    impl_->digest_hub = std::move(digest);
    if (impl_->digest_hub)
        impl_->digest_hub->subscribe(this, [impl = impl_.get()](const std::string &moved) {
            impl->documentMoved(moved);
        });
}

void RenderingSettingsWidget::setRenderHistoryActions(const QList<QAction *> &actions) {
    addActions(actions);
}

void RenderingSettingsWidget::setEngineRestartAction(
    std::function<void(const QString &reason)> action) {
    impl_->engine_restart_action = std::move(action);
}

void RenderingSettingsWidget::engineRestartDeclined(const QString &message) {
    if (!std::exchange(impl_->restart_asked, false)) return;
    impl_->read(false);
    impl_->showInformation(message);
}

void RenderingSettingsWidget::refresh() { impl_->read(true); }

} // namespace PelicanStudio
