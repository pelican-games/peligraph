#pragma once

#include <QObject>
#include <QString>

#include <functional>

class QLocalServer;
class QLocalSocket;
class QWidget;

namespace PelicanStudio {

class EmbeddedViewport;

// Everything the automation surface is allowed to reach in the running Studio.
// It is handed in rather than looked up, so this library never links the view
// library that builds the window -- the window builds the host instead, and the
// two cannot form a link cycle.
struct AutomationHost {
    QWidget *window = nullptr;
    EmbeddedViewport *viewport = nullptr;
    std::function<void(const QString &directory)> open_project;
    // UX-7, decision 9: run before `quit` closes the window, with the quit
    // to call once it is done (a run in Play is stopped first). Absent: the
    // quit goes at once.
    std::function<void(std::function<void()> quit)> before_quit;
};

// One newline-delimited JSON channel into a running Studio, opened only by
// `--automation <name>`. Nothing here exists in a shipping launch: the server
// object is never constructed without the flag, so the socket does not exist
// and no command path can be reached.
//
// Wire format, one JSON object per line in each direction:
//   -> {"id": 7, "cmd": "click", "name": "pelican.graphCanvasRefresh"}
//   <- {"id": 7, "ok": true, "result": {...}}
//   <- {"id": 7, "ok": false, "error": {"code": "automation.widget_missing@1",
//                                       "message": "..."}}
class AutomationServer final : public QObject {
    Q_OBJECT

  public:
    explicit AutomationServer(AutomationHost host, QObject *parent = nullptr);
    ~AutomationServer() override;

    // Start listening on the local socket `name` (a named pipe on Windows).
    bool listen(const QString &name, QString *error = nullptr);
    [[nodiscard]] const QString &endpoint() const noexcept { return endpoint_; }

  private:
    AutomationHost host_;
    QLocalServer *server_ = nullptr;
    QString endpoint_;
    // A command that spins a nested event loop (rpc, wait) can be re-entered by
    // a second line arriving meanwhile. The driver is strictly sequential, so
    // the honest answer to that is an error rather than interleaved state.
    bool running_command_ = false;

    void acceptConnection();
    void readFrom(QLocalSocket *socket);
    void handleLine(QLocalSocket *socket, const QByteArray &line);
};

} // namespace PelicanStudio
