#include "runtimeoutliner.hpp"

#include "outlinertree.hpp"
#include "../viewport/embeddedviewport.hpp"

#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPalette>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidgetItem>
#include <QVariant>

#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

// JSON-RPC's "method not found": an engine from before RO-1.
constexpr int UnknownMethodCode = -32601;

Json parsed(const QByteArray &bytes) {
    return Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
}

qulonglong packed(const RuntimeObjectEntity &entity) {
    return (static_cast<qulonglong>(entity.index) << 32U) | entity.generation;
}

} // namespace

QString RuntimeOutliner::editRefusal() {
    return tr("Objects spawned while the game runs cannot be edited.");
}

RuntimeOutliner::RuntimeOutliner(EmbeddedViewport &viewport, OutlinerTree &tree,
                                 std::function<void()> changed, QObject *parent)
    : QObject{parent}, viewport_{viewport}, tree_{tree}, changed_{std::move(changed)} {
    read_timer_ = new QTimer(this);
    read_timer_->setObjectName(QStringLiteral("pelican.outliner.runtimeReadTimer"));
    read_timer_->setInterval(RuntimeObjectsModel::PlayingReadIntervalMs);
    connect(read_timer_, &QTimer::timeout, this, [this] {
        if (model_.tick()) dispatch();
    });
    // Every get_status reply, whoever asked: the play controls' reads after
    // Pause, Step, Play and Stop are the ones that move the state.
    // UX-7, decision 10: the viewport's one parse of it.
    connect(&viewport_, &EmbeddedViewport::engineStatusParsed, this,
            [this](qint64, const nlohmann::json &status) {
                if (model_.observeStatus(status)) dispatch();
            });
    connect(&viewport_, &EmbeddedViewport::engineRpcBecameAvailable, this, [this] {
        model_.connect();
        dispatch();
    });
    connect(&viewport_, &EmbeddedViewport::engineRpcBecameUnavailable, this,
            [this](const QString &) {
                transport_.clear();
                unknown_method_.clear();
                model_.disconnect();
                synchronize();
            });
    connect(&viewport_, &EmbeddedViewport::engineRpcRefused, this,
            [this](qint64 transport_id, int code, const QString &, const QJsonValue &) {
                if (code == UnknownMethodCode && transport_.contains(transport_id))
                    unknown_method_.insert(transport_id);
            });
    connect(&viewport_, &EmbeddedViewport::inspectorRpcSucceeded, this,
            [this](qint64 transport_id, const QByteArray &result_json) {
                const auto found = transport_.find(transport_id);
                if (found == transport_.end()) return;
                const auto id = found.value();
                transport_.erase(found);
                model_.receiveResult(id, parsed(result_json));
                dispatch();
            });
    connect(&viewport_, &EmbeddedViewport::inspectorRpcFailed, this,
            [this](qint64 transport_id, const QString &) {
                const auto found = transport_.find(transport_id);
                if (found == transport_.end()) return;
                const auto id = found.value();
                transport_.erase(found);
                model_.receiveFailure(id, unknown_method_.remove(transport_id));
                dispatch();
            });
}

void RuntimeOutliner::dispatch() {
    for (auto &request : model_.takeRequests()) {
        QString error;
        const QJsonObject params =
            QJsonDocument::fromJson(QByteArray::fromStdString(request.params.dump())).object();
        const qint64 transport_id =
            viewport_.requestRpc(QString::fromStdString(request.method), params, &error);
        if (transport_id == 0) {
            model_.receiveFailure(request.request_id, false);
            continue;
        }
        transport_.insert(transport_id, request.request_id);
    }
    synchronize();
}

void RuntimeOutliner::synchronize() {
    if (model_.tickWanted()) {
        if (!read_timer_->isActive()) read_timer_->start();
    } else {
        read_timer_->stop();
    }
    if (model_.revision() == shown_revision_) return;
    shown_revision_ = model_.revision();
    // A new reading of the same objects (a moving one's transform) leaves
    // the rows as they are: a menu or a press on one is not pulled away.
    if (rowsSignature() != shown_rows_) appendRows();
    if (changed_) changed_();
}

QStringList RuntimeOutliner::rowsSignature() const {
    QStringList signature{QString::fromStdString(model_.sceneId())};
    for (const RuntimeObject &object : model_.objects())
        signature << QStringLiteral("%1/%2/%3/%4")
                         .arg(object.entity.index)
                         .arg(object.entity.generation)
                         .arg(QString::fromStdString(object.name),
                              QString::fromStdString(object.prefab));
    return signature;
}

void RuntimeOutliner::removeRows() {
    std::vector<QTreeWidgetItem *> rows;
    for (int top = 0; top < tree_.topLevelItemCount(); ++top) {
        QTreeWidgetItem *scene = tree_.topLevelItem(top);
        for (int child = 0; child < scene->childCount(); ++child)
            if (outlinerRuntimeEntity(scene->child(child))) rows.push_back(scene->child(child));
    }
    for (QTreeWidgetItem *row : rows) delete row;
}

void RuntimeOutliner::appendRows() {
    const QSignalBlocker block{&tree_};
    removeRows();
    shown_rows_ = rowsSignature();
    if (model_.objects().empty() || tree_.topLevelItemCount() == 0) return;
    // Under the scene the engine runs; the first one while get_status has
    // not named it.
    QTreeWidgetItem *scene = tree_.topLevelItem(0);
    const QString running = QString::fromStdString(model_.sceneId());
    for (int top = 0; top < tree_.topLevelItemCount(); ++top) {
        if (tree_.topLevelItem(top)->data(0, OutlinerSceneIdRole).toString() == running) {
            scene = tree_.topLevelItem(top);
            break;
        }
    }
    const QColor grey = tree_.palette().color(QPalette::Disabled, QPalette::Text);
    QTreeWidgetItem *current = nullptr;
    for (const RuntimeObject &object : model_.objects()) {
        auto *row = new QTreeWidgetItem(
            QStringList{tr("%1 (runtime)").arg(QString::fromStdString(object.name))});
        // No scene id: the Outliner's drag refuses a row of no scene, so no
        // declared object can be dropped under this one.
        row->setData(0, OutlinerRuntimeEntityRole, QVariant::fromValue(packed(object.entity)));
        // Selectable, for the Inspector's read-only view; nothing else.
        row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        row->setForeground(0, grey);
        QFont font = row->font(0);
        font.setItalic(true);
        row->setFont(0, font);
        const QString prefab = object.prefab.empty()
                                   ? tr("Spawned while the game runs.")
                                   : tr("Spawned from the prefab %1 while the game runs.")
                                         .arg(QString::fromStdString(object.prefab));
        row->setToolTip(0, prefab + QLatin1Char('\n') + editRefusal());
        scene->addChild(row);
        if (model_.selected() && *model_.selected() == object.entity) current = row;
    }
    if (current != nullptr) tree_.setCurrentItem(current);
}

QTreeWidgetItem *RuntimeOutliner::rowOf(const RuntimeObjectEntity &entity) const {
    for (int top = 0; top < tree_.topLevelItemCount(); ++top) {
        QTreeWidgetItem *scene = tree_.topLevelItem(top);
        for (int child = 0; child < scene->childCount(); ++child) {
            const auto row_entity = outlinerRuntimeEntity(scene->child(child));
            if (row_entity && *row_entity == entity) return scene->child(child);
        }
    }
    return nullptr;
}

} // namespace PelicanStudio
