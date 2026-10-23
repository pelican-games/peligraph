#include "renderfallbacknotice.hpp"

#include "panelhelp.hpp"

#include <QDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace PelicanStudio {
namespace {

QString qtext(const std::string &value) { return QString::fromStdString(value); }

} // namespace

RenderFallbackNotice::RenderFallbackNotice(Hooks hooks, QWidget *parent)
    : QFrame(parent), hooks_{std::move(hooks)},
      repair_{RenderDocumentRepair::Transport{.request = hooks_.request, .schedule = hooks_.schedule}} {
    setObjectName(QStringLiteral("pelican.renderFallbackNotice"));
    setFrameShape(QFrame::StyledPanel);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 3, 6, 3);
    layout->setSpacing(6);
    text_ = new QLabel(this);
    text_->setObjectName(QStringLiteral("pelican.renderFallbackNotice.text"));
    text_->setWordWrap(true);
    text_->setTextFormat(Qt::PlainText);
    text_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text_->setStyleSheet(QStringLiteral("color: #d94c3d;"));
    layout->addWidget(text_, 1);

    reload_ = new QPushButton(tr("Reload from disk"), this);
    reload_->setObjectName(QStringLiteral("pelican.renderFallbackNotice.reload"));
    reload_->setToolTip(tr("Read the rendering documents again from their files "
                           "(reload_render_documents)"));
    open_graph_ = new QPushButton(tr("Open in Graph"), this);
    open_graph_->setObjectName(QStringLiteral("pelican.renderFallbackNotice.openGraph"));
    open_file_ = new QPushButton(tr("Open file"), this);
    open_file_->setObjectName(QStringLiteral("pelican.renderFallbackNotice.openFile"));
    show_document_ = new QPushButton(tr("Show the document"), this);
    show_document_->setObjectName(QStringLiteral("pelican.renderFallbackNotice.showDocument"));
    show_document_->setToolTip(tr("The document as it is on disk, to fix by hand and save whole "
                                  "(restore_render_documents)"));
    restart_ = new QPushButton(tr("Restart engine"), this);
    restart_->setObjectName(QStringLiteral("pelican.renderFallbackNotice.restart"));
    for (QPushButton *button : {reload_, open_graph_, open_file_, show_document_, restart_})
        layout->addWidget(button);

    connect(reload_, &QPushButton::clicked, this, [this] {
        if (hooks_.reload) hooks_.reload();
    });
    connect(open_graph_, &QPushButton::clicked, this, [this] {
        if (hooks_.open_graph && !session_.notice().node.empty())
            hooks_.open_graph(qtext(session_.notice().node));
    });
    connect(open_file_, &QPushButton::clicked, this, [this] {
        if (hooks_.open_file && !session_.notice().document.empty())
            hooks_.open_file(qtext(session_.notice().document));
    });
    connect(show_document_, &QPushButton::clicked, this, [this] { showDocument(); });
    connect(restart_, &QPushButton::clicked, this, [this] {
        if (hooks_.restart) hooks_.restart(qtext(session_.restartReason()));
    });
    repair_.changed = [this] { presentRepair(); };
    repair_.restored = [this](const nlohmann::json &commit) {
        if (hooks_.restored) hooks_.restored(commit);
    };
    present(true);
}

RenderFallbackNotice::~RenderFallbackNotice() {
    repair_.changed = nullptr;
    repair_.restored = nullptr;
    if (dialog_) delete dialog_.data();
}

void RenderFallbackNotice::observeStatus(const nlohmann::json &reply) {
    const bool was_fallback = session_.fallback();
    present(session_.observeStatus(reply));
    // RR-1s: the placeholder gave way; whether the reads are live is asked
    // again.
    if (was_fallback && !session_.fallback()) readAuthoring();
}

void RenderFallbackNotice::observeReloadRefusal(const nlohmann::json &error) {
    present(session_.observeReloadRefusal(error));
    // The engine may say more about why it stays on the placeholder.
    if (session_.fallback() && hooks_.read_status) hooks_.read_status();
    // RR-1s: a refusal of the documents makes the version on disk the
    // editor's base (engine RR-1).
    readAuthoring();
}

