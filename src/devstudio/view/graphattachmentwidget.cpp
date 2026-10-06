#include "graphattachmentwidget.hpp"
#include "jsonrpcrequest.hpp"
#include "renderconfigdigest.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace PelicanStudio {

struct GraphAttachmentWidget::Impl {
    GraphAttachmentWidget &view;
    GraphAttachmentModel model;
    QListWidget *instances;
    QPushButton *add;
    QPushButton *remove;
    QPushButton *refresh;
    QTimer *poll_timer = nullptr;
    QLabel *notice;
    QGroupBox *ports;
    QFormLayout *port_rows;
    std::map<std::string, QLineEdit *> port_values;
    std::string ports_instance;
    std::uint64_t rendered_ports_revision = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t rendered_revision = std::numeric_limits<std::uint64_t>::max();
    QPointer<QDialog> dialog;
    QPointer<QDialog> connect_dialog;
    QComboBox *definitions = nullptr;
    QLineEdit *instance_id = nullptr;
    QFormLayout *parameters = nullptr;
    QLabel *draft_notice = nullptr;
    QPushButton *submit = nullptr;
    QPushButton *refresh_draft = nullptr;
    std::map<std::string, QLineEdit *> values;
    bool submitted = false;

    explicit Impl(GraphAttachmentWidget &widget) : view(widget) {
        // Ticket polls use the component's 25 / 500 ms schedule. The older
        // dispatch clock only flushes reads queued by synchronize() when it
        // starts watching an instance; it never polls an edit ticket.
        model.scheduleEdit = [this](int ms, std::function<void()> task) {
            QTimer::singleShot(ms, &view, [this, task = std::move(task)] {
                task();
                synchronize();
                dispatch();
            });
        };
        poll_timer = new QTimer(&view);
        poll_timer->setObjectName(
            QStringLiteral("pelican.graphAttachmentPollTimer"));
        poll_timer->setInterval(100);
        QObject::connect(poll_timer, &QTimer::timeout, &view, [this] {
            dispatch();
        });
        poll_timer->start();
        auto *layout = new QVBoxLayout(&view);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *group = new QGroupBox(view.tr("Attached graphs"), &view);
        auto *contents = new QVBoxLayout(group);
        instances = new QListWidget(group);
        instances->setObjectName("pelican.graphAttachments");
        instances->setMaximumHeight(110);
        contents->addWidget(instances);
        auto *buttons = new QHBoxLayout;
        add = new QPushButton(view.tr("Attach..."), group);
        add->setObjectName("pelican.graphAttach");
        remove = new QPushButton(view.tr("Detach"), group);
        remove->setObjectName("pelican.graphDetach");
        refresh = new QPushButton(view.tr("Refresh"), group);
        refresh->setObjectName("pelican.graphRefresh");
        buttons->addWidget(add);
        buttons->addWidget(remove);
        buttons->addWidget(refresh);
        contents->addLayout(buttons);
        notice = new QLabel(group);
        notice->setObjectName("pelican.graphNotice");
        notice->setTextFormat(Qt::PlainText);
        notice->setWordWrap(true);
        contents->addWidget(notice);
        layout->addWidget(group);
        // The input ports of the selected attachment. The list above says what
        // is attached; this says where each of its inputs gets its value.
        ports = new QGroupBox(view.tr("Instance inputs"), &view);
        ports->setObjectName("pelican.graphInstanceInputs");
        port_rows = new QFormLayout(ports);
        ports->setVisible(false);
        layout->addWidget(ports);
        QObject::connect(add, &QPushButton::clicked, &view, [this] { openDialog(); });
        QObject::connect(remove, &QPushButton::clicked, &view, [this] {
            if (auto *item = instances->currentItem())
                model.detach(item->data(Qt::UserRole).toString().toStdString());
            synchronize();
            dispatch();
        });
        QObject::connect(refresh, &QPushButton::clicked, &view, [this] {
            model.refresh();
            synchronize();
            dispatch();
        });
        QObject::connect(instances, &QListWidget::currentRowChanged, &view, [this] {
            auto *item = instances->currentItem();
            model.watchInstance(item ? item->data(Qt::UserRole).toString().toStdString() : std::string{});
            rendered_ports_revision = std::numeric_limits<std::uint64_t>::max();
            synchronize();
            dispatch();
        });
        synchronize();
    }

