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
    qsizetype maximumLines() const noexcept;

  private:
    const qsizetype maximum_lines_;
    QStringList recent_standard_error_lines_;
    QString pending_line_;
    QString last_fatal_error_line_;

    void appendLine(QString line);
};

} // namespace PelicanStudio
