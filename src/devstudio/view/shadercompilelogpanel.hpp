#pragma once

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QToolButton;

namespace PelicanStudio {

// F-3. Where a shader that did not compile is shown: under the form that sent
// it, open when it arrives and foldable to its first line, the whole log in a
// fixed-width face with its line numbers and every `file:line` it names
// picked out. It
// holds text and nothing else, so the form above it keeps what was typed.
class ShaderCompileLogPanel final : public QWidget {
  public:
    explicit ShaderCompileLogPanel(QWidget *parent = nullptr);

    // An engine message that carries one or more "Shader compile failed"
    // entries. Returns false, and leaves the panel as it was, for a message
    // that carries none -- that one is not a compile log.
    bool showMessage(const QString &message);
    void clear();

    [[nodiscard]] bool expanded() const;
    void setExpanded(bool expanded);
    [[nodiscard]] QString logText() const;

  private:
    QToolButton *toggle_ = nullptr;
    QLabel *summary_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
};

} // namespace PelicanStudio
