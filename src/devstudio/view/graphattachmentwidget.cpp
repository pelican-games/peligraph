#include "graphattachmentwidget.hpp"
#include "jsonrpcrequest.hpp"

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
#include <QTimer>
#include <QVBoxLayout>

#include <limits>
#include <map>

namespace PelicanStudio {

struct GraphAttachmentWidget::Impl {
    GraphAttachmentWidget &view;
    GraphAttachmentModel model;
    QListWidget *instances;
    QPushButton *add;
    QPushButton *remove;
    QPushButton *refresh;
    QLabel *notice;
    std::uint64_t rendered_revision = std::numeric_limits<std::uint64_t>::max();
    QPointer<QDialog> dialog;
    QComboBox *definitions = nullptr;
    QLineEdit *instance_id = nullptr;
    QFormLayout *parameters = nullptr;
    QLabel *draft_notice = nullptr;
    QPushButton *submit = nullptr;
    QPushButton *refresh_draft = nullptr;
    std::map<std::string, QLineEdit *> values;
    bool submitted = false;

    explicit Impl(GraphAttachmentWidget &widget) : view(widget) {
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
        QObject::connect(instances, &QListWidget::currentRowChanged, &view,
                         [this] { synchronize(); });
        auto *poll = new QTimer(&view);
        poll->setInterval(100);
        QObject::connect(poll, &QTimer::timeout, &view, [this] {
            model.poll();
            dispatch();
        });
        poll->start();
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

void GraphAttachmentWidget::setConnected(bool connected) {
    if (!connected && impl_->dialog) impl_->dialog->reject();
    impl_->model.setConnected(connected);
    impl_->synchronize();
    impl_->dispatch();
}
void GraphAttachmentWidget::setOwner(std::optional<GraphAttachmentOwner> owner) {
    if (impl_->model.owner() != owner && impl_->dialog) impl_->dialog->reject();
    impl_->model.selectOwner(std::move(owner));
    impl_->synchronize();
    impl_->dispatch();
}
void GraphAttachmentWidget::receiveRpcResult(quint64 id, const QByteArray &result) {
    impl_->model.receiveRpcResult(id, std::string_view(result.constData(), result.size()));
    impl_->synchronize();
    impl_->dispatch();
}
void GraphAttachmentWidget::receiveRpcFailure(quint64 id, const QString &message) {
    impl_->model.receiveRpcFailure(id, message.toStdString());
    impl_->synchronize();
    impl_->dispatch();
}
} // namespace PelicanStudio