void RenderFallbackNotice::observeReply(const nlohmann::json &reply) {
    if (session_.wantsStatusAfter(reply) && hooks_.read_status) hooks_.read_status();
    // RR-1s: an edit that committed on the held version publishes it.
    const auto &reading = session_.authoring();
    if (reading.known && (!reading.live || reading.held_unparsed) && reply.is_object() &&
        (reply.value("status", std::string{}) == "committed" ||
         (reply.contains("committed") && reply.at("committed").is_boolean() && reply.at("committed").get<bool>())))
        readAuthoring();
}

void RenderFallbackNotice::reloadCommitted() {
    if (session_.fallback() && hooks_.read_status) hooks_.read_status();
    readAuthoring();
}

void RenderFallbackNotice::engineGone() {
    authoring_request_ = 0;
    authoring_again_ = false;
    problem_ids_.clear();
    session_.engineGone();
    repair_.engineGone();
    present(true);
}

void RenderFallbackNotice::readAuthoring() {
    if (!hooks_.request) return;
    if (authoring_request_ != 0) {
        authoring_again_ = true;
        return;
    }
    std::string error;
    authoring_request_ = hooks_.request("get_graph_instances", nlohmann::json::object(), &error);
}

void RenderFallbackNotice::observeProblems(const std::vector<ProblemRow> &rows) {
    present(session_.observeProblems(rows));
    std::vector<std::string> ids;
    ids.reserve(rows.size());
    for (const auto &row : rows)
        if (row.origin == ProblemRow::Origin::Engine) ids.push_back(row.id);
    // A reload refused or taken in moves the engine's list: whether the
    // reads answer from the live version may have changed with it.
    if (ids != problem_ids_) {
        problem_ids_ = std::move(ids);
        readAuthoring();
    }
}

bool RenderFallbackNotice::receiveResult(qint64 id, const QByteArray &result_json) {
    const bool authoring = id != 0 && id == authoring_request_;
    if (!authoring && !repair_.owns(id)) return false;
    nlohmann::json result;
    try {
        result = nlohmann::json::parse(result_json.constData(), result_json.constData() + result_json.size());
    } catch (const std::exception &) {
        result = nlohmann::json::object();
    }
    if (!authoring) return repair_.receiveResult(id, result);
    authoring_request_ = 0;
    present(session_.observeAuthoringRead(result));
    // Decision 4: while a refused version is held, edits are based on its
    // digest; every panel that holds the live one reads again. (A version
    // that is not JSON has no reads of its own: the panels keep theirs.)
    const auto &reading = session_.authoring();
    if (reading.known && !reading.live && !reading.held_unparsed && !reading.digest.empty() &&
        hooks_.held_digest)
        hooks_.held_digest(reading.digest);
    if (std::exchange(authoring_again_, false)) readAuthoring();
    return true;
}

bool RenderFallbackNotice::receiveFailure(qint64 id, const QString &message) {
    if (id != 0 && id == authoring_request_) {
        authoring_request_ = 0;
        if (std::exchange(authoring_again_, false)) readAuthoring();
        return true;
    }
    return repair_.receiveFailure(id, message.toStdString());
}

void RenderFallbackNotice::showDocument() {
    const auto &notice = session_.notice();
    if (!notice.syntax || notice.document.empty()) return;
    dialog_filled_ = false;
    dialog_find_.clear();
    if (repair_.open(notice.document, notice.syntax_line, notice.syntax_column)) buildDialog();
    presentRepair();
}

void RenderFallbackNotice::showDocumentAt(const QString &document, std::int64_t line, std::int64_t column,
                                          const QString &find) {
    if (document.isEmpty()) return;
    dialog_filled_ = false;
    dialog_find_ = find;
    if (repair_.open(document.toStdString(), line, column)) buildDialog();
    presentRepair();
}

