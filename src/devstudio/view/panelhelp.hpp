#pragma once

// N-7 rule 1: the sentence that explains a mechanism does not live on the
// panel. It lives on the control it explains (a tooltip) or behind one `?`
// in the panel's corner, which opens the manual section that owns the
// subject. This header is the `?` and the two text styles every panel shares,
// so a panel does not invent its own face, size or colour.

#include "studiotheme.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QUrl>
#include <QWidget>

namespace PelicanStudio {

// The manual section each panel's `?` opens. One place, so a heading that
// moves is one edit rather than six.
inline constexpr const char *ManualPassEditorAnchor =
    "レンダー設定を編集する";
inline constexpr const char *ManualRenderFeaturesAnchor =
    "レンダー設定を編集する";
inline constexpr const char *ManualInspectorAnchor =
    "studioでcomponentを追加する";
inline constexpr const char *ManualFramePlanAnchor =
    "レンダー設定を編集する";
inline constexpr const char *ManualGraphCanvasAnchor =
    "studio-のノードキャンバスgraph-タブ";

// Where the shipped manual is, found by walking up from the executable. A
// Studio run out of a build tree, out of `dist_debug`, or out of the source
// tree all reach the same file; a Studio that has no manual next to it
// answers with an empty string rather than a URL that opens nothing.
[[nodiscard]] inline QString manualEditorChapterPath() {
    QDir directory{QCoreApplication::applicationDirPath()};
    for (int step = 0; step < 8; ++step) {
        const QString candidate =
            directory.filePath(QStringLiteral("docs/manual/13_editor.md"));
        if (QFileInfo::exists(candidate)) return candidate;
        if (!directory.cdUp()) break;
    }
    return {};
}

inline bool openManualSection(const QString &anchor) {
    const QString path = manualEditorChapterPath();
    if (path.isEmpty()) return false;
    QUrl url = QUrl::fromLocalFile(path);
    if (!anchor.isEmpty()) url.setFragment(anchor);
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

// A one-line state row: the theme's small size and its dim colour, never a
// paragraph. `setPanelNotice` is the whole of rule 2 -- an empty state hides
// the row instead of leaving a blank line behind.
inline void applyPanelNoticeStyle(QLabel *label) {
    if (label == nullptr) return;
    label->setFont(studioSmallFont());
    label->setWordWrap(false);
    label->setTextFormat(Qt::PlainText);
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
