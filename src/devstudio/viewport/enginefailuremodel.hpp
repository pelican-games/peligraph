#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace PelicanStudio {

struct EngineFailurePresentation {
    // Empty when the process failed without producing Pelican's fatal marker.
    QString fatal_error_line;
};

class EngineFailureModel {
  public:
    static constexpr qsizetype DefaultMaximumLines = 200;

    explicit EngineFailureModel(
        qsizetype maximum_lines = DefaultMaximumLines);

    void beginRun();
    void appendStandardError(const QString &output);
    std::optional<EngineFailurePresentation> finishRun(int exit_code);

    const QStringList &recentStandardErrorLines() const noexcept;
    // MG-1. The line a player writes when the project it opens needs
    // migrating ("project.migration_required@1: {...}"), once this run has
    // written it; empty before.
    const QString &migrationRequiredLine() const noexcept {
        return migration_required_line_;
    }
    qsizetype maximumLines() const noexcept;

  private:
    const qsizetype maximum_lines_;
    QStringList recent_standard_error_lines_;
    QString pending_line_;
    QString last_fatal_error_line_;
    QString migration_required_line_;
    QString last_problems_line_;

    void appendLine(QString line);
};

} // namespace PelicanStudio
