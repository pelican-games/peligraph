#pragma once

#include <QString>

namespace PelicanStudio {

class EngineLogBuffer {
  public:
    // One MiB keeps substantial diagnostic history while bounding both this
    // model and the text widget copy during long-running Studio sessions.
    static constexpr qsizetype DefaultMaximumBytes = 1024 * 1024;

    explicit EngineLogBuffer(qsizetype maximum_bytes = DefaultMaximumBytes);

    // Returns true when the oldest text had to be discarded.
    bool append(const QString &output);

    const QString &text() const noexcept;
    qsizetype maximumBytes() const noexcept;

  private:
    const qsizetype maximum_bytes_;
    QString text_;
};

} // namespace PelicanStudio
