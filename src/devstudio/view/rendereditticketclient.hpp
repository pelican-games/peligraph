#pragma once

#include "rendereditticket.hpp"
#include <QObject>
#include <QPointer>
#include <QString>
#include <map>
#include <memory>

namespace PelicanStudio {
class EmbeddedViewport;
class RenderConfigDigest;

// One Qt boundary for edits and the context reads used to build them. Context
// handlers stay with the caller; the adapter only owns their response IDs.
class RenderEditTicketClient final : public QObject {
public:
    using Json = RenderEditTicket::Json;
    using Outcome = RenderEditTicket::Outcome;
    using Kind = RenderEditTicket::Kind;
    RenderEditTicketClient(EmbeddedViewport &viewport, QObject *context);
    bool submit(const QString &method, const Json &params);
    bool read(const QString &method, const Json &params,
              std::function<void(const Json &)> result,
              std::function<void(const QString &)> failure = {});
    void resume() { edit_.resume(); }
    void abandon();
    bool busy() const { return edit_.busy() || !reads_.empty(); }
    bool editing() const { return edit_.busy(); }
    bool awaitingResponse() const { return edit_.awaitingResponse() || !reads_.empty(); }
    void setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> hub);
    std::function<void(const Outcome &)> completed;
    std::function<void(const QString &)> failed;
    std::function<void(const QString &)> unavailable;
    // Reads in separate, independent slots (providers, ray layout/tasks).
    std::function<void(qint64, const QByteArray &)> unhandledResult;
    std::function<void(qint64, const QString &)> unhandledFailure;
private:
    EmbeddedViewport &viewport_;
    QPointer<QObject> context_;
    std::shared_ptr<RenderConfigDigest> hub_;
    RenderEditTicket edit_;
    struct Read {
        std::function<void(const Json &)> result;
        std::function<void(const QString &)> failure;
    };
    std::map<qint64, Read> reads_;
    void receive(qint64 id, const QByteArray &bytes);
    void fail(qint64 id, const QString &error);
};
} // namespace PelicanStudio
