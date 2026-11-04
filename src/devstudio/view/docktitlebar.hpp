#pragma once

// DK-1: a floating dock keeps the main window's shortcuts, so it stays a
// Qt::Tool window rather than becoming a Qt::Window. A tool window has no
// platform maximize button, so this title bar supplies one for every dock.
// The rest of the bar -- drag, dock, double-click -- is still QDockWidget's:
// the bar ignores mouse presses and lets them through to the dock.

#include <QRect>
#include <QWidget>

class QDockWidget;
class QLabel;
class QToolButton;

namespace PelicanStudio {

class DockTitleBar final : public QWidget {
    Q_OBJECT

public:
    explicit DockTitleBar(QDockWidget *dock);

    [[nodiscard]] QLabel *titleLabel() const { return title_; }
    [[nodiscard]] QToolButton *maximizeButton() const { return maximize_; }
    [[nodiscard]] QToolButton *floatButton() const { return float_; }
    [[nodiscard]] QToolButton *closeButton() const { return close_; }

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    [[nodiscard]] bool isDocked() const;
    [[nodiscard]] bool isFloatingGroup() const;
    void syncTitle();
    void syncButtons();
    void toggleMaximized();
    void toggleFloating();
    void restoreNormalGeometry();

    QDockWidget *dock_ = nullptr;
    QRect normal_geometry_;
    QLabel *title_ = nullptr;
    QToolButton *maximize_ = nullptr;
    QToolButton *float_ = nullptr;
    QToolButton *close_ = nullptr;
};

} // namespace PelicanStudio
