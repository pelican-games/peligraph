#include "raylayoutwidget.hpp"
#include "rendereditticketclient.hpp"

#include "panelhelp.hpp"
#include "inspectormodel.hpp"
#include "raylayoutmodel.hpp"
#include "renderconfigdigest.hpp"
#include "rendereditcompletion.hpp"
#include "studiotheme.hpp"
#include "../viewport/embeddedviewport.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QString text(const std::string &value) { return QString::fromStdString(value); }

QJsonObject toQt(const Json &object) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(object.dump())).object();
}

const QString Prefix = QStringLiteral("pelican.rayLayout.");

// An objectName part from a name: letters, digits, '_', '.' and '-'.
QString namePart(const std::string &name) {
    QString part;
    for (const char c : name)
        part += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') ? QChar(c)
                                                                                                    : QChar('_');
    return part;
}

void markName(QLineEdit *field, const std::optional<std::string> &problem) {
    field->setStyleSheet(problem ? QStringLiteral("QLineEdit { color: #d94c3d; border: 1px solid #d94c3d; }")
                                 : QString{});
    field->setToolTip(problem ? text(*problem) : QString{});
}

QLabel *heading(const QString &title, QWidget *parent) {
    auto *label = new QLabel(title, parent);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QPushButton *smallButton(const QString &label, const QString &name, const QString &tip, QWidget *parent) {
    auto *button = new QPushButton(label, parent);
    button->setObjectName(name);
    button->setToolTip(tip);
    button->setMaximumWidth(28);
    return button;
}

bool writeFile(const std::filesystem::path &path, const std::string &content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output{path, std::ios::binary};
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();
    return static_cast<bool>(output);
}

// The refusal's error object.
Json refusalError(const Json &response) {
    Json error = response.is_object() ? response.value("error", Json::object()) : Json::object();
    return error.is_object() ? error : Json::object();
}

// RTL-2s-c: the refusal as one sentence -- the Graph's and the Problems
// window's table (rayTracingRefusalDescription, knownRefusalDescription) --
// else the parser's message, its code and what it names.
std::string refusalSentence(const Json &response) {
    const auto error = refusalError(response);
    if (auto ray = rayTracingRefusalDescription(error); !ray.empty()) return ray;
    if (auto known = knownRefusalDescription(error.value("code", std::string{}), error.value("message", std::string{}));
        !known.empty())
        return known;
    return {};
}

// The refusal as one line: the parser's code, and what it names.
QString refusalLine(const Json &response) {
    if (const auto sentence = refusalSentence(response); !sentence.empty()) return text(sentence);
    Json error = refusalError(response);
    QString where;
    for (const auto *key : {"task", "node", "ray_type", "group", "miss", "asset"}) {
        const auto found = error.find(key);
        if (found != error.end() && found->is_string())
            where += QStringLiteral(" %1 %2").arg(QString::fromLatin1(key), text(found->get<std::string>()));
    }
    const auto code = error.value("code", std::string{});
    auto message = error.value("message", std::string{});
    if (message.empty()) message = "The engine refused the ray layout edit.";
    return text(message) + (code.empty() ? QString{} : QStringLiteral(" (") + text(code) + QStringLiteral(")")) +
           (where.isEmpty() ? QString{} : QStringLiteral(" --") + where);
}

} // namespace

struct RayLayoutWidget::Impl {

    RayLayoutWidget &owner;
    EmbeddedViewport &viewport;
    RenderEditTicketClient client;
    std::shared_ptr<RenderConfigDigest> digest_hub;
    std::function<void(const QString &)> engine_restart_action;
    std::function<void(const QString &)> show_document_action;
    std::function<void()> acceleration_action;
    std::function<void(const Json &, const std::string &)> refusal_action;
    std::function<void(std::vector<RenderEditHistory::FileChange>)> shader_history_action;
    bool restart_asked = false;

    QLabel *state_line = nullptr;
    QPushButton *enable_button = nullptr;
    QPushButton *disable_button = nullptr;
    QPushButton *restart_button = nullptr;
    QPushButton *show_document = nullptr;
    QPushButton *acceleration_button = nullptr;
    QWidget *tables_host = nullptr;
    QVBoxLayout *tables_layout = nullptr;
    QWidget *tables = nullptr;
    QLabel *notice = nullptr;
    QPointer<QDialog> dialog;

    // --- What was read.
    qint64 layout_request = 0;
    qint64 documents_request = 0;
    std::optional<RayLayoutRead> layout_answer;
    std::optional<Json> documents_answer;
    std::optional<RayLayoutRead> read_state;
    std::vector<RayTracingTask> tasks;
    bool read_again = false;

    // --- What is in flight.
    std::optional<RayLayoutPlan> submitted;
    std::optional<RayLayoutRead> submitted_from;
    // Decision 7: the reads before the edits that rewrote procedural records.
    QString pending_reveal;

