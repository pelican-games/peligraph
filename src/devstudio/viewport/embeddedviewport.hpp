#pragma once

#include "engineprocess.hpp"
#include "nativewindowhost.hpp"

#include <QElapsedTimer>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

namespace PelicanStudio {

class EmbeddedViewport final : public QWidget {
    Q_OBJECT

  public:
    explicit EmbeddedViewport(QWidget *parent = nullptr);
    ~EmbeddedViewport() override;

  private:
    EngineProcess process_;
    QWidget *native_host_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *restart_button_ = nullptr;
    QPushButton *stop_button_ = nullptr;
    QTimer *window_discovery_timer_ = nullptr;
    QTimer *diagnostics_timer_ = nullptr;
    QTimer *pointer_focus_timer_ = nullptr;
    QElapsedTimer window_discovery_elapsed_;
    NativeWindowHandle child_window_ = 0;
    bool pointer_button_was_down_ = false;
    QSize last_requested_extent_;
    QString recent_output_;

    EngineProcessLaunch launchCommand() const;
    void startEngine();
    void stopEngine();
    void discoverEngineWindow();
    void resizeEmbeddedWindow();
    void focusEmbeddedWindow();
    void pollPointerFocus();
    void updateDiagnostics();
    void finishProcessShutdown(qint64 expected_process_id);
};

} // namespace PelicanStudio
