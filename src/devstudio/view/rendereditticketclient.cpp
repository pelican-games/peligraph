#include "rendereditticketclient.hpp"
#include "embeddedviewport.hpp"
#include "jsonrpcrequest.hpp"
#include "renderconfigdigest.hpp"
#include <QTimer>

namespace PelicanStudio {
RenderEditTicketClient::RenderEditTicketClient(EmbeddedViewport &viewport, QObject *context)
    : viewport_(viewport), context_(context),
      edit_({[this](const std::string &method, const Json &params, std::string *error) {
                 try {
                     QString message;
                     const auto id = viewport_.requestRpc(QString::fromStdString(method),
                                                         exactJsonRpcObject(params), &message);
                     if (error) *error = message.toStdString();
                     return static_cast<std::int64_t>(id);
                 } catch (const std::exception &exception) {
                     if (error) *error = exception.what();
                     return std::int64_t{0};
                 }
             },
             [this](int ms, std::function<void()> task) {
                 QTimer::singleShot(ms, this, std::move(task));
             }},
            [this](const std::string &digest) {
                if (hub_) hub_->publish(digest, context_);
            }) {
    edit_.changed = [this](const Outcome &outcome) {
        if (completed) completed(outcome);
    };
    connect(&viewport_, &EmbeddedViewport::inspectorRpcSucceeded, this,
            [this](qint64 id, const QByteArray &bytes) { receive(id, bytes); });
    connect(&viewport_, &EmbeddedViewport::inspectorRpcFailed, this,
            [this](qint64 id, const QString &error) { fail(id, error); });
    connect(&viewport_, &EmbeddedViewport::engineRpcBecameUnavailable, this,
            [this](const QString &error) {
                abandon();
                if (unavailable) unavailable(error);
            });
}

void RenderEditTicketClient::setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> hub) {
    hub_ = std::move(hub);
}

void RenderEditTicketClient::abandon() {
    reads_.clear();
    edit_.abandon();
}

bool RenderEditTicketClient::submit(const QString &method, const Json &params) {
    if (!reads_.empty()) return false;
    return edit_.submit(method.toStdString(), params);
}

bool RenderEditTicketClient::read(const QString &method, const Json &params,
                                std::function<void(const Json &)> result,
                                std::function<void(const QString &)> failure) {
    QString error;
    qint64 id = 0;
    try {
        id = viewport_.requestRpc(method, exactJsonRpcObject(params), &error);
    } catch (const std::exception &exception) {
        error = QString::fromUtf8(exception.what());
    }
    if (id) {
        reads_.emplace(id, Read{std::move(result), std::move(failure)});
        return true;
    }
    if (failure) failure(error);
    else if (failed) failed(error);
    return false;
}

void RenderEditTicketClient::receive(qint64 id, const QByteArray &bytes) {
    const auto json = Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
    if (auto found = reads_.find(id); found != reads_.end()) {
        auto read = std::move(found->second);
        reads_.erase(found);
        if (json.is_object()) {
            if (read.result) read.result(json);
        } else if (read.failure) read.failure(tr("The engine answered something that is not an object."));
        else if (failed) failed(tr("The engine answered something that is not an object."));
        return;
    }
    if (edit_.receive(id, json).kind == Kind::NotMine && unhandledResult)
        unhandledResult(id, bytes);
}

void RenderEditTicketClient::fail(qint64 id, const QString &error) {
    if (auto found = reads_.find(id); found != reads_.end()) {
        auto read = std::move(found->second);
        reads_.erase(found);
        if (read.failure) read.failure(error);
        else if (failed) failed(error);
        return;
    }
    if (edit_.fail(id, error.toStdString()).kind == Kind::NotMine && unhandledFailure)
        unhandledFailure(id, error);
}
} // namespace PelicanStudio