    Impl(RayLayoutWidget &widget, EmbeddedViewport &embedded) : owner{widget}, viewport{embedded}, client{embedded, &widget} {
        owner.setObjectName(QStringLiteral("pelican.rayLayout"));
        owner.setFont(studioPanelFont());
        auto *layout = new QVBoxLayout(&owner);
        layout->setContentsMargins(0, 8, 0, 0);
        auto *line = new QFrame(&owner);
        line->setFrameShape(QFrame::HLine);
        layout->addWidget(line);
        auto *head = new QHBoxLayout;
        auto *title = heading(owner.tr("Ray layout"), &owner);
        title->setObjectName(Prefix + QStringLiteral("title"));
        title->setToolTip(owner.tr("The project's ray group layout: the ray types, hit groups and miss "
                                   "shaders its ray tracing tasks fill, and which objects each hit group "
                                   "answers for."));
        head->addWidget(title, 1);
        head->addWidget(makePanelHelpButton(&owner, "pelican.rayLayout.help", ManualRayLayoutAnchor,
                                            owner.tr("The ray group layout (get_ray_layout / edit_ray_layout).")));
        layout->addLayout(head);
        state_line = new QLabel(&owner);
        state_line->setObjectName(Prefix + QStringLiteral("state"));
        state_line->setWordWrap(true);
        layout->addWidget(state_line);
        auto *buttons = new QHBoxLayout;
        enable_button = new QPushButton(owner.tr("Enable hit groups"), &owner);
        enable_button->setObjectName(Prefix + QStringLiteral("enable"));
        enable_button->setToolTip(owner.tr("Write a ray layout (ray type primary, hit group default, miss "
                                           "miss) and every ray tracing task in its form; the engine starts "
                                           "again to run it"));
        disable_button = new QPushButton(owner.tr("Disable hit groups"), &owner);
        disable_button->setObjectName(Prefix + QStringLiteral("disable"));
        restart_button = new QPushButton(owner.tr("Restart engine"), &owner);
        restart_button->setObjectName(Prefix + QStringLiteral("restart"));
        show_document = new QPushButton(owner.tr("Show the document"), &owner);
        show_document->setObjectName(Prefix + QStringLiteral("showDocument"));
        acceleration_button = new QPushButton(owner.tr("Acceleration structures..."), &owner);
        acceleration_button->setObjectName(Prefix + QStringLiteral("accelerationStructures"));
        acceleration_button->setToolTip(owner.tr("The scene's acceleration structure settings (defaults, "
                                                 "rules, space) in the Task Editor"));
        // RTL-3s, decision 3 (engine EB-2): a procedural asset's record is
        // taken back by the render history's Undo, as the rest is.
        for (auto *button : {enable_button, disable_button, restart_button, show_document, acceleration_button})
            buttons->addWidget(button);
        buttons->addStretch(1);
        layout->addLayout(buttons);
        tables_host = new QWidget(&owner);
        tables_layout = new QVBoxLayout(tables_host);
        tables_layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(tables_host);
        notice = new QLabel(&owner);
        notice->setObjectName(Prefix + QStringLiteral("notice"));
        applyPanelNoticeStyle(notice);
        notice->hide();
        layout->addWidget(notice);

        QObject::connect(enable_button, &QPushButton::clicked, &owner, [this] { enable(); });
        QObject::connect(disable_button, &QPushButton::clicked, &owner, [this] { disable(); });
        QObject::connect(restart_button, &QPushButton::clicked, &owner, [this] {
            if (engine_restart_action) {
                restart_asked = true;
                engine_restart_action(owner.tr("the ray layout"));
            }
        });
        QObject::connect(show_document, &QPushButton::clicked, &owner, [this] {
            if (show_document_action && read_state) show_document_action(text(read_state->layout.reference));
        });
        QObject::connect(acceleration_button, &QPushButton::clicked, &owner, [this] {
            if (acceleration_action) acceleration_action();
        });


        QObject::connect(&viewport, &EmbeddedViewport::engineRpcBecameAvailable, &owner, [this] {
            forget();
            restart_asked = false;
            read();
        });
        client.unavailable = [this](const QString &message) {
            forget();
            read_state.reset();
            fill();
        };
        client.completed = [this](const auto &outcome) { edited(outcome); };
        client.unhandledResult = [this](qint64 id, const QByteArray &bytes) { receive(id, bytes); };
        client.unhandledFailure = [this](qint64 id, const QString &message) { fail(id, message); };
        fill();
        if (viewport.rpcReady()) read();
    }

    // --- Notices.
    void say(const QString &colour, const QString &message) {
        notice->setStyleSheet(colour);
        setPanelNotice(notice, oneLine(message, 160), message);
    }
    void showError(const QString &message) { say(QStringLiteral("color: #d94c3d;"), message); }
    void showInformation(const QString &message) { say(QStringLiteral("color: #4f7cac;"), message); }
    void showSuccess(const QString &message) { say(QStringLiteral("color: #388e3c;"), message); }

    [[nodiscard]] bool busy() const { return client.busy(); }
    [[nodiscard]] std::filesystem::path projectRoot() const {
        return std::filesystem::path{viewport.projectRoot().toStdWString()};
    }

    void forget() {
        client.abandon();
        layout_request = documents_request = 0;
        layout_answer.reset();
        documents_answer.reset();
        submitted.reset();
        submitted_from.reset();
    }

    // --- Reading: get_ray_layout and get_render_documents (the tasks).
    void read() {
        if (!viewport.rpcReady()) return;
        if (busy() || layout_request != 0 || documents_request != 0) {
            read_again = true;
            return;
        }
        read_again = false;
        layout_answer.reset();
        documents_answer.reset();
        QString error;
        layout_request = viewport.requestRpc(QStringLiteral("get_ray_layout"), QJsonObject{}, &error);
        documents_request = viewport.requestRpc(QStringLiteral("get_render_documents"), QJsonObject{}, &error);
        if (layout_request == 0 || documents_request == 0) showError(error);
    }

    void readArrived() {
        if (!layout_answer || !documents_answer) return;
        read_state = std::move(*layout_answer);
        tasks = rayTracingTasks(*documents_answer);
        layout_answer.reset();
        documents_answer.reset();
        fill();
        if (!pending_reveal.isEmpty()) {
            const auto material = std::exchange(pending_reveal, QString{});
            owner.revealMaterial(material);
        }
        if (read_again) read();
    }