void RenderFallbackNotice::setSeeProblems(bool see) {
    if (see_problems_ == see) return;
    see_problems_ = see;
    present(true);
}

void RenderFallbackNotice::buildDialog() {
    if (dialog_) return;
    auto *dialog = new QDialog(window());
    dialog_ = dialog;
    dialog->setObjectName(QStringLiteral("pelican.renderDocumentDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(720, 560);
    auto *layout = new QVBoxLayout(dialog);
    auto *heading = new QLabel(dialog);
    heading->setObjectName(QStringLiteral("pelican.renderDocumentDialog.heading"));
    heading->setWordWrap(true);
    layout->addWidget(heading);
    dialog_text_ = new QPlainTextEdit(dialog);
    dialog_text_->setObjectName(QStringLiteral("pelican.renderDocumentDialog.text"));
    dialog_text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    dialog_text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(dialog_text_, 1);
    dialog_notice_ = new QLabel(dialog);
    dialog_notice_->setObjectName(QStringLiteral("pelican.renderDocumentDialog.notice"));
    applyPanelNoticeStyle(dialog_notice_);
    dialog_notice_->setWordWrap(true);
    layout->addWidget(dialog_notice_);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    dialog_save_ = new QPushButton(tr("Save"), dialog);
    dialog_save_->setObjectName(QStringLiteral("pelican.renderDocumentDialog.save"));
    dialog_save_->setToolTip(tr("Write the document whole (restore_render_documents); the engine "
                                "writes the file and composes it again"));
    auto *cancel = new QPushButton(tr("Cancel"), dialog);
    cancel->setObjectName(QStringLiteral("pelican.renderDocumentDialog.cancel"));
    buttons->addWidget(dialog_save_);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);

    connect(dialog_text_, &QPlainTextEdit::textChanged, dialog, [this] { checkDialogText(); });
    connect(dialog_save_, &QPushButton::clicked, dialog, [this] {
        if (dialog_text_ == nullptr) return;
        auto content = dialog_text_->toPlainText();
        if (dialog_crlf_) content.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
        (void)repair_.save(content.toStdString());
    });
    connect(cancel, &QPushButton::clicked, dialog, [dialog] { dialog->close(); });
    connect(dialog, &QObject::destroyed, this, [this] {
        dialog_text_ = nullptr;
        dialog_notice_ = nullptr;
        dialog_save_ = nullptr;
        if (repair_.state() != RenderDocumentRepair::State::Closed) repair_.close();
    });
    dialog->open();
}

void RenderFallbackNotice::checkDialogText() {
    if (dialog_text_ == nullptr || dialog_notice_ == nullptr || dialog_save_ == nullptr) return;
    const bool editing = repair_.state() == RenderDocumentRepair::State::Editing;
    if (!editing) {
        dialog_save_->setEnabled(false);
        return;
    }
    const auto spot = renderDocumentSyntaxOf(dialog_text_->toPlainText().toStdString());
    QString line;
    if (spot) {
        line = spot->line > 0 ? tr("Not valid JSON yet at line %1, column %2: %3")
                                    .arg(spot->line)
                                    .arg(spot->column)
                                    .arg(qtext(spot->reason))
                              : tr("Not valid JSON yet: %1").arg(qtext(spot->reason));
    } else if (!repair_.notice().empty()) {
        line = qtext(repair_.notice());
    } else {
        line = tr("Valid JSON. Save writes it to %1.").arg(qtext(repair_.reference()));
    }
    setPanelNotice(dialog_notice_, line, line);
    dialog_save_->setEnabled(!spot.has_value());
}

void RenderFallbackNotice::presentRepair() {
    if (!dialog_) return;
    using State = RenderDocumentRepair::State;
    const auto state = repair_.state();
    if (state == State::Closed) {
        if (repair_.notice().empty()) {
            // Committed (or closed): the window goes.
            dialog_->close();
        } else if (dialog_notice_ != nullptr) {
            setPanelNotice(dialog_notice_, qtext(repair_.notice()), qtext(repair_.notice()));
            if (dialog_save_ != nullptr) dialog_save_->setEnabled(false);
        }
        return;
    }
    if (auto *heading = dialog_->findChild<QLabel *>(QStringLiteral("pelican.renderDocumentDialog.heading"))) {
        heading->setText(state == State::Reading
                             ? tr("Reading the document\xE2\x80\xA6")
                             : tr("%1 as it is on disk. Fix it and Save: the file is replaced whole. "
                                  "This is for a document that is not JSON at all; the Graph and the "
                                  "Frame Plan edit it otherwise.")
                                   .arg(qtext(repair_.reference())));
    }
    if (state == State::Editing && !dialog_filled_ && dialog_text_ != nullptr) {
        dialog_filled_ = true;
        auto content = qtext(repair_.content());
        dialog_crlf_ = content.contains(QStringLiteral("\r\n"));
        content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        dialog_text_->setPlainText(content);
        // The cursor where the parser stopped (1-based line and column).
        QTextCursor cursor{dialog_text_->document()};
        const auto line = repair_.line();
        const QTextBlock block =
            line > 0 ? dialog_text_->document()->findBlockByNumber(static_cast<int>(line - 1)) : QTextBlock{};
        if (block.isValid()) {
            const int column = static_cast<int>(std::max<std::int64_t>(repair_.column(), 1)) - 1;
            cursor.setPosition(block.position() + std::min(column, std::max(0, block.length() - 1)));
        } else if (const QTextCursor found = dialog_find_.isEmpty()
                                                 ? QTextCursor{}
                                                 : dialog_text_->document()->find(dialog_find_);
                   !found.isNull()) {
            cursor.setPosition(found.selectionStart());
        } else {
            cursor.movePosition(QTextCursor::End);
        }
        dialog_text_->setTextCursor(cursor);
        dialog_text_->centerCursor();
        dialog_text_->setFocus(Qt::OtherFocusReason);
    }
    if (dialog_text_ != nullptr) dialog_text_->setReadOnly(state != State::Editing);
    checkDialogText();
    if (state == State::Editing && !repair_.notice().empty() && dialog_notice_ != nullptr)
        setPanelNotice(dialog_notice_, qtext(repair_.notice()), qtext(repair_.notice()));
    if (state == State::Saving && dialog_notice_ != nullptr)
        setPanelNotice(dialog_notice_, tr("Saving\xE2\x80\xA6"));
}

void RenderFallbackNotice::present(bool changed) {
    if (!changed) return;
    const auto &notice = session_.notice();
    // PB-1s, decision 4: the row says where the whole list is.
    const QString line = notice.shown && see_problems_ ? qtext(notice.line) + tr(" See Problems.")
                                                       : qtext(notice.line);
    setPanelNotice(text_, line,
                   notice.detail.empty() ? qtext(notice.line)
                                         : qtext(notice.line) + QLatin1Char('\n') + qtext(notice.detail));
    const bool restart = !notice.restart_feature.empty();
    reload_->setVisible(notice.shown && !restart);
    open_graph_->setVisible(notice.shown && !restart && !notice.node.empty());
    open_graph_->setToolTip(notice.node.empty() ? QString{} : tr("Show %1 on the Graph tab").arg(qtext(notice.node)));
    open_file_->setVisible(notice.shown && !restart && !notice.document.empty());
    open_file_->setToolTip(notice.document.empty() ? QString{} : tr("Open %1").arg(qtext(notice.document)));
    show_document_->setVisible(notice.shown && !restart && notice.syntax && !notice.document.empty() &&
                               static_cast<bool>(hooks_.request));
    restart_->setVisible(notice.shown && restart && static_cast<bool>(hooks_.restart));
    setVisible(notice.shown);
    // Said when it appears or changes, and once when it goes.
    const bool was_shown = std::exchange(shown_, notice.shown);
    if (hooks_.shown_changed && (notice.shown || was_shown)) hooks_.shown_changed(notice.shown, qtext(notice.line));
}

} // namespace PelicanStudio
