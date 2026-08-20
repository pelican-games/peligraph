#include "renderfeatureswidget.hpp"

#include "../viewport/embeddedviewport.hpp"

#include <QAbstractItemView>
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

void RenderFeaturesWidget::receiveCatalogResult(
    const QByteArray &result_json) {
    impl_->receiveCatalogResult(result_json);
}

} // namespace PelicanStudio