    // --- Sending a plan.
    void apply(RayLayoutPlan plan, bool confirmed = false) {
        if (plan.problem) {
            showError(text(*plan.problem));
            return;
        }
        if (plan.empty() && plan.shaders.empty()) return;
        if (!read_state) return;
        if (busy()) {
            showInformation(owner.tr("Another ray layout edit is on its way; try again when it is done."));
            return;
        }
        if (!plan.confirm.empty() && !confirmed) {
            confirm(text(plan.confirm), [this, plan]() mutable { apply(std::move(plan), true); });
            return;
        }
        // The shader files first: the edit's commit compiles them.
        for (const auto &write : plan.shaders)
            if (!writeFile(write.path, write.content)) {
                revertShaders(plan);
                showError(owner.tr("Could not write %1.").arg(text(write.path.generic_string())));
                return;
            }
        const auto request = rayLayoutRequest(*read_state, tasks, plan);
        // RTL-2s-c: the shaders it rewrote go with the edit's history entry,
        // so one Undo takes the names and the shaders back together.
        if (shader_history_action && !plan.shaders.empty()) {
            std::vector<RenderEditHistory::FileChange> files;
            for (const auto &write : plan.shaders)
                files.push_back({write.path.string(), write.previous, write.content});
            shader_history_action(std::move(files));
        }
        submitted_from = read_state;
        submitted = std::move(plan);
        if (!client.submit(text(request.method), request.params)) return;
        showInformation(owner.tr("Saving the ray layout (%1)...").arg(text(submitted->label)));
        controls();
    }

    void revertShaders(const RayLayoutPlan &plan) {
        for (const auto &write : plan.shaders) {
            if (write.previous) {
                (void)writeFile(write.path, *write.previous);
            } else {
                std::error_code error;
                std::filesystem::remove(write.path, error);
            }
        }
    }





    void edited(const RenderEditTicketClient::Outcome &outcome) {
        using Kind = RenderEditTicketClient::Kind;
        if (outcome.kind == Kind::Accepted || outcome.kind == Kind::Pending) return;
        if (outcome.kind == Kind::Committed) consumeResult(outcome.result);
        else if (outcome.kind == Kind::Refused ||
                 (outcome.kind == Kind::Failed && outcome.result.is_object() && outcome.result.contains("error")))
            refused(outcome.result);
        else {
            showError(text(outcome.error));
            if (outcome.kind == Kind::Failed) {
                if (submitted) revertShaders(*submitted);
                submitted_from.reset();
                submitted.reset();
                read();
            }
        }
        controls();
    }

    void consumeResult(const Json &response) {
        const auto completion = completeRenderEdit(response, [this](const std::string &line) {
            restart_asked = true;
            if (engine_restart_action) engine_restart_action(text(line));
            showInformation(text(line));
        });
        if (!completion.committed) {
            refused(response);
            return;
        }
        auto plan = std::exchange(submitted, std::nullopt);
        submitted_from.reset();
        if (completion.restart_required) return;
        QString line = plan ? owner.tr("Saved: %1.").arg(text(plan->label)) : owner.tr("Saved.");
        if (plan)
            for (const auto &note : plan->notes) line += QStringLiteral(" ") + text(note);
        showSuccess(line);
        read();
    }

    void refused(const Json &response) {
        if (submitted) revertShaders(*submitted);
        submitted.reset();
        submitted_from.reset();
        showError(refusalLine(response));
        // RTL-2s-c: a task's refusal marks its node and the cell its fields
        // name in the Graph, with the same sentence.
        const auto error = refusalError(response);
        if (refusal_action && error.contains("task")) {
            auto sentence = refusalSentence(response);
            if (sentence.empty()) sentence = refusalLine(response).toStdString();
            refusal_action(error, sentence);
        }
        read();
    }

    // --- Replies.
    void receive(qint64 id, const QByteArray &bytes) {
        if (id == 0) return;
        const auto response = Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
        if (id == layout_request) {
            layout_request = 0;
            layout_answer = parseRayLayoutRead(response);
            readArrived();
            return;
        }
        if (id == documents_request) {
            documents_request = 0;
            documents_answer = response.is_object() ? response : Json::object();
            readArrived();
            return;
        }
    }

    void fail(qint64 id, const QString &message) {
        if (id == 0) return;
        if (id == layout_request || id == documents_request) {
            layout_request = documents_request = 0;
            layout_answer.reset();
            documents_answer.reset();
            showError(message);
            return;
        }
    }

    // --- Decision 2.
    void enable() {
        if (!read_state) return;
        apply(planEnableHitGroups(*read_state, tasks, projectRoot()));
    }
    void disable() {
        if (!read_state) return;
        auto plan = planDisableHitGroups(*read_state, tasks, projectRoot());
        if (!plan.problem)
            plan.confirm = "Disable hit groups? The ray layout is taken out of the project, every object answers "
                           "with the same closest hit, and the engine starts again.";
        apply(std::move(plan));
    }

    // --- The confirmation and the add dialog.
    void confirm(const QString &question, std::function<void()> yes) {
        if (dialog) dialog->close();
        auto *box = new QDialog(&owner);
        dialog = box;
        box->setObjectName(Prefix + QStringLiteral("confirm"));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setWindowTitle(owner.tr("Ray layout"));
        auto *layout = new QVBoxLayout(box);
        auto *label = new QLabel(question, box);
        label->setObjectName(Prefix + QStringLiteral("confirm.text"));
        label->setWordWrap(true);
        layout->addWidget(label);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch(1);
        auto *ok = new QPushButton(owner.tr("OK"), box);
        ok->setObjectName(Prefix + QStringLiteral("confirm.ok"));
        auto *cancel = new QPushButton(owner.tr("Cancel"), box);
        cancel->setObjectName(Prefix + QStringLiteral("confirm.cancel"));
        buttons->addWidget(ok);
        buttons->addWidget(cancel);
        layout->addLayout(buttons);
        QObject::connect(ok, &QPushButton::clicked, box, [box, yes] {
            box->accept();
            yes();
        });
        QObject::connect(cancel, &QPushButton::clicked, box, &QDialog::reject);
        box->open();
    }

    enum class AddKind { ray_type, group, miss, record_field };

