#include "enginelogbuffer.hpp"

#include <QByteArray>

#include <stdexcept>

namespace PelicanStudio {

EngineLogBuffer::EngineLogBuffer(qsizetype maximum_bytes) : maximum_bytes_(maximum_bytes) {
    if (maximum_bytes_ <= 0) {
        throw std::invalid_argument("Engine log byte limit must be positive.");
    }
}

bool EngineLogBuffer::append(const QString &output) {
    if (output.isEmpty()) {
        return false;
    }

    text_.append(output);
    const QByteArray utf8 = text_.toUtf8();
    if (utf8.size() <= maximum_bytes_) {
        return false;
    }

    qsizetype first_kept_byte = utf8.size() - maximum_bytes_;
    while (first_kept_byte < utf8.size() &&
           (static_cast<unsigned char>(utf8.at(first_kept_byte)) & 0xc0U) == 0x80U) {
        ++first_kept_byte;
    }

    // Prefer a complete oldest line when at least one complete line remains.
    // A single oversized line still keeps its bounded tail instead of becoming
    // an empty log.
    const qsizetype next_line = utf8.indexOf('\n', first_kept_byte);
    if (next_line >= 0 && next_line + 1 < utf8.size()) {
        first_kept_byte = next_line + 1;
    }

    text_ = QString::fromUtf8(
        utf8.constData() + first_kept_byte, utf8.size() - first_kept_byte);
    return true;
}

const QString &EngineLogBuffer::text() const noexcept {
    return text_;
}

qsizetype EngineLogBuffer::maximumBytes() const noexcept {
    return maximum_bytes_;
}

} // namespace PelicanStudio
