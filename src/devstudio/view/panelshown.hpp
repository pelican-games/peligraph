#pragma once

#include <QDockWidget>
#include <QEvent>
#include <QObject>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <utility>

namespace PelicanStudio {

// RP-1a1 follow-up 1: whether a panel is actually shown.
//
// Qt keeps every page of a tabified dock visible: a non-selected tab's content
// still answers QWidget::isVisible() == true and receives no showEvent when
// its tab is selected. QDockWidget::visibilityChanged(bool) does follow the
// dock's shown / hidden state, including tab selection, so a panel under a
// dock uses its last value. A panel that is not in a dock keeps the old rule:
// QWidget::isVisible().
//
// The enclosing dock is found lazily: a panel is constructed before it is put
// into its dock. The connection's context is this helper, owned by the panel's
// Impl, so it dies with the panel.
class PanelShown final : public QObject {
  public:
    using BecameShown = std::function<void()>;
    using BecameHidden = std::function<void()>;

    PanelShown(QWidget &owner, BecameShown became_shown,
               BecameHidden became_hidden = {})
        : owner_{owner},
          became_shown_{std::move(became_shown)},
          became_hidden_{std::move(became_hidden)} {
        // A panel hidden directly (no dock) does not get a visibilityChanged
        // false; its own Hide event is the transition's end.
        owner_.installEventFilter(this);
    }

    // Rule 1. Finds the enclosing dock on the first call. A dock assigned
    // after the panel was shown makes the remembered state catch up at once.
    [[nodiscard]] bool isShown() const {
        const_cast<PanelShown *>(this)->findDock();
        return owner_.isVisible() && (!dock_ || dock_visible_);
    }

    // Finds a dock that may have been assigned since the last call, without
    // changing the remembered visibility.
    void locateDock() { findDock(); }

    // The "became shown" path. showEvent and visibilityChanged(true) both
    // arrive for one show; this calls `became_shown_` once per transition.
    void becameShown() {
        findDock();
        const bool shown = isShown();
        if (shown && !was_shown_) {
            was_shown_ = true;
            if (became_shown_) became_shown_();
        } else if (!shown) {
            transitionToHidden();
        }
    }

    // The "became hidden" path. hideEvent, the owner's Hide event and
    // visibilityChanged(false) may all arrive for one hide; this calls
    // `became_hidden_` once per shown -> hidden transition.
    void becameHidden() {
        findDock();
        transitionToHidden();
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == &owner_ && event->type() == QEvent::Hide)
            transitionToHidden();
        return QObject::eventFilter(watched, event);
    }

  private:
    void transitionToHidden() {
        if (!was_shown_) return;
        was_shown_ = false;
        if (became_hidden_) became_hidden_();
    }

    void findDock() {
        if (dock_) return;
        for (QWidget *widget = owner_.parentWidget(); widget != nullptr;
             widget = widget->parentWidget()) {
            auto *dock = qobject_cast<QDockWidget *>(widget);
            if (dock == nullptr) continue;
            dock_ = dock;
            // Selected tab: visible with a non-empty visible region. A
            // non-selected tab: visible, but its region is empty.
            dock_visible_ = dock->isVisible() && !dock->visibleRegion().isEmpty();
            QObject::connect(dock, &QDockWidget::visibilityChanged, this,
                             [this](bool visible) {
                                 dock_visible_ = visible;
                                 if (visible) {
                                     becameShown();
                                 } else {
                                     transitionToHidden();
                                 }
                             });
            // The dock may have been assigned after the panel was shown (or
            // hidden): the remembered state must not stay out of step.
            const bool shown = owner_.isVisible() && dock_visible_;
            if (shown && !was_shown_) {
                was_shown_ = true;
                if (became_shown_) became_shown_();
            } else if (!shown) {
                transitionToHidden();
            }
            return;
        }
    }

    QWidget &owner_;
    BecameShown became_shown_;
    BecameHidden became_hidden_;
    QPointer<QDockWidget> dock_;
    bool dock_visible_ = false;
    bool was_shown_ = false;
};

} // namespace PelicanStudio
