#pragma once

#include "../model/problemsmodel.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QWidget>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace PelicanStudio {

// A row of the list carries its problem's id.
inline constexpr int ProblemIdRole = Qt::UserRole + 1;

// PB-1s. The Problems dock (beside Output and the Engine Log, View >
// Problems): one row a problem -- its severity, what is wrong, the document,
// the line and column, and where Studio fixes it -- from the engine's list
// (get_problems, read again when get_status.problems.revision moves) and the
// Assets dock's contents check. A click on a row goes where it is fixed (the
// owner's `open`); Restart engine, under the list, when only a restart fixes
// the selected one. Sorted by a click on a heading, narrowed by severity and
// by the words of the filter.
//
// Named for scenarios: pelican.problemsDock (the dock), pelican.problems.list
// (rows are `row#Error` / `row#Warning` by their first column),
// .severity (All / Errors / Warnings), .filter, .count ("2 errors, 1
// warning"), .detail (the selected row: "<document>:<line>:<column> —
// <sentence>"), .notice and .restart.
class ProblemsDock final : public QWidget {
  public:
    struct Hooks {
        // Sends one editor request; the id its reply comes back under, or 0
        // with `error` saying why.
        std::function<qint64(const QString &method, const QJsonObject &params, QString *error)> send;
        // Reads get_status (its reply comes back through observeStatus, as
        // every get_status does). Asked when nobody else read it for a
        // StatusPollMs.
        std::function<void()> read_status;
        // Where a click on the row goes (decision 3).
        std::function<void(const ProblemRow &row)> open;
        // Restart engine, with the reason.
        std::function<void(const QString &reason)> restart;
        // The count changed (decision 4).
        std::function<void(std::size_t errors, std::size_t warnings)> counted;
    };

    // get_status is read at least this often while the engine runs, so a
    // problem the engine meets on its own (its file watch) shows.
    static constexpr int StatusPollMs = 1000;

    explicit ProblemsDock(Hooks hooks, QWidget *parent = nullptr);
    ~ProblemsDock() override;

    void engineAvailable();
    void engineUnavailable();
    // Every get_status result, whoever asked.
    void observeStatus(const nlohmann::json &status);
    // A reply that may have changed the list (a reload, a restore): read it.
    void readNow();
    // The Assets dock's contents check.
    void setAssetIssues(const std::vector<AssetStoreVerification> &stores);
    // The replies to get_problems. False for an id that is not its own.
    bool receiveResult(qint64 request_id, const QByteArray &result_json);
    bool receiveFailure(qint64 request_id, const QString &message);

    // The row's click, by id (what a click on it does).
    void openProblem(const QString &id);

    [[nodiscard]] const ProblemsSession &session() const noexcept;
    [[nodiscard]] QTreeWidget *list() const noexcept;
    [[nodiscard]] QLabel *countLine() const noexcept;
    [[nodiscard]] QLabel *detailLine() const noexcept;
    [[nodiscard]] QComboBox *severityFilter() const noexcept;
    [[nodiscard]] QLineEdit *textFilter() const noexcept;
    [[nodiscard]] QPushButton *restartButton() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace PelicanStudio
