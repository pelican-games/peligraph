#include "enginefailuremodel.hpp"

#include <stdexcept>
#include <utility>

namespace PelicanStudio {

EngineFailureModel::EngineFailureModel(qsizetype maximum_lines)
    : maximum_lines_{maximum_lines} {
    if (maximum_lines_ <= 0) {
        throw std::invalid_argument(
            "Engine stderr line limit must be positive.");
    }
}

void EngineFailureModel::beginRun() {
    recent_standard_error_lines_.clear();
    pending_line_.clear();
    last_fatal_error_line_.clear();
}

void EngineFailureModel::appendStandardError(const QString &output) {
    pending_line_.append(output);
    for (;;) {
        const qsizetype newline = pending_line_.indexOf(QLatin1Char('\n'));
        if (newline < 0) {
            return;
        }

        QString line = pending_line_.first(newline);
        pending_line_.remove(0, newline + 1);
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        appendLine(std::move(line));
    }
}

std::optional<EngineFailurePresentation>
EngineFailureModel::finishRun(int exit_code) {
    if (!pending_line_.isEmpty()) {
        QString line = std::move(pending_line_);
        pending_line_.clear();
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        appendLine(std::move(line));
    }

    if (exit_code == 0) {
        return std::nullopt;
    }
    return EngineFailurePresentation{
        .fatal_error_line = last_fatal_error_line_,
    };
}

const QStringList &EngineFailureModel::recentStandardErrorLines() const noexcept {
    return recent_standard_error_lines_;
}

qsizetype EngineFailureModel::maximumLines() const noexcept {
    return maximum_lines_;
}

void EngineFailureModel::appendLine(QString line) {
    if (line.contains(QStringLiteral("Pelican fatal error"))) {
        last_fatal_error_line_ = line;
    }

    recent_standard_error_lines_.push_back(std::move(line));
    while (recent_standard_error_lines_.size() > maximum_lines_) {
        recent_standard_error_lines_.removeFirst();
    }
}

} // namespace PelicanStudio
