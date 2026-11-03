#pragma once

#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QObject>

#include "studiocommands.hpp"

#include <functional>
#include <utility>

namespace PelicanStudio {

// Report one completion: Enter commits, Escape cancels, and moving focus to
// another widget commits. A popup or inactive window only borrows focus, so
// Alt-Tab and the embedded viewport's native focus handoff keep the edit open.
// Callers must defer destroying the editor until its current event returns.
class InlineEditEventFilter final : public QObject {
  public:
    using Done = std::function<void(bool commit)>;

    explicit InlineEditEventFilter(Done done) : done_{std::move(done)} {}

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::KeyPress) {
            const auto *key_event = static_cast<QKeyEvent *>(event);
            if (studioKeyMatches("inline.cancel", *key_event)) {
                finish(false);
                return true;
            }
            if (studioKeyMatches("inline.commit", *key_event)) {
                finish(true);
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            const auto reason = static_cast<QFocusEvent *>(event)->reason();
            if (reason != Qt::ActiveWindowFocusReason &&
                reason != Qt::PopupFocusReason)
                finish(true);
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    void finish(bool commit) {
        if (!done_) return;
        auto done = std::exchange(done_, {});
        done(commit);
    }

    Done done_;
};

} // namespace PelicanStudio
