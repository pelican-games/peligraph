#include "docktitlebar.hpp"

#include <QDockWidget>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QStyle>
#include <QToolButton>

namespace PelicanStudio {
namespace {

QToolButton *makeTitleButton(
    QWidget *parent, const QString &object_name, const QString &tooltip,
    QStyle::StandardPixmap icon) {
    auto *button = new QToolButton(parent);
    button->setObjectName(object_name);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    const int icon_size = button->style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, button);
    const int margin =
        button->style()->pixelMetric(QStyle::PM_DockWidgetTitleBarButtonMargin, nullptr, button);
    button->setIconSize(QSize(icon_size, icon_size));
    button->setFixedSize(icon_size + 2 * margin, icon_size + 2 * margin);
    button->setIcon(button->style()->standardIcon(icon, nullptr, button));
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    return button;
}

} // namespace

DockTitleBar::DockTitleBar(QDockWidget *dock) : QWidget(dock), dock_(dock) {
    setObjectName(dock_->objectName() + QStringLiteral(".titleBar"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(style()->pixelMetric(QStyle::PM_TitleBarHeight, nullptr, this));
    dock_->installEventFilter(this);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 2, 0);
    layout->setSpacing(1);

    title_ = new QLabel(this);
    title_->setObjectName(dock_->objectName() + QStringLiteral(".title"));
    title_->setTextFormat(Qt::PlainText);
    title_->setAttribute(Qt::WA_TransparentForMouseEvents);
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    title_->setMinimumWidth(0);
    layout->addWidget(title_, 1);

    const QString prefix = dock_->objectName() + QLatin1Char('.');

    maximize_ = makeTitleButton(
        this, prefix + QStringLiteral("maximize"), tr("Maximize"),
        QStyle::SP_TitleBarMaxButton);
    layout->addWidget(maximize_);

    float_ = makeTitleButton(
        this, prefix + QStringLiteral("float"), tr("Float"),
        QStyle::SP_TitleBarNormalButton);
    layout->addWidget(float_);

    close_ = makeTitleButton(
        this, prefix + QStringLiteral("close"), tr("Close"),
        QStyle::SP_TitleBarCloseButton);
    layout->addWidget(close_);

    connect(maximize_, &QToolButton::clicked, this, [this] { toggleMaximized(); });
    connect(float_, &QToolButton::clicked, this, [this] { toggleFloating(); });
    connect(close_, &QToolButton::clicked, dock_, &QDockWidget::close);
    connect(dock_, &QDockWidget::windowTitleChanged, this,
            [this](const QString &) { syncTitle(); });
    connect(dock_, &QDockWidget::topLevelChanged, this,
            [this](bool) { syncButtons(); });

    syncTitle();
    syncButtons();
}

bool DockTitleBar::isDocked() const {
    auto *main_window = qobject_cast<QMainWindow *>(dock_->window());
    return main_window != nullptr && dock_->window() != dock_;
}

bool DockTitleBar::isFloatingGroup() const {
    QWidget *window = dock_->window();
    return window != nullptr && window != dock_ &&
           qobject_cast<QMainWindow *>(window) == nullptr;
}

void DockTitleBar::syncTitle() {
    if (title_ == nullptr) return;
    title_->setText(dock_->windowTitle());
    title_->setToolTip(dock_->windowTitle());
}

void DockTitleBar::syncButtons() {
    if (maximize_ == nullptr) return;

    const bool docked = isDocked();
    const bool group = isFloatingGroup();
    const bool single_floating = !docked && !group && dock_->isWindow();

    // Docked: today's Float and Close only. A floating tab group is a window
    // this bar cannot maximize without maximizing its siblings, so the button
    // stays visible but disabled and says why.
    maximize_->setVisible(!docked);
    maximize_->setEnabled(single_floating);

    if (single_floating && dock_->isMaximized()) {
        maximize_->setToolTip(tr("Restore"));
        maximize_->setAccessibleName(tr("Restore"));
        maximize_->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton, nullptr, this));
    } else {
        maximize_->setToolTip(
            group ? tr("Only a single floating panel can be maximized") : tr("Maximize"));
        maximize_->setAccessibleName(maximize_->toolTip());
        maximize_->setIcon(style()->standardIcon(QStyle::SP_TitleBarMaxButton, nullptr, this));
    }

    const bool floating = dock_->isFloating() || group;
    float_->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton, nullptr, this));
    float_->setToolTip(floating ? tr("Dock") : tr("Float"));
    float_->setAccessibleName(float_->toolTip());
    close_->setToolTip(tr("Close"));
    close_->setAccessibleName(close_->toolTip());
}

void DockTitleBar::toggleMaximized() {
    if (!dock_->isWindow() || isFloatingGroup()) return;
    if (dock_->isMaximized()) {
        dock_->showNormal();
    } else {
        normal_geometry_ = dock_->geometry();
        dock_->showMaximized();
    }
    syncButtons();
}

void DockTitleBar::restoreNormalGeometry() {
    // Qt applies showNormal's geometry on a later event-loop turn, but both
    // setFloating(false) and a drag read the current geometry immediately.
    // Put the saved normal geometry back in this stack.
    const QRect normal =
        normal_geometry_.isValid() ? normal_geometry_ : dock_->normalGeometry();
    dock_->showNormal();
    if (normal.isValid()) dock_->setGeometry(normal);
}

void DockTitleBar::toggleFloating() {
    if (isFloatingGroup() || dock_->isFloating()) {
        if (dock_->isMaximized()) {
            // A maximized floating dock would otherwise store its maximized
            // size as the size to come back at.
            restoreNormalGeometry();
        }
        dock_->setFloating(false);
    } else {
        dock_->setFloating(true);
    }
    syncButtons();
}

void DockTitleBar::mousePressEvent(QMouseEvent *event) {
    // QDockWidget's event filter has already seen this press and will start
    // its drag, dock or double-click. Let the press reach the dock as well.
    event->ignore();
}

void DockTitleBar::mouseMoveEvent(QMouseEvent *event) {
    // QDockWidget starts its drag from the press and usually owns the mouse
    // from then on. If this bar still sees the first move, restore before the
    // drag uses the maximized size.
    if ((event->buttons() & Qt::LeftButton) && dock_->isWindow() && dock_->isMaximized()) {
        restoreNormalGeometry();
    }
    event->ignore();
}

void DockTitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
    event->ignore();
}

bool DockTitleBar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == dock_) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
            if (dock_->isWindow() && !dock_->isMaximized()) {
                normal_geometry_ = dock_->geometry();
            }
            break;
        case QEvent::MouseMove: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if ((mouse->buttons() & Qt::LeftButton) && dock_->isWindow() &&
                dock_->isMaximized()) {
                // The drag starts with QDockWidget's own handling of this
                // move; restore first so it moves a normal window.
                restoreNormalGeometry();
            }
            break;
        }
        case QEvent::ParentChange:
        case QEvent::WindowStateChange:
            syncButtons();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace PelicanStudio