    void openAdd(AddKind kind) {
        if (!read_state || !read_state->layout.usable()) return;
        if (dialog) dialog->close();
        const auto &layout = read_state->layout.document;
        std::vector<std::string> taken;
        std::string base;
        QString title;
        RayNameKind name_kind = RayNameKind::ray_type;
        switch (kind) {
        case AddKind::ray_type:
            taken = rayLayoutRayTypes(layout);
            base = "shadow";
            title = owner.tr("Add ray type");
            break;
        case AddKind::group:
            taken = rayLayoutGroupNames(layout);
            base = "group_" + std::to_string(taken.size() + 1);
            title = owner.tr("Add hit group");
            name_kind = RayNameKind::group;
            break;
        case AddKind::miss:
            taken = rayLayoutMisses(layout);
            base = "miss_" + std::to_string(taken.size() + 1);
            title = owner.tr("Add miss shader");
            name_kind = RayNameKind::miss;
            break;
        case AddKind::record_field:
            for (const auto &field : rayLayoutRecord(layout)) taken.push_back(field.name);
            base = "value";
            title = owner.tr("Add record field");
            name_kind = RayNameKind::record_field;
            break;
        }
        auto *box = new QDialog(&owner);
        dialog = box;
        box->setObjectName(Prefix + QStringLiteral("addDialog"));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setWindowTitle(title);
        auto *form = new QFormLayout(box);
        auto *name = new QLineEdit(text(rayFreeName(base, taken)), box);
        name->setObjectName(Prefix + QStringLiteral("addDialog.name"));
        form->addRow(owner.tr("Name"), name);
        auto *problem = new QLabel(box);
        problem->setObjectName(Prefix + QStringLiteral("addDialog.problem"));
        problem->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        form->addRow(problem);
        QComboBox *type = nullptr;
        if (kind == AddKind::group) {
            type = new QComboBox(box);
            type->addItem(owner.tr("Triangles"), QStringLiteral("triangles"));
            type->addItem(owner.tr("Procedural"), QStringLiteral("procedural"));
        } else if (kind == AddKind::record_field) {
            type = new QComboBox(box);
            for (const auto &each : rayRecordTypes()) type->addItem(text(each), text(each));
        }
        if (type != nullptr) {
            type->setObjectName(Prefix + QStringLiteral("addDialog.type"));
            form->addRow(owner.tr("Type"), type);
        }
        QRadioButton *copy = nullptr;
        QRadioButton *from_template = nullptr;
        if (kind == AddKind::group || kind == AddKind::miss || kind == AddKind::ray_type) {
            copy = new QRadioButton(kind == AddKind::group  ? owner.tr("Copy the shaders of the default group")
                                    : kind == AddKind::miss ? owner.tr("Copy the first miss shader")
                                                            : owner.tr("Copy the first ray type's hit shaders"),
                                    box);
            copy->setObjectName(Prefix + QStringLiteral("addDialog.copy"));
            from_template = new QRadioButton(kind == AddKind::ray_type
                                                 ? owner.tr("New hit shaders from the template (one per hit group)")
                                                 : owner.tr("New shader from the template"),
                                             box);
            from_template->setObjectName(Prefix + QStringLiteral("addDialog.template"));
            // RTL-2s-c: a ray type's copied row would answer in the first ray
            // type's payload: the template is its default.
            (kind == AddKind::ray_type ? from_template : copy)->setChecked(true);
            form->addRow(copy);
            form->addRow(from_template);
        }
        auto *ok = new QPushButton(owner.tr("Add"), box);
        ok->setObjectName(Prefix + QStringLiteral("addDialog.ok"));
        auto *cancel = new QPushButton(owner.tr("Cancel"), box);
        cancel->setObjectName(Prefix + QStringLiteral("addDialog.cancel"));
        auto *row = new QHBoxLayout;
        row->addStretch(1);
        row->addWidget(ok);
        row->addWidget(cancel);
        form->addRow(row);
        const auto check = [name, problem, ok, taken, name_kind] {
            const auto found = rayNameProblem(name_kind, name->text().toStdString(), taken);
            markName(name, found);
            problem->setText(found ? text(*found) : QString{});
            ok->setEnabled(!found);
        };
        QObject::connect(name, &QLineEdit::textChanged, box, check);
        check();
        QObject::connect(cancel, &QPushButton::clicked, box, &QDialog::reject);
        QObject::connect(ok, &QPushButton::clicked, box, [this, box, kind, name, type, from_template] {
            const auto chosen = name->text().toStdString();
            const auto chosen_type = type ? type->currentData().toString().toStdString() : std::string{};
            const auto mode = from_template && from_template->isChecked() ? RayFillMode::from_template
                                                                          : RayFillMode::copy;
            box->accept();
            if (!read_state) return;
            switch (kind) {
            case AddKind::ray_type: apply(planAddRayType(*read_state, tasks, chosen, projectRoot(), mode)); break;
            case AddKind::group:
                apply(planAddGroup(*read_state, tasks, projectRoot(), chosen, chosen_type, mode));
                break;
            case AddKind::miss: apply(planAddMiss(*read_state, tasks, projectRoot(), chosen, mode)); break;
            case AddKind::record_field:
                apply(planAddRecordField(*read_state, chosen, chosen_type, tasks, projectRoot()));
                break;
            }
        });
        box->open();
    }

    // --- The section.
    void controls() {
        const bool ready = viewport.rpcReady() && read_state.has_value() && !busy();
        const bool has_layout = read_state && read_state->layout.named;
        enable_button->setVisible(read_state && !has_layout);
        enable_button->setEnabled(ready);
        disable_button->setVisible(has_layout && read_state->layout.usable());
        // Offered only when every task can go back to the form without a
        // layout; otherwise the button says what stays.
        std::optional<std::string> stays;
        if (has_layout && read_state->layout.usable())
            stays = planDisableHitGroups(*read_state, tasks, projectRoot()).problem;
        disable_button->setEnabled(ready && !stays);
        disable_button->setToolTip(stays ? text(*stays)
                                         : owner.tr("Take the ray layout out and every task back to the form "
                                                    "without one; the engine starts again"));
        restart_button->setVisible(read_state && read_state->restartPending());
        restart_button->setEnabled(viewport.rpcReady() && !busy());
        show_document->setVisible(has_layout && !read_state->layout.usable());
        acceleration_button->setVisible(has_layout && read_state->layout.usable());
        acceleration_button->setEnabled(ready && static_cast<bool>(acceleration_action));
        if (tables) tables->setEnabled(ready);
    }

