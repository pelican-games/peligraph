#pragma once

#include "../model/graphattachmentmodel.hpp"

#include <QWidget>
#include <QJsonObject>
#include <memory>

namespace PelicanStudio {

class GraphAttachmentWidget final : public QWidget {
    Q_OBJECT
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit GraphAttachmentWidget(QWidget *parent = nullptr);
    ~GraphAttachmentWidget() override;
    void setConnected(bool connected);
    void setOwner(std::optional<GraphAttachmentOwner> owner);
    void receiveRpcResult(quint64 id, const QByteArray &result);
    void receiveRpcFailure(quint64 id, const QString &message);

  signals:
    void rpcRequested(quint64 id, const QString &method, const QJsonObject &params);
};

} // namespace PelicanStudio