    void dispatch() {
        for (const auto &request : model.takeRpcRequests()) {
            try {
                emit view.rpcRequested(request.request_id,
                    QString::fromStdString(request.method), exactJsonRpcObject(request.params));
            } catch (const std::exception &error) {
                model.receiveRpcFailure(request.request_id, error.what());
            }
        }
        synchronize();
    }

    void synchronize() {
        if (rendered_revision != model.contentRevision()) {
            rendered_revision = model.contentRevision();
            const auto selected = instances->currentItem()
                ? instances->currentItem()->data(Qt::UserRole).toString() : QString{};
            {
                // Refilling the list passes through "nothing selected". That is
                // not the user deselecting anything, so it must not be reported
                // as a selection change and throw away what is being shown.
                const QSignalBlocker blocker{instances};
                instances->clear();
                for (const auto &instance : model.instances()) {
                    const auto id = QString::fromStdString(instance.at("id").get<std::string>());
                    auto *item = new QListWidgetItem(id + " — " +
                        QString::fromStdString(instance.at("definition").get<std::string>()), instances);
                    item->setData(Qt::UserRole, id);
                    item->setToolTip(QString::fromStdString(instance.value("parameters", nlohmann::json::object()).dump(2)));
                    if (id == selected) instances->setCurrentItem(item);
                }
            }
            model.watchInstance(selectedInstance());
        }
        synchronizePorts();
        add->setEnabled(model.canEdit() && !model.definitions().empty());
        remove->setEnabled(model.canEdit() && instances->currentItem());
        refresh->setEnabled(model.canRefresh());
        const auto message = QString::fromStdString(model.notice().message);
        notice->setText(message.isEmpty() && model.owner() && model.canEdit() && model.definitions().empty()
            ? view.tr("No graph definitions in the current rendering document.") : message);
        if (dialog) {
            if (submitted && model.notice().kind == InspectorNoticeKind::Success) {
                dialog->accept();
            } else {
                definitions->setEnabled(!model.busy());
                instance_id->setEnabled(!model.busy());
                for (const auto &[name, editor] : values) editor->setEnabled(!model.busy());
                submit->setEnabled(model.canEdit() && definitions->count() > 0);
                refresh_draft->setEnabled(model.canRefresh());
                if (!message.isEmpty()) draft_notice->setText(message);
            }
        }
    }

    std::string selectedInstance() const {
        auto *item = instances->currentItem();
        return item ? item->data(Qt::UserRole).toString().toStdString() : std::string{};
    }

    // One row per input port of the selected attachment: what supplies it, what
    // it is worth right now, and the four ways to change where it comes from.
    void synchronizePorts() {
        const auto instance = selectedInstance();
        const auto revision = model.contentRevision() ^ (model.reportRevision() << 32U);
        const bool rebuild = instance != ports_instance || rendered_ports_revision != revision;
        ports_instance = instance;
        rendered_ports_revision = revision;
        const auto rows = instance.empty() ? std::vector<GraphInstanceInputRow>{}
                                           : model.instanceInputs(instance);
        ports->setVisible(!rows.empty());
        if (rebuild) {
            port_values.clear();
            while (port_rows->rowCount()) port_rows->removeRow(0);
            for (const auto &row : rows) addPortRow(instance, row);
        }
        for (const auto &row : rows) {
            const auto editor = port_values.find(row.port);
            if (editor == port_values.end()) continue;
            editor->second->setEnabled(model.canEdit() && row.has_stored);
        }
    }

