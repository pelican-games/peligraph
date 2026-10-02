#include "planperfhub.hpp"

#include "../viewport/embeddedviewport.hpp"
#include "renderedithistory.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <nlohmann/json.hpp>

#include <type_traits>
#include <variant>

namespace PelicanStudio {
namespace {

constexpr int PollIntervalMs = 250;
// get_gpu_timing (each node's average over 30 frames, a few kilobytes) is what
// the line and the bands read, every two seconds while a panel shows them --
// the Frame Plan asks for it on its own clock, and its replies are read too.
constexpr qint64 TimingEveryMs = 2000;
// get_status carries every sample of the GPU window (a megabyte with a full
// window), which the engine writes and Studio parses on their main threads.
// It is read only while an edit waits for its after. Neither is read until
// the other panels have sent nothing for three seconds: the engine answers in
// order, and a megabyte written ahead of the document read that follows a
// commit kept the Graph from taking the next gesture (gui_build_optimize's
// drags fell on a tab still reading). get_status is then read every two
// seconds, for half a minute at most -- an edit whose after has not come by
// then made no generation, or was refused.
constexpr qint64 QuietAfterActivityMs = 3000;
constexpr qint64 WaitingStatusMs = 2000;
constexpr qint64 WaitingForMs = 30000;

std::optional<nlohmann::json> parsed(const QByteArray &bytes) {
    try {
        return nlohmann::json::parse(bytes.constData(), bytes.constData() + bytes.size());
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

// The requests whose name can say an edit that moves the plan.
bool editMethod(const QString &method) {
    return method == QLatin1String{"edit_graph_document"} ||
           method == QLatin1String{"restore_render_documents"} ||
           RenderEditHistory::recordsMethod(method.toStdString());
}

// What a panel reads after a commit, or polls while an edit is out.
bool workMethod(const QString &method) {
    static const QStringList methods{
        QStringLiteral("get_edit_result"),     QStringLiteral("get_graph_instances"),
        QStringLiteral("get_frame_plan"),      QStringLiteral("get_render_authoring_context"),
        QStringLiteral("get_render_documents"), QStringLiteral("reload_shaders"),
    };
    return methods.contains(method);
}

} // namespace

PlanPerfHub::PlanPerfHub(EmbeddedViewport &viewport, Hooks hooks, QObject *parent)
    : QObject{parent}, viewport_{viewport}, hooks_{std::move(hooks)} {
    setObjectName(QStringLiteral("pelican.planPerfHub"));
    timer_ = new QTimer(this);
    timer_->setObjectName(QStringLiteral("pelican.planPerfPollTimer"));
    timer_->setInterval(PollIntervalMs);
    connect(timer_, &QTimer::timeout, this, [this] { poll(); });
    timer_->start();
    // Every get_status reply, whoever asked: the GPU window is in each.
    connect(&viewport_, &EmbeddedViewport::engineStatusRead, this,
            [this](const QByteArray &result_json) {
                last_status_.start();
                readOffThread(result_json, Reply::Status);
            });
    connect(&viewport_, &EmbeddedViewport::inspectorRpcSucceeded, this,
            [this](qint64 request_id, const QByteArray &result_json) {
                if (request_id == 0) return;
                if (request_id == status_request_) status_request_ = 0;
                if (timing_reads_.remove(request_id)) {
                    if (request_id == timing_request_) timing_request_ = 0;
                    last_timing_.start();
                    readOffThread(result_json, Reply::Timing);
                    return;
                }
                const bool own = request_id == plan_request_;
                if (own) plan_request_ = 0;
                if (!plan_reads_.remove(request_id) && !own) return;
                readOffThread(result_json, Reply::Plan);
            });
    connect(&viewport_, &EmbeddedViewport::inspectorRpcFailed, this,
            [this](qint64 request_id, const QString &) {
                if (request_id == status_request_) status_request_ = 0;
                if (request_id == plan_request_) plan_request_ = 0;
                if (request_id == timing_request_) timing_request_ = 0;
                plan_reads_.remove(request_id);
                timing_reads_.remove(request_id);
            });
    connect(&viewport_, &EmbeddedViewport::engineRpcBecameUnavailable, this,
            [this](const QString &) {
                status_request_ = 0;
                plan_request_ = 0;
                timing_request_ = 0;
                plan_reads_.clear();
                timing_reads_.clear();
                plan_asked_for_.reset();
                // A reading of the engine that went may still be parsed: it
                // is dropped when it comes back.
                ++epoch_;
                model_.engineStopped();
                announce();
            });
}

void PlanPerfHub::requestGoing(const QString &method, const QJsonObject &params) {
    // Someone at work: an edit, its ticket, and the reads a commit is
    // followed by.
    if (editMethod(method) || workMethod(method)) last_activity_.start();
    if (!editMethod(method)) return;
    const auto json = parsed(QJsonDocument(params).toJson(QJsonDocument::Compact));
    if (!json) return;
    const auto label = PlanPerfModel::editLabel(method.toStdString(), *json);
    if (label.empty()) return;
    last_edit_.start();
    model_.noteEdit(label);
}

void PlanPerfHub::requestSent(qint64 id, const QString &method, const QJsonObject &params) {
    if (id == 0) return;
    // A page of a draw loop's materials is not the plan.
    if (method == QLatin1String{"get_frame_plan"} &&
        !params.contains(QStringLiteral("loop_elements")))
        plan_reads_.insert(id);
    else if (method == QLatin1String{"get_gpu_timing"})
        timing_reads_.insert(id);
}

void PlanPerfHub::poll() {
    if (!viewport_.rpcReady()) return;
    const bool watched = hooks_.watched && hooks_.watched();
    // While someone is at work the hub asks nothing: every read it adds goes
    // ahead of the reads that follow a commit, and a gesture waits on those.
    if (last_activity_.isValid() && last_activity_.elapsed() < QuietAfterActivityMs) return;
    if (watched && timing_request_ == 0 &&
        (!last_timing_.isValid() || last_timing_.elapsed() >= TimingEveryMs)) {
        timing_request_ = viewport_.requestRpc(QStringLiteral("get_gpu_timing"), QJsonObject{});
        last_timing_.start();
    }
    const bool waiting = model_.waiting() && last_edit_.isValid() &&
                         last_edit_.elapsed() < WaitingForMs;
    if (!waiting) return;
    if (status_request_ == 0 &&
        (!last_status_.isValid() || last_status_.elapsed() >= WaitingStatusMs)) {
        status_request_ = viewport_.requestRpc(QStringLiteral("get_status"), QJsonObject{});
        last_status_.start();
    }
    // An edit's after whose plan no panel read: once per generation.
    if (const auto generation = model_.planWanted();
        generation && plan_request_ == 0 && plan_asked_for_ != generation) {
        plan_asked_for_ = generation;
        plan_request_ = viewport_.requestRpc(QStringLiteral("get_frame_plan"), QJsonObject{});
    }
}

void PlanPerfHub::readOffThread(const QByteArray &bytes, Reply kind) {
    // A full GPU window, or a plan, is hundreds of kilobytes: parsed on the
    // pool, so the panel being worked in does not wait for it.
    QPointer<PlanPerfHub> self{this};
    const std::uint64_t epoch = epoch_;
    QThreadPool::globalInstance()->start([self, bytes, kind, epoch] {
        const auto json = parsed(bytes);
        if (!json) return;
        std::variant<std::monostate, PlanPerfStatusReading, PlanPerfPlanReading,
                     PlanPerfTimingReading>
            reading;
        if (kind == Reply::Status) {
            if (auto read = PlanPerfModel::statusReading(*json)) reading = std::move(*read);
        } else if (kind == Reply::Plan) {
            if (auto read = PlanPerfModel::planReading(*json)) reading = std::move(*read);
        } else if (auto read = PlanPerfModel::timingReading(*json)) {
            reading = std::move(*read);
        }
        if (std::holds_alternative<std::monostate>(reading)) return;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, reading = std::move(reading), epoch] {
                if (self.isNull() || self->epoch_ != epoch) return;
                std::visit(
                    [&](const auto &value) {
                        if constexpr (!std::is_same_v<std::decay_t<decltype(value)>,
                                                      std::monostate>)
                            self->model_.read(value);
                    },
                    reading);
                self->announce();
            },
            Qt::QueuedConnection);
    });
}

QString PlanPerfHub::summary() const { return QString::fromStdString(model_.summaryLine()); }

QString PlanPerfHub::delta() const { return QString::fromStdString(model_.deltaLine()); }

void PlanPerfHub::announce() {
    if (announced_ == model_.revision()) return;
    announced_ = model_.revision();
    if (hooks_.changed) hooks_.changed();
}

} // namespace PelicanStudio
