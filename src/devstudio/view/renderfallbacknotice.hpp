#pragma once

#include "../model/renderfallback.hpp"

#include <QByteArray>
#include <QFrame>
#include <QPointer>
#include <QString>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class QDialog;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace PelicanStudio {

// BS-1s, decisions 2-4 and 8. The row above the viewport while the engine
// draws its placeholder because the project's rendering setup was refused at
// start (RenderFallbackSession says what): the sentence, Reload from disk,
// Open in Graph when the refusal names a node, Open file when it names a
// file, Show the document when the document is not JSON at all (engine
// BS-1b), and Restart engine when only a restart enables what the fixed setup
// needs. Hidden while there is nothing to say.
// RR-1s, decision 1: the same row while a reload stands refused and the
// panels show the version on disk (engine RR-1, `live: false`) -- the
// placeholder's sentence first when both are true.
//
// Named for scenarios: pelican.renderFallbackNotice (the row), .text, .reload,
// .openGraph, .openFile, .showDocument, .restart; the document's window
// pelican.renderDocumentDialog with .text, .notice, .save and .cancel.
class RenderFallbackNotice final : public QFrame {
  public:
    struct Hooks {
        // Reload from disk: reload_render_documents (the window's RL-1s reload).
        std::function<void()> reload;
        // Open in Graph: the Graph tab, on the node.
        std::function<void(const QString &node)> open_graph;
        // Open file: the file as the project names it ("passes/main.json").
        std::function<void(const QString &document)> open_file;
        // Restart engine (decision 4), with the reason it is asked for.
        std::function<void(const QString &reason)> restart;
        // Reads get_status again (its reply comes back through
        // observeStatus, as every get_status does).
        std::function<void()> read_status;
        // Decision 8: an editor request of the document's window (its reply
        // comes back through receiveResult / receiveFailure), and its clock.
        std::function<std::int64_t(const std::string &method, const nlohmann::json &params, std::string *error)>
            request;
        std::function<void(int milliseconds, std::function<void()> task)> schedule;
        // The document's restore committed (restore_render_documents).
        std::function<void(const nlohmann::json &commit)> restored;
        // RR-1s, decision 4: the digest of the version on disk the engine
        // holds after a refused reload -- what every structural edit is
        // based on now (the window's RenderConfigDigest).
        std::function<void(const std::string &digest)> held_digest;
        // The notice appeared (true) or went away (false), with its line.
        std::function<void(bool shown, const QString &line)> shown_changed;
    };

    explicit RenderFallbackNotice(Hooks hooks, QWidget *parent = nullptr);
    ~RenderFallbackNotice() override;

    // Every get_status reply.
    void observeStatus(const nlohmann::json &reply);
    // A refused reload_render_documents ({code, message, ...}).
    void observeReloadRefusal(const nlohmann::json &error);
    // A render edit's or a reload's reply: get_status is read again after
    // one that may have published another configuration (decision 3).
    void observeReply(const nlohmann::json &reply);
    // A reload committed: the configuration may be the project's again.
    void reloadCommitted();
    // RR-1s, decision 1: reads get_graph_instances, whose `live` and
    // held_unparsed say whether the reads answer from the version the picture
    // was made from (request; its reply comes back through receiveResult).
    // One read at a time; one asked for meanwhile follows it.
    void readAuthoring();
    // RR-1s, decision 1: the Problems list's rows (its sentence for the held
    // version); a list that changed reads get_graph_instances again.
    void observeProblems(const std::vector<ProblemRow> &rows);
    void engineGone();
    // Decision 8: the document window's replies. True when nothing else
    // should read the reply (a committed restore is read as every commit).
    bool receiveResult(qint64 id, const QByteArray &result_json);
    bool receiveFailure(qint64 id, const QString &message);

    [[nodiscard]] const RenderFallbackSession &session() const noexcept { return session_; }
    [[nodiscard]] const RenderDocumentRepair &repair() const noexcept { return repair_; }
    [[nodiscard]] QLabel *line() const noexcept { return text_; }
    // The document's window, while it is open.
    [[nodiscard]] QDialog *documentDialog() const noexcept { return dialog_.data(); }
    // Show the document (the button's click).
    void showDocument();
    // PB-1s, decision 3: the same window on any render document the engine
    // lists, from a row of the Problems dock -- the cursor at line / column
    // (1-based), or, without a line, at the first `find` in the text (a
    // node's id), else at the end.
    // RR-1s, decision 2: Save restores it against the digest listed, after a
    // refused reload as on the placeholder.
    void showDocumentAt(const QString &document, std::int64_t line, std::int64_t column,
                        const QString &find = {});
    // PB-1s, decision 4: the sentence ends "See Problems." while the window
    // has the Problems dock.
    void setSeeProblems(bool see);

  private:
    Hooks hooks_;
    RenderFallbackSession session_;
    RenderDocumentRepair repair_;
    QLabel *text_ = nullptr;
    QPushButton *reload_ = nullptr;
    QPushButton *open_graph_ = nullptr;
    QPushButton *open_file_ = nullptr;
    QPushButton *show_document_ = nullptr;
    QPushButton *restart_ = nullptr;
    bool shown_ = false;
    QPointer<QDialog> dialog_;
    QPlainTextEdit *dialog_text_ = nullptr;
    QLabel *dialog_notice_ = nullptr;
    QPushButton *dialog_save_ = nullptr;
    // The document's line ends were CRLF: what is saved keeps them.
    bool dialog_crlf_ = false;
    // What the editor was filled with last (the repair's content).
    bool dialog_filled_ = false;
    // Where the cursor goes without a line (showDocumentAt).
    QString dialog_find_;
    bool see_problems_ = false;
    // RR-1s: the get_graph_instances in flight, and whether another follows.
    std::int64_t authoring_request_ = 0;
    bool authoring_again_ = false;
    // The ids of the Problems rows last seen.
    std::vector<std::string> problem_ids_;

    void present(bool changed);
    void presentRepair();
    void buildDialog();
    void checkDialogText();
};

} // namespace PelicanStudio
