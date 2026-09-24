#include "shadercompilelogpanel.hpp"

#include "shaderdiagnostics.hpp"

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

#include <string>

namespace PelicanStudio {
namespace {

QString text(std::string_view value) {
    return QString::fromUtf8(value.data(),
                             static_cast<qsizetype>(value.size()));
}

} // namespace

ShaderCompileLogPanel::ShaderCompileLogPanel(QWidget *parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("pelican.shaderCompileLog"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto *heading = new QHBoxLayout;
    toggle_ = new QToolButton(this);
    toggle_->setObjectName(QStringLiteral("pelican.shaderCompileLog.toggle"));
    toggle_->setCheckable(true);
    toggle_->setArrowType(Qt::RightArrow);
    toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle_->setAutoRaise(true);
    toggle_->setText(tr("Shader compile log"));
    toggle_->setStyleSheet(QStringLiteral("color: #d94c3d;"));
    summary_ = new QLabel(this);
    summary_->setObjectName(
        QStringLiteral("pelican.shaderCompileLog.summary"));
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    heading->addWidget(toggle_);
    heading->addWidget(summary_, 1);
    layout->addLayout(heading);

    log_ = new QPlainTextEdit(this);
    log_->setObjectName(QStringLiteral("pelican.shaderCompileLog.text"));
    log_->setReadOnly(true);
    log_->setLineWrapMode(QPlainTextEdit::NoWrap);
    log_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log_->setMinimumHeight(120);
    log_->hide();
    layout->addWidget(log_);

    QObject::connect(toggle_, &QToolButton::toggled, this,
                     [this](bool open) { setExpanded(open); });
    hide();
}

bool ShaderCompileLogPanel::showMessage(const QString &message) {
    const std::string bytes = message.toStdString();
    const auto failures = parseShaderCompileFailures(bytes);
    if (failures.empty()) return false;

    QString body;
    QStringList locations;
    struct Emphasis {
        int start = 0;
        int length = 0;
    };
    std::vector<Emphasis> emphasis;
    for (const auto &failure : failures) {
        if (!body.isEmpty()) body += QLatin1Char('\n');
        body += tr("Shader compile failed: %1").arg(text(failure.path)) +
                QLatin1Char('\n');
        std::size_t start = 0;
        int number = 1;
        const std::string_view log{failure.log};
        while (start <= log.size() && !log.empty()) {
            const auto end = log.find('\n', start);
            const auto line = log.substr(
                start, end == std::string_view::npos ? std::string_view::npos
                                                     : end - start);
            const QString prefix =
                QStringLiteral("%1 | ").arg(number, 4);
            const QString rendered = text(line);
            const int line_start =
                static_cast<int>(body.size()) + static_cast<int>(prefix.size());
            for (const auto &location : shaderLogLocations(line)) {
                const QString before = text(line.substr(0, location.offset));
                const QString span = text(
                    line.substr(location.offset, location.length));
                emphasis.push_back(Emphasis{
                    .start = line_start + static_cast<int>(before.size()),
                    .length = static_cast<int>(span.size()),
                });
                locations.push_back(
                    QStringLiteral("%1:%2")
                        .arg(text(location.path))
                        .arg(location.line));
            }
            body += prefix + rendered + QLatin1Char('\n');
            ++number;
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
    }
    log_->setPlainText(body);

    QTextCharFormat marked;
    marked.setForeground(QColor{QStringLiteral("#d94c3d")});
    marked.setFontWeight(QFont::Bold);
    QList<QTextEdit::ExtraSelection> selections;
    for (const auto &span : emphasis) {
        QTextEdit::ExtraSelection selection;
        selection.cursor = QTextCursor{log_->document()};
        selection.cursor.setPosition(span.start);
        selection.cursor.setPosition(span.start + span.length,
                                     QTextCursor::KeepAnchor);
        selection.format = marked;
        selections.push_back(selection);
    }
    log_->setExtraSelections(selections);

    const QString first = text(failures.front().first_line);
    summary_->setText(first);
    summary_->setToolTip(first);
    setProperty("pelicanCompileFailureCount",
                static_cast<qulonglong>(failures.size()));
    setProperty("pelicanCompileLocations", locations);
    show();
    // A new log arrives open: it is what the author has to read next. The
    // fold is theirs to close.
    setExpanded(true);
    return true;
}

void ShaderCompileLogPanel::clear() {
    log_->clear();
    log_->setExtraSelections({});
    summary_->clear();
    setProperty("pelicanCompileFailureCount", qulonglong{0});
    setProperty("pelicanCompileLocations", QStringList{});
    setExpanded(false);
    hide();
}

bool ShaderCompileLogPanel::expanded() const { return !log_->isHidden(); }

void ShaderCompileLogPanel::setExpanded(bool expanded) {
    if (toggle_->isChecked() != expanded) {
        toggle_->setChecked(expanded);
        return; // toggled() comes back here with the same answer
    }
    toggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    log_->setVisible(expanded);
}

QString ShaderCompileLogPanel::logText() const { return log_->toPlainText(); }

} // namespace PelicanStudio