    void fill() {
        if (tables) {
            tables->hide();
            tables->setParent(nullptr);
            tables->deleteLater();
            tables = nullptr;
        }
        if (!read_state) {
            state_line->setText(viewport.rpcReady() ? owner.tr("Reading the ray layout...")
                                                    : owner.tr("Open a project to read its ray layout."));
            controls();
            return;
        }
        const auto &read = *read_state;
        const QString dim = QStringLiteral("color: %1;").arg(studioDimTextColor().name());
        if (!read.layout.named) {
            state_line->setStyleSheet(dim);
            state_line->setText(
                tasks.empty()
                    ? owner.tr("No ray layout: a ray tracing task traces every object with the same closest "
                               "hit. Enable hit groups to give objects hit groups of their own.")
                    : owner.tr("No ray layout: %n ray tracing task(s) trace every object with the same closest "
                               "hit. Enable hit groups to give objects hit groups of their own.",
                               nullptr, static_cast<int>(tasks.size())));
            if (read.restartPending())
                state_line->setText(owner.tr("The ray layout was taken out; the engine runs without it once it "
                                             "starts again."));
            controls();
            return;
        }
        if (!read.layout.usable()) {
            state_line->setStyleSheet(QStringLiteral("color: #d94c3d;"));
            state_line->setText(owner.tr("The ray layout %1 cannot be read: %2 (%3)")
                                    .arg(text(read.layout.reference), text(read.layout.error_message),
                                         text(read.layout.error_code)));
            controls();
            return;
        }
        state_line->setStyleSheet(read.restartPending() ? QStringLiteral("color: #c77700;") : dim);
        state_line->setText(read.restartPending()
                                ? owner.tr("Written to %1; the engine runs it once it starts again.")
                                      .arg(text(read.layout.reference))
                                : owner.tr("Ray layout %1.").arg(text(read.layout.reference)));
        tables = new QWidget(tables_host);
        tables->setObjectName(Prefix + QStringLiteral("tables"));
        auto *layout = new QVBoxLayout(tables);
        layout->setContentsMargins(0, 0, 0, 0);
        buildNames(layout, RayEntryKind::ray_type);
        buildNames(layout, RayEntryKind::group);
        buildNames(layout, RayEntryKind::miss);
        buildRecord(layout);
        buildAssignments(layout);
        tables_layout->addWidget(tables);
        controls();
    }

