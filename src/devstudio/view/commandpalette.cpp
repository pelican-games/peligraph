#include "commandpalette.hpp"

#include "studiotheme.hpp"

#include <QApplication>
#include <QHeaderView>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QScreen>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace PelicanStudio {

CommandPalette::CommandPalette(QWidget &main_window) : QFrame(&main_window, Qt::Popup) {
    setObjectName(QStringLiteral("pelican.commandPalette"));
    setFrameShape(QFrame::StyledPanel);
    setFont(studioPanelFont());
    setAttribute(Qt::WA_NoMouseReplay);

    edit_ = new QLineEdit(this);
    edit_->setObjectName(QStringLiteral("pelican.commandPalette.edit"));
    edit_->setPlaceholderText(tr("Type a command name"));
    edit_->installEventFilter(this);
    setFocusProxy(edit_);

    list_ = new QTreeWidget(this);
    list_->setObjectName(QStringLiteral("pelican.commandPalette.list"));
    list_->setColumnCount(2);
    list_->setHeaderHidden(true);
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(edit_);
    layout->addWidget(list_);

    connect(edit_, &QLineEdit::textChanged, this, [this] { rebuildRows(); });
    connect(list_, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) { choose(item); });
}

void CommandPalette::present(std::vector<CommandPaletteRow> table,
                             QWidget &invoking_window) {
    ++presentation_;
    table_ = std::move(table);
    if (parentWidget() != &invoking_window) setParent(&invoking_window, Qt::Popup);
    edit_->clear();
    rebuildRows();

    const int width = std::min(fontMetrics().averageCharWidth() * 64,
                               invoking_window.width() * 9 / 10);
    const int visible_rows = std::min<int>(10, static_cast<int>(table_.size()));
    const int row_height = std::max(list_->sizeHintForRow(0), fontMetrics().height());
    list_->setFixedHeight(visible_rows * row_height + 2 * list_->frameWidth());
    resize(width, sizeHint().height());

    const QPoint local((invoking_window.width() - width) / 2, fontMetrics().height() * 3);
    QPoint global = invoking_window.mapToGlobal(local);
    QScreen *screen = invoking_window.screen();
    if (screen != nullptr) {
        const QRect available = screen->availableGeometry();
        global.setX(std::clamp(global.x(), available.left(),
                               available.right() - width + 1));
        global.setY(std::clamp(global.y(), available.top(),
                               available.bottom() - height() + 1));
    }
    move(global);
    show();
    raise();
    edit_->setFocus(Qt::PopupFocusReason);
    const quint64 presentation = presentation_;
    QTimer::singleShot(0, edit_, [this, presentation] {
        if (presentation != presentation_ || !isVisible() ||
            QApplication::activeModalWidget() != nullptr)
            return;
        edit_->setFocus(Qt::PopupFocusReason);
    });
}

bool CommandPalette::eventFilter(QObject *watched, QEvent *event) {
    if (watched == edit_ && event->type() == QEvent::InputMethod) {
        const auto *input = static_cast<QInputMethodEvent *>(event);
        input_method_composing_ = !input->preeditString().isEmpty();
        return false;
    }
    if (watched != edit_ || event->type() != QEvent::KeyPress)
        return QFrame::eventFilter(watched, event);

    const auto *key = static_cast<QKeyEvent *>(event);
    const bool composition_key = key->key() == Qt::Key_Return ||
                                 key->key() == Qt::Key_Enter || key->key() == Qt::Key_Escape;
    if (input_method_composing_ && composition_key) return false;

    if (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
        const int step = key->key() == Qt::Key_Down ? 1 : -1;
        const int current = list_->currentIndex().row();
        const int next = nextEnabledRow(rows_, current, step);
        list_->setCurrentItem(next >= 0 ? list_->topLevelItem(next) : nullptr);
        return true;
    }
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
        choose(list_->currentItem());
        return true;
    }
    if (key->key() == Qt::Key_Escape) {
        hide();
        if (dismissed) dismissed();
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

void CommandPalette::rebuildRows() {
    rows_ = filterCommandRows(edit_->text(), table_);
    list_->clear();
    for (const CommandPaletteRow &row : rows_) {
        auto *item = new QTreeWidgetItem(list_, {row.title, row.keys});
        item->setData(0, Qt::UserRole, row.token);
        Qt::ItemFlags flags = Qt::ItemFlags{};
        if (row.enabled) flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        item->setFlags(flags);
        if (row.checkable) {
            item->setCheckState(0, row.checked ? Qt::Checked : Qt::Unchecked);
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        }
    }
    const int current = firstEnabledRow(rows_);
    list_->setCurrentItem(current >= 0 ? list_->topLevelItem(current) : nullptr);
}

void CommandPalette::choose(QTreeWidgetItem *item) {
    if (item == nullptr || !(item->flags() & Qt::ItemIsEnabled)) return;
    const QString token = item->data(0, Qt::UserRole).toString();
    hide();
    if (chosen) chosen(token);
}

} // namespace PelicanStudio
