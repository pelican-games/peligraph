#pragma once

#include "planperfmodel.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>

class QJsonObject;
class QTimer;

namespace PelicanStudio {

class EmbeddedViewport;

// UX-6b. The one reader of "how fast is the frame, and what did the last edit
// do to it" for the Frame Plan and the Graph tab. The line's time and the
// bands' read get_gpu_timing (each node's average, small) about once a second
// while either panel is on screen; the generations an edit is compared by
// read get_status (every sample of the window, large) only while an edit
// waits for its after. It reads no plan of its own while a panel's plan read
// will do: every get_frame_plan and get_gpu_timing any panel sends is read
// here as well (the window's RPC observer names them). Only when an edit's
// after has a generation no panel read the plan of does it ask for the plan
// once. Every reply is parsed on the thread pool.
class PlanPerfHub final : public QObject {
  public:
    struct Hooks {
        // A panel that shows the lines is on screen.
        std::function<bool()> watched;
        // The lines or the nodes' times moved.
        std::function<void()> changed;
    };

    PlanPerfHub(EmbeddedViewport &viewport, Hooks hooks, QObject *parent = nullptr);

    // The window's RPC observer: before every request, and once it is sent.
    void requestGoing(const QString &method, const QJsonObject &params);
    void requestSent(qint64 id, const QString &method, const QJsonObject &params);

    [[nodiscard]] const PlanPerfModel &model() const noexcept { return model_; }
    [[nodiscard]] QString summary() const;
    [[nodiscard]] QString delta() const;
    // One tick of the clock (what the timer does), named for a test.
    void poll();

  private:
    enum class Reply : std::uint8_t { Status, Plan, Timing };
    void announce();
    // Parses a reply on the thread pool and reads it here.
    void readOffThread(const QByteArray &bytes, Reply kind);

    EmbeddedViewport &viewport_;
    Hooks hooks_;
    PlanPerfModel model_;
    QTimer *timer_ = nullptr;
    qint64 status_request_ = 0;
    qint64 plan_request_ = 0;
    qint64 timing_request_ = 0;
    // The whole-plan and GPU timing reads in flight, whoever sent them.
    QSet<qint64> plan_reads_;
    QSet<qint64> timing_reads_;
    std::optional<std::uint64_t> plan_asked_for_;
    std::uint64_t announced_ = ~std::uint64_t{0};
    // Moves when the engine goes, so a reading parsed for it is dropped.
    std::uint64_t epoch_ = 0;
    // When get_status and get_gpu_timing were last read (by anyone), and an
    // edit last sent.
    QElapsedTimer last_status_;
    QElapsedTimer last_timing_;
    QElapsedTimer last_edit_;
    // When a panel last sent an edit, its ticket, or a read that follows a
    // commit.
    QElapsedTimer last_activity_;
};

} // namespace PelicanStudio