    // Ray types / Hit groups / Miss shaders: a row each.
    void buildNames(QVBoxLayout *layout, RayEntryKind kind) {
        const auto &doc = read_state->layout.document;
        const QString section = kind == RayEntryKind::ray_type ? QStringLiteral("rayType")
                                : kind == RayEntryKind::group  ? QStringLiteral("group")
                                                               : QStringLiteral("miss");
        layout->addWidget(heading(kind == RayEntryKind::ray_type ? owner.tr("Ray types")
                                  : kind == RayEntryKind::group  ? owner.tr("Hit groups")
                                                                 : owner.tr("Miss shaders"),
                                  tables));
        std::vector<std::string> names = kind == RayEntryKind::ray_type ? rayLayoutRayTypes(doc)
                                         : kind == RayEntryKind::group  ? rayLayoutGroupNames(doc)
                                                                        : rayLayoutMisses(doc);
        const auto groups = rayLayoutGroups(doc);
        const auto name_kind = kind == RayEntryKind::ray_type ? RayNameKind::ray_type
                               : kind == RayEntryKind::group  ? RayNameKind::group
                                                              : RayNameKind::miss;
        auto *grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto name = names[index];
            const auto prefix = Prefix + section + QStringLiteral(".") + namePart(name);
            auto *field = new QLineEdit(text(name), tables);
            field->setObjectName(prefix + QStringLiteral(".name"));
            int column = 0;
            grid->addWidget(field, static_cast<int>(index), column++);
            std::vector<std::string> others = names;
            others.erase(others.begin() + static_cast<std::ptrdiff_t>(index));
            QObject::connect(field, &QLineEdit::textEdited, field, [field, others, name_kind] {
                markName(field, rayNameProblem(name_kind, field->text().toStdString(), others));
            });
            QObject::connect(field, &QLineEdit::editingFinished, &owner, [this, field, name, kind, others, name_kind] {
                const auto to = field->text().trimmed().toStdString();
                if (to == name || !read_state) return;
                if (rayNameProblem(name_kind, to, others)) return;
                auto plan = planRename(*read_state, tasks, kind, name, to, rayProjectShaderFiles(projectRoot()));
                QTimer::singleShot(0, &owner, [this, plan = std::move(plan)]() mutable { apply(std::move(plan)); });
            });
            if (kind == RayEntryKind::group) {
                auto *type = new QComboBox(tables);
                type->setObjectName(prefix + QStringLiteral(".type"));
                type->addItem(owner.tr("Triangles"), QStringLiteral("triangles"));
                type->addItem(owner.tr("Procedural"), QStringLiteral("procedural"));
                type->setCurrentIndex(groups[index].type == "procedural" ? 1 : 0);
                grid->addWidget(type, static_cast<int>(index), column++);
                QObject::connect(type, &QComboBox::currentIndexChanged, &owner, [this, type, name] {
                    if (!read_state) return;
                    apply(planSetGroupType(*read_state, tasks, name, type->currentData().toString().toStdString()));
                });
                auto *is_default = new QRadioButton(owner.tr("default"), tables);
                is_default->setObjectName(prefix + QStringLiteral(".default"));
                is_default->setAutoExclusive(false);
                is_default->setChecked(rayLayoutDefaultGroup(doc) == name);
                is_default->setToolTip(owner.tr("The hit group of every object no assignment picks"));
                grid->addWidget(is_default, static_cast<int>(index), column++);
                QObject::connect(is_default, &QRadioButton::clicked, &owner, [this, name] {
                    if (read_state) apply(planSetDefaultGroup(*read_state, name));
                });
            }
            auto *up = smallButton(QStringLiteral("\u2191"), prefix + QStringLiteral(".up"), owner.tr("Earlier"), tables);
            auto *down =
                smallButton(QStringLiteral("\u2193"), prefix + QStringLiteral(".down"), owner.tr("Later"), tables);
            auto *remove =
                smallButton(QStringLiteral("\u00d7"), prefix + QStringLiteral(".remove"), owner.tr("Remove"), tables);
            up->setEnabled(index > 0);
            down->setEnabled(index + 1 < names.size());
            remove->setEnabled(names.size() > 1);
            grid->addWidget(up, static_cast<int>(index), column++);
            grid->addWidget(down, static_cast<int>(index), column++);
            grid->addWidget(remove, static_cast<int>(index), column++);
            QObject::connect(up, &QPushButton::clicked, &owner,
                             [this, kind, name] { if (read_state) apply(planMove(*read_state, kind, name, -1)); });
            QObject::connect(down, &QPushButton::clicked, &owner,
                             [this, kind, name] { if (read_state) apply(planMove(*read_state, kind, name, +1)); });
            QObject::connect(remove, &QPushButton::clicked, &owner, [this, kind, name] {
                if (!read_state) return;
                if (kind == RayEntryKind::ray_type) apply(planRemoveRayType(*read_state, tasks, name));
                else if (kind == RayEntryKind::group) apply(planRemoveGroup(*read_state, tasks, name));
                else apply(planRemoveMiss(*read_state, tasks, name));
            });
        }
        grid->setColumnStretch(0, 1);
        layout->addLayout(grid);
        auto *add = new QPushButton(QStringLiteral("+"), tables);
        add->setObjectName(Prefix + section + QStringLiteral(".add"));
        add->setMaximumWidth(32);
        add->setToolTip(kind == RayEntryKind::ray_type ? owner.tr("Add a ray type")
                        : kind == RayEntryKind::group  ? owner.tr("Add a hit group")
                                                       : owner.tr("Add a miss shader"));
        layout->addWidget(add, 0, Qt::AlignLeft);
        QObject::connect(add, &QPushButton::clicked, &owner, [this, kind] {
            openAdd(kind == RayEntryKind::ray_type ? AddKind::ray_type
                    : kind == RayEntryKind::group  ? AddKind::group
                                                   : AddKind::miss);
        });
    }

    void buildRecord(QVBoxLayout *layout) {
        const auto &doc = read_state->layout.document;
        layout->addWidget(heading(owner.tr("Record fields"), tables));
        const auto fields = rayLayoutRecord(doc);
        for (const auto &field : fields) {
            auto *row = new QHBoxLayout;
            auto *label = new QLabel(owner.tr("%1 (%2)").arg(text(field.name), text(field.type)), tables);
            label->setObjectName(Prefix + QStringLiteral("record.") + namePart(field.name));
            row->addWidget(label, 1);
            auto *remove = smallButton(QStringLiteral("\u00d7"),
                                       Prefix + QStringLiteral("record.") + namePart(field.name) +
                                           QStringLiteral(".remove"),
                                       owner.tr("Remove this record field"), tables);
            row->addWidget(remove);
            layout->addLayout(row);
            const auto name = field.name;
            QObject::connect(remove, &QPushButton::clicked, &owner, [this, name] {
                if (read_state) apply(planRemoveRecordField(*read_state, name, tasks, projectRoot()));
            });
        }
        auto *add = new QPushButton(QStringLiteral("+"), tables);
        add->setObjectName(Prefix + QStringLiteral("record.add"));
        add->setMaximumWidth(32);
        add->setToolTip(owner.tr("Add a record field: every assignment and procedural asset gets it, at 0"));
        layout->addWidget(add, 0, Qt::AlignLeft);
        QObject::connect(add, &QPushButton::clicked, &owner, [this] { openAdd(AddKind::record_field); });
        // The engine record: what the engine puts before the record fields.
        auto *engine = new QHBoxLayout;
        engine->addWidget(new QLabel(owner.tr("Engine record:"), tables));
        const auto chosen = rayLayoutEngineRecord(doc);
        for (const auto &each : rayEngineRecordFields()) {
            auto *check = new QCheckBox(text(each), tables);
            check->setObjectName(Prefix + QStringLiteral("engineRecord.") + text(each));
            check->setChecked(std::find(chosen.begin(), chosen.end(), each) != chosen.end());
            engine->addWidget(check);
            QObject::connect(check, &QCheckBox::toggled, &owner, [this, each](bool on) {
                if (!read_state) return;
                auto fields = rayLayoutEngineRecord(read_state->layout.document);
                fields.erase(std::remove(fields.begin(), fields.end(), each), fields.end());
                if (on) {
                    // In the engine's order.
                    std::vector<std::string> ordered;
                    for (const auto &known : rayEngineRecordFields())
                        if (known == each || std::find(fields.begin(), fields.end(), known) != fields.end())
                            ordered.push_back(known);
                    fields = ordered;
                }
                apply(planSetEngineRecord(*read_state, fields, tasks, projectRoot()));
            });
        }
        engine->addStretch(1);
        layout->addLayout(engine);
    }

    QWidget *recordEditor(const RayRecordField &field, const Json &value, const QString &name,
                          std::function<void(Json)> changed) {
        auto *box = new QWidget(tables);
        auto *row = new QHBoxLayout(box);
        row->setContentsMargins(0, 0, 0, 0);
        int count = field.type == "vec2" ? 2 : field.type == "vec3" ? 3 : field.type == "vec4" ? 4 : 1;
        std::vector<QDoubleSpinBox *> spins;
        for (int index = 0; index < count; ++index) {
            auto *spin = new QDoubleSpinBox(box);
            spin->setObjectName(count == 1 ? name : name + QStringLiteral(".%1").arg(index));
            spin->setRange(field.type == "u32" ? 0.0 : -1.0e9, 1.0e9);
            spin->setDecimals(field.type == "i32" || field.type == "u32" ? 0 : 3);
            spin->setKeyboardTracking(false);
            double current = 0.0;
            if (count == 1 && value.is_number()) current = value.get<double>();
            if (count > 1 && value.is_array() && static_cast<int>(value.size()) > index && value[index].is_number())
                current = value[index].get<double>();
            spin->setValue(current);
            row->addWidget(spin);
            spins.push_back(spin);
        }
        for (auto *spin : spins)
            QObject::connect(spin, &QDoubleSpinBox::valueChanged, box, [spins, field, changed] {
                Json out;
                if (spins.size() == 1) {
                    if (field.type == "f32") out = spins.front()->value();
                    else out = static_cast<std::int64_t>(spins.front()->value());
                } else {
                    out = Json::array();
                    for (auto *each : spins) out.push_back(each->value());
                }
                changed(std::move(out));
            });
        return box;
    }

    // Decision 4: the Hit group assignments.
    void buildAssignments(QVBoxLayout *layout) {
        const auto &read = *read_state;
        const auto &doc = read.layout.document;
        layout->addWidget(heading(owner.tr("Hit group assignments"), tables));
        const auto bindings = rayBindingsDocument(read);
        const auto rows = bindings.value("bindings", Json::array());
        const auto groups = rayLayoutGroups(doc);
        const auto record = rayLayoutRecord(doc);
        const auto default_group = rayLayoutDefaultGroup(doc);
        auto *grid = new QGridLayout;
        int line = 0;
        if (read.materials.empty() && read.procedurals.empty()) {
            auto *none = new QLabel(owner.tr("No material or procedural asset to assign yet."), tables);
            none->setObjectName(Prefix + QStringLiteral("assign.none"));
            layout->addWidget(none);
        }
        for (std::size_t index = 0; index < read.materials.size(); ++index) {
            const auto &material = read.materials[index];
            const auto prefix = Prefix + QStringLiteral("assign.") +
                                namePart(material.name.empty() ? std::to_string(index) : material.name);
            // RTL-3s, decision 2 (engine EB-2): two rows of one name told apart
            // by their model and the objects drawn with them; grey when none is.
            const auto row_label = rayMaterialRowLabel(material);
            auto *label = new QLabel(text(row_label.text), tables);
            label->setObjectName(prefix + QStringLiteral(".material"));
            label->setToolTip(text(material.label));
            if (!row_label.used) label->setStyleSheet(QStringLiteral("color: palette(mid);"));
            grid->addWidget(label, line, 0);
            auto *combo = new QComboBox(tables);
            combo->setObjectName(prefix + QStringLiteral(".group"));
            combo->addItem(owner.tr("Default (%1)").arg(text(default_group)), QString{});
            for (const auto &group : groups)
                if (group.type == "triangles") combo->addItem(text(group.name), text(group.name));
            const auto found = rayBindingIndex(bindings, material.selector);
            const auto group = found ? rows[*found].value("group", std::string{}) : std::string{};
            combo->setCurrentIndex(std::max(0, combo->findData(text(group))));
            grid->addWidget(combo, line, 1);
            const auto selector = material.selector;
            QObject::connect(combo, &QComboBox::currentIndexChanged, &owner, [this, combo, selector] {
                if (read_state)
                    apply(planAssignMaterial(*read_state, selector, combo->currentData().toString().toStdString()));
            });
            int column = 2;
            for (const auto &field : record) {
                const auto value = found ? rows[*found].value("record", Json::object()).value(field.name, Json(nullptr))
                                         : Json(nullptr);
                auto *editor = recordEditor(field, value, prefix + QStringLiteral(".record.") + namePart(field.name),
                                            [this, selector, name = field.name](Json changed) {
                                                if (read_state)
                                                    apply(planSetMaterialRecord(*read_state, selector, name, changed));
                                            });
                editor->setEnabled(found.has_value());
                editor->setToolTip(found ? text(field.name) : owner.tr("Assign a hit group to set its record"));
                grid->addWidget(editor, line, column++);
            }
            ++line;
        }
        for (const auto &asset : read.procedurals) {
            const auto prefix = Prefix + QStringLiteral("assignProcedural.") + namePart(asset.asset);
            auto *label = new QLabel(owner.tr("%1 (procedural)").arg(text(asset.asset)), tables);
            label->setObjectName(prefix + QStringLiteral(".asset"));
            grid->addWidget(label, line, 0);
            auto *combo = new QComboBox(tables);
            combo->setObjectName(prefix + QStringLiteral(".group"));
            combo->addItem(owner.tr("(none)"), QString{});
            for (const auto &group : groups)
                if (group.type == "procedural") combo->addItem(text(group.name), text(group.name));
            std::string group;
            for (const auto &row : rows)
                if (row.is_object() && row.value("procedural", std::string{}) == asset.asset)
                    group = row.value("group", std::string{});
            combo->setCurrentIndex(std::max(0, combo->findData(text(group))));
            grid->addWidget(combo, line, 1);
            const auto name = asset.asset;
            QObject::connect(combo, &QComboBox::currentIndexChanged, &owner, [this, combo, name] {
                if (read_state)
                    apply(planAssignProcedural(*read_state, name, combo->currentData().toString().toStdString()));
            });
            int column = 2;
            for (const auto &field : record) {
                auto *editor = recordEditor(field, asset.record_fields.value(field.name, Json(nullptr)),
                                            prefix + QStringLiteral(".record.") + namePart(field.name),
                                            [this, name, field_name = field.name](Json changed) {
                                                if (read_state)
                                                    apply(planSetProceduralRecord(*read_state, name, field_name,
                                                                                  changed));
                                            });
                grid->addWidget(editor, line, column++);
            }
            ++line;
        }
        grid->setColumnStretch(0, 1);
        layout->addLayout(grid);
        // Assignments no scene row matches: an orange mark and a sentence.
        for (std::size_t index = 0; index < read.unresolved.size(); ++index) {
            const auto &unresolved = read.unresolved[index];
            auto *row = new QHBoxLayout;
            auto *label = new QLabel(owner.tr("\u25cf %1 \u2192 %2: nothing in the scene uses this assignment's "
                                              "material.")
                                         .arg(text(raySelectorLabel(read, unresolved.selector)),
                                              text(unresolved.group)),
                                     tables);
            label->setObjectName(Prefix + QStringLiteral("assign.unresolved%1").arg(index));
            label->setStyleSheet(QStringLiteral("color: #c77700;"));
            label->setWordWrap(true);
            row->addWidget(label, 1);
            auto *remove = smallButton(QStringLiteral("\u00d7"),
                                       Prefix + QStringLiteral("assign.unresolved%1.remove").arg(index),
                                       owner.tr("Remove this assignment"), tables);
            row->addWidget(remove);
            layout->addLayout(row);
            const auto binding = static_cast<std::size_t>(unresolved.index);
            QObject::connect(remove, &QPushButton::clicked, &owner, [this, binding] {
                if (read_state) apply(planRemoveBinding(*read_state, binding));
            });
        }
    }

    void documentMoved(const std::string &) {
        if (owner.isVisible() || read_state) read();
    }
};

