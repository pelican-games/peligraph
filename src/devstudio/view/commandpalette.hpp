#pragma once

#include "commandpalettematch.hpp"

#include <QFrame>
#include <QPointer>

#include <functional>
#include <vector>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace PelicanStudio {

class CommandPalette final : public QFrame {
  public:
    explicit CommandPalette(QWidget &main_window);

    void present(std::vector<CommandPaletteRow> table, QWidget &invoking_window);
    bool eventFilter(QObject *watched, QEvent *event) override;

    std::function<void(const QString &token)> chosen;
    std::function<void()> dismissed;

  private:
    void rebuildRows();
    void choose(QTreeWidgetItem *item);

    QLineEdit *edit_ = nullptr;
    QTreeWidget *list_ = nullptr;
    std::vector<CommandPaletteRow> table_;
    std::vector<CommandPaletteRow> rows_;
    quint64 presentation_ = 0;
    bool input_method_composing_ = false;
};

} // namespace PelicanStudio