    void addPortRow(const std::string &instance, const GraphInstanceInputRow &row) {
        auto *field = new QWidget(ports);
        auto *line = new QHBoxLayout(field);
        line->setContentsMargins(0, 0, 0, 0);
        auto *mode = new QLabel(QString::fromStdString(row.mode), field);
        mode->setObjectName(QString::fromStdString("pelican.graphPort.mode." + row.port));
        line->addWidget(mode);
        auto *editor = new QLineEdit(field);
        editor->setObjectName(QString::fromStdString("pelican.graphPort.value." + row.port));
        editor->setText(row.has_stored ? QString::number(row.stored, 'g', 17) : QString{});
        line->addWidget(editor);
        port_values.emplace(row.port, editor);
        // What the drawn generation actually used, with where it came from.
        auto *live = new QLabel(field);
        live->setObjectName(QString::fromStdString("pelican.graphPort.live." + row.port));
        live->setText(row.has_value
            ? QString::number(row.value, 'g', 17) + " (" + QString::fromStdString(row.value_source) + ")"
            : view.tr("not published"));
        line->addWidget(live);
        const auto button = [&](const char *suffix, const QString &text) {
            auto *action = new QPushButton(text, field);
            action->setObjectName(QString::fromStdString("pelican.graphPort." + std::string{suffix} + "." + row.port));
            line->addWidget(action);
            return action;
        };
        const auto port = row.port;
        QObject::connect(editor, &QLineEdit::editingFinished, &view, [this, instance, port, editor] {
            bool valid = false;
            const auto value = QLocale::c().toDouble(editor->text(), &valid);
            if (!valid) return;
            model.setInstanceParameter(instance, port, value);
            synchronize();
            dispatch();
        });
        QObject::connect(button("curve", view.tr("Control with a curve")), &QPushButton::clicked, &view,
            [this, instance, port] {
                model.controlWithCurve(instance, port);
                synchronize();
                dispatch();
            });
        QObject::connect(button("connect", view.tr("Connect to another output")), &QPushButton::clicked, &view,
            [this, instance, port] { openConnectDialog(instance, port); });
        QObject::connect(button("reveal", view.tr("Open the source")), &QPushButton::clicked, &view,
            [this, row] {
                // v0 names the producing scope; the graph view that can select
                // it arrives with the edge-editing work.
                emit view.sourceRevealRequested(QString::fromStdString(row.source_node),
                                                QString::fromStdString(row.source_port));
                notice->setText(row.mode == "connected"
                    ? view.tr("Source: %1.%2").arg(QString::fromStdString(row.source_node),
                                                   QString::fromStdString(row.source_port))
                    : view.tr("This input has no source to open; it is a stored value."));
            });
        auto *disconnect = button("disconnect", view.tr("Disconnect"));
        disconnect->setEnabled(row.mode == "connected");
        QObject::connect(disconnect, &QPushButton::clicked, &view, [this, instance, port] {
            model.disconnectInstanceInput(instance, port);
            synchronize();
            dispatch();
        });
        port_rows->addRow(QString::fromStdString(row.port), field);
    }

    void openConnectDialog(const std::string &instance, const std::string &port) {
        if (connect_dialog) { connect_dialog->raise(); return; }
        if (!model.canEdit()) return;
        model.requestCatalog();
        dispatch();
        connect_dialog = new QDialog(&view);
        auto *dialog = connect_dialog.data();
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName("pelican.graphConnectDialog");
        dialog->setWindowTitle(view.tr("Connect input"));
        auto *layout = new QVBoxLayout(dialog);
        auto *sources = new QComboBox(dialog);
        sources->setObjectName("pelican.graphConnectSource");
        const auto candidates = model.outputCandidates(instance);
        for (const auto &candidate : candidates)
            sources->addItem(QString::fromStdString(candidate.node + "." + candidate.port));
        layout->addWidget(sources);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        auto *apply = buttons->addButton(view.tr("Connect"), QDialogButtonBox::ActionRole);
        apply->setObjectName("pelican.graphConnectApply");
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        QObject::connect(apply, &QPushButton::clicked, dialog, [this, instance, port, sources, candidates] {
            const auto index = sources->currentIndex();
            if (index < 0 || static_cast<std::size_t>(index) >= candidates.size()) return;
            if (model.connectInstanceInput(instance, port, candidates[static_cast<std::size_t>(index)]) &&
                connect_dialog)
                connect_dialog->accept();
            synchronize();
            dispatch();
        });
        dialog->show();
    }

    void populateParameters() {
        values.clear();
        while (parameters->rowCount()) parameters->removeRow(0);
        draft_notice->clear();
        try {
            const auto definition = definitions->currentText().toStdString();
            instance_id->setText(QString::fromStdString(model.suggestInstanceId(definition)));
            const auto defaults = model.parameterDefaults(definition);
            for (const auto &[name, value] : defaults.items()) {
                auto *editor = new QLineEdit(dialog);
                editor->setObjectName(QString::fromStdString("pelican.graphParameter." + name));
                editor->setText(QString::number(value.get<double>(), 'g', 17));
                parameters->addRow(QString::fromStdString(name), editor);
                values.emplace(name, editor);
            }
        } catch (const std::exception &error) {
            draft_notice->setText(QString::fromUtf8(error.what()));
        }
    }