RayLayoutWidget::RayLayoutWidget(EmbeddedViewport *viewport, QWidget *parent) : QWidget{parent} {
    if (viewport == nullptr) throw std::invalid_argument("RayLayoutWidget requires an embedded viewport");
    impl_ = std::make_unique<Impl>(*this, *viewport);
}

RayLayoutWidget::~RayLayoutWidget() = default;

void RayLayoutWidget::setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> digest) {
    impl_->client.setRenderConfigDigest(digest);
    impl_->digest_hub = std::move(digest);
    if (impl_->digest_hub)
        impl_->digest_hub->subscribe(this, [impl = impl_.get()](const std::string &moved) { impl->documentMoved(moved); });
}

void RayLayoutWidget::setEngineRestartAction(std::function<void(const QString &reason)> action) {
    impl_->engine_restart_action = std::move(action);
}

void RayLayoutWidget::engineRestartDeclined(const QString &message) {
    if (!std::exchange(impl_->restart_asked, false)) return;
    impl_->read();
    impl_->showInformation(message);
}

void RayLayoutWidget::setShowDocumentAction(std::function<void(const QString &reference)> action) {
    impl_->show_document_action = std::move(action);
    impl_->controls();
}

void RayLayoutWidget::setAccelerationStructuresAction(std::function<void()> action) {
    impl_->acceleration_action = std::move(action);
    impl_->controls();
}

