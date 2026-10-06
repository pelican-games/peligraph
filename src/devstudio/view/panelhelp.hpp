#pragma once

// N-7 rule 1: the sentence that explains a mechanism does not live on the
// panel. It lives on the control it explains (a tooltip) or behind one `?`
// in the panel's corner, which opens the manual section that owns the
// subject. This header is the `?` and the two text styles every panel shares,
// so a panel does not invent its own face, size or colour.

#include "studiotheme.hpp"

#include <QAction>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QUrl>
#include <QWidget>

namespace PelicanStudio {

// The manual section each panel's `?` opens, as `<chapter>#<heading slug>`.
// One place, so a heading that moves is one edit rather than six.
inline constexpr const char *ManualPassEditorAnchor =
    "15_studio.md#159-pass-editor";
inline constexpr const char *ManualRenderFeaturesAnchor =
    "13_editor.md#レンダー設定を編集する";
inline constexpr const char *ManualInspectorAnchor =
    "15_studio.md#studioでcomponentを追加する";
inline constexpr const char *ManualFramePlanAnchor =
    "15_studio.md#158-frame-plan";
inline constexpr const char *ManualGraphCanvasAnchor =
    "15_studio.md#studio-のノードキャンバスgraph-タブ";

// Where a chapter of the shipped manual is, found by walking up from the
// executable. A Studio run out of a build tree, out of `dist_debug`, or out
// of the source tree all reach the same file; a Studio with no manual next
// to it answers with an empty string rather than a URL that opens nothing.
[[nodiscard]] inline QString manualChapterPath(const QString &chapter) {
    QDir directory{QCoreApplication::applicationDirPath()};
    for (int step = 0; step < 8; ++step) {
        const QString candidate =
            directory.filePath(QStringLiteral("docs/manual/") + chapter);
        if (QFileInfo::exists(candidate)) return candidate;
        if (!directory.cdUp()) break;
    }
    return {};
}

inline bool openManualSection(const QString &anchor) {
    const QString path = manualChapterPath(anchor.section(QLatin1Char('#'), 0, 0));
    if (path.isEmpty()) return false;
    QUrl url = QUrl::fromLocalFile(path);
    url.setFragment(anchor.section(QLatin1Char('#'), 1));
    return QDesktopServices::openUrl(url);
}

// The panel's `?`. It carries the anchor as a property so a test can see
// which section a panel claims without opening anything, and its tooltip is
// where the panel's own explanation went.
[[nodiscard]] inline QPushButton *makePanelHelpButton(
    QWidget *parent, const char *object_name, const char *anchor,
    const QString &explanation) {
    auto *button = new QPushButton(QStringLiteral("?"), parent);
    button->setObjectName(QString::fromLatin1(object_name));
    button->setProperty("pelicanManualAnchor", QString::fromUtf8(anchor));
    button->setFlat(true);
    button->setFixedWidth(22);
    button->setToolTip(explanation);
    const QString section = QString::fromUtf8(anchor);
    QObject::connect(button, &QPushButton::clicked, button,
                     [section] { openManualSection(section); });
    return button;
}

// UX-1, decision 7: the whole of what a notice row says, as it is to be
// pasted -- the full message its tooltip carries (an engine refusal's code
// and message) rather than the line cut to fit, and the line followed by
// its tooltip when the tooltip is a detail the line does not contain.
[[nodiscard]] inline QString panelNoticeMessage(const QLabel &label) {
    const QString tip = label.toolTip();
    const QString line = label.text();
    if (tip.isEmpty()) return line;
    if (line.isEmpty()) return tip;
    // The line is the tooltip's start, cut short: the tooltip is the whole.
    QString head = line.left(24);
    if (head.endsWith(QChar{0x2026})) head.chop(1);
    if (tip.contains(head.trimmed())) return tip;
    return line + QLatin1Char('\n') + tip;
}

// A one-line state row: the theme's small size and its dim colour, never a
// paragraph. `setPanelNotice` is the whole of rule 2 -- an empty state hides
// the row instead of leaving a blank line behind.
//
// UX-1, decision 7: and a row whose words can be taken away -- selected
// with the mouse, or, from its right-click menu, Copy message, which puts
// the whole message on the clipboard (panelNoticeMessage) however much of
// it the row shows. Every panel's notice is styled here, so every one of
// them can be copied.
inline void applyPanelNoticeStyle(QLabel *label) {
    if (label == nullptr) return;
    label->setFont(studioSmallFont());
    label->setWordWrap(false);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    const QString name = QStringLiteral("pelican.panelNotice.copyMessage");
    if (label->findChild<QAction *>(name, Qt::FindDirectChildrenOnly) != nullptr) return;
    auto *copy = new QAction(QObject::tr("Copy message"), label);
    copy->setObjectName(name);
    label->addAction(copy);
    label->setContextMenuPolicy(Qt::ActionsContextMenu);
    QObject::connect(copy, &QAction::triggered, label, [label] {
        if (QClipboard *clipboard = QGuiApplication::clipboard())
            clipboard->setText(panelNoticeMessage(*label));
    });
}

// Rule 2 again, for the messages a panel does not write itself: the first
// sentence is the line, and the rest is what the tooltip and the Output dock
// are for. A message that is already one short sentence comes back unchanged,
// which is what the scenarios that read a notice depend on.
[[nodiscard]] inline QString oneLine(const QString &message, int limit = 90) {
    QString line = message.trimmed();
    const qsizetype newline = line.indexOf(QLatin1Char('\n'));
    if (newline >= 0) line = line.left(newline).trimmed();
    const qsizetype stop = line.indexOf(QStringLiteral(". "));
    if (stop >= 0) line = line.left(stop + 1);
    if (line.size() > limit)
        line = line.left(limit - 1).trimmed() + QStringLiteral("…");
    return line;
}

inline void setPanelNotice(QLabel *label, const QString &line,
                           const QString &detail = {}) {
    if (label == nullptr) return;
    label->setToolTip(detail);
    label->setText(line);
    label->setVisible(!line.isEmpty());
}

} // namespace PelicanStudio