    void openDialog() {
        if (dialog) { dialog->raise(); return; }
        if (!model.canEdit()) return;
        submitted = false;
        dialog = new QDialog(&view);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName("pelican.graphAttachmentDialog");
        dialog->setWindowTitle(view.tr("Attach graph"));
        auto *layout = new QVBoxLayout(dialog);
        auto *form = new QFormLayout;
        definitions = new QComboBox(dialog);
        definitions->setObjectName("pelican.graphDefinition");
        for (const auto &[name, definition] : model.definitions().items())
            definitions->addItem(QString::fromStdString(name));
        instance_id = new QLineEdit(dialog);
        instance_id->setObjectName("pelican.graphInstanceId");
        form->addRow(view.tr("Definition"), definitions);
        form->addRow(view.tr("Instance name"), instance_id);
        layout->addLayout(form);
        parameters = new QFormLayout;
        layout->addLayout(parameters);
        draft_notice = new QLabel(dialog);
        draft_notice->setObjectName("pelican.graphDraftNotice");
        draft_notice->setTextFormat(Qt::PlainText);
        draft_notice->setWordWrap(true);
        layout->addWidget(draft_notice);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        refresh_draft = buttons->addButton(view.tr("Refresh source"), QDialogButtonBox::ActionRole);
        refresh_draft->setObjectName("pelican.graphDraftRefresh");
        submit = buttons->addButton(view.tr("Attach and save"), QDialogButtonBox::ActionRole);
        submit->setObjectName("pelican.graphAttachSave");
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        QObject::connect(refresh_draft, &QPushButton::clicked, dialog, [this] {
            model.refresh();
            synchronize();
            dispatch();
        });
        QObject::connect(definitions, &QComboBox::currentIndexChanged, dialog,
                         [this] { populateParameters(); });
        QObject::connect(submit, &QPushButton::clicked, dialog, [this] {
            auto initial = nlohmann::json::object();
            for (const auto &[name, editor] : values) {
                bool valid = false;
                const auto value = QLocale::c().toDouble(editor->text(), &valid);
                if (!valid) {
                    draft_notice->setText(view.tr("Enter a scalar number for %1.").arg(QString::fromStdString(name)));
                    return;
                }
                initial[name] = value;
            }
            submitted = model.attach(instance_id->text().toStdString(),
                definitions->currentText().toStdString(), std::move(initial));
            synchronize();
            dispatch();
        });
        populateParameters();
        synchronize();
        dialog->show();
    }
};

GraphAttachmentWidget::GraphAttachmentWidget(QWidget *parent)
    : QWidget(parent), impl_(std::make_unique<Impl>(*this)) {}
GraphAttachmentWidget::~GraphAttachmentWidget() = default;

void GraphAttachmentWidget::setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> hub) {
    impl_->model.publishDigest = [this, hub = std::move(hub)](const std::string &digest) {
        if (hub) hub->publish(digest, this);
    };
}

void GraphAttachmentWidget::setConnected(bool connected) {
    if (!connected && impl_->dialog) impl_->dialog->reject();
    if (!connected && impl_->connect_dialog) impl_->connect_dialog->reject();
    impl_->model.setConnected(connected);
    impl_->synchronize();
    impl_->dispatch();
}
void GraphAttachmentWidget::setOwner(std::optional<GraphAttachmentOwner> owner) {
    if (impl_->model.owner() != owner && impl_->dialog) impl_->dialog->reject();
    if (impl_->model.owner() != owner && impl_->connect_dialog) impl_->connect_dialog->reject();
    impl_->model.selectOwner(std::move(owner));
    impl_->synchronize();
    impl_->dispatch();
}
void GraphAttachmentWidget::receiveRpcResult(quint64 id, const QByteArray &result) {
    impl_->model.receiveRpcResult(id, std::string_view(result.constData(), result.size()));
    impl_->synchronize();
    impl_->dispatch();
    // RSX: last, since it can stop the engine the requests above go to.
    if (const auto line = impl_->model.takeEngineRestart())
        emit engineRestartRequested(QString::fromStdString(*line));
}
void GraphAttachmentWidget::receiveRpcFailure(quint64 id, const QString &message) {
    impl_->model.receiveRpcFailure(id, message.toStdString());
    impl_->synchronize();
    impl_->dispatch();
}
} // namespace PelicanStudio