void RayLayoutWidget::revealMaterial(const QString &material) {
    auto &impl = *impl_;
    if (!impl.read_state) {
        impl.pending_reveal = material;
        impl.read();
        return;
    }
    if (!impl.read_state->layout.usable()) {
        impl.showInformation(tr("Enable hit groups first: a hit group is assigned in the project's ray layout."));
        return;
    }
    const auto wanted = material.toStdString();
    for (const auto &entry : impl.read_state->materials) {
        const bool by_document = entry.selector.is_object() && entry.selector.contains("document") &&
                                 sameRenderDocument(entry.selector.value("document", std::string{}), wanted);
        if (!by_document && entry.name != wanted && entry.label != wanted) continue;
        const auto name = QStringLiteral("pelican.rayLayout.assign.") +
                          namePart(entry.name.empty() ? std::string{"0"} : entry.name) + QStringLiteral(".group");
        if (auto *combo = findChild<QComboBox *>(name)) {
            for (QWidget *parent = parentWidget(); parent != nullptr; parent = parent->parentWidget())
                if (auto *scroll = qobject_cast<QScrollArea *>(parent)) {
                    scroll->ensureWidgetVisible(combo);
                    break;
                }
            combo->setFocus();
            impl.showInformation(tr("Pick the hit group of %1.").arg(text(entry.label.empty() ? entry.name : entry.label)));
            return;
        }
    }
    impl.showInformation(tr("%1 is not a material the engine lists for the ray layout.").arg(material));
}

void RayLayoutWidget::refresh() { impl_->read(); }

void RayLayoutWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (!event->spontaneous()) impl_->read();
}

void RayLayoutWidget::revealEnable() {
    auto *button = impl_->enable_button;
    for (QWidget *parent = parentWidget(); parent != nullptr; parent = parent->parentWidget())
        if (auto *scroll = qobject_cast<QScrollArea *>(parent)) {
            scroll->ensureWidgetVisible(button);
            break;
        }
    if (button->isVisible()) {
        button->setFocus();
        impl_->showInformation(tr("Enable hit groups gives the project a ray layout; Ray geometry needs one."));
    }
}

void RayLayoutWidget::setRefusalAction(std::function<void(const nlohmann::json &, const std::string &)> action) {
    impl_->refusal_action = std::move(action);
}

void RayLayoutWidget::setShaderHistoryAction(std::function<void(std::vector<RenderEditHistory::FileChange>)> action) {
    impl_->shader_history_action = std::move(action);
}

} // namespace PelicanStudio
