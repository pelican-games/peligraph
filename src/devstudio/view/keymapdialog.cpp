#include "keymapdialog.hpp"

#include "studiocommands.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QBrush>
#include <QColor>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

namespace PelicanStudio {
namespace {

constexpr int SurfaceColumn = 0;
constexpr int CommandColumn = 1;
constexpr int KeysColumn = 2;
constexpr int DefaultColumn = 3;

QString keysText(const QList<QKeySequence> &keys) {
    QStringList text;
    for (const QKeySequence &key : keys)
        text.push_back(key.toString(QKeySequence::NativeText));
    return text.join(QStringLiteral(" / "));
}

QList<QKeySequence> liveKeys(QObject *object) {
    if (auto *action = qobject_cast<QAction *>(object))
        return action->shortcuts();
    if (auto *shortcut = qobject_cast<QShortcut *>(object))
        return shortcut->keys();
    return {};
}

void appendUnique(QList<QKeySequence> &keys, const QList<QKeySequence> &more) {
    for (const QKeySequence &key : more)
        if (!key.isEmpty() && !keys.contains(key)) keys.push_back(key);
}

QKeySequenceEdit *makeEditor(const QString &id, const QKeySequence &sequence,
                             int binding_index, bool draft, QWidget *parent) {
    auto *editor = new QKeySequenceEdit(sequence, parent);
    // One name per binding: a command may have several recorders, and they all
    // carry the mandated name so a scenario can address any of them.
    editor->setObjectName(QStringLiteral("pelican.keymap.edit.") + id);
    editor->setProperty("pelican.keymap.binding.index", binding_index);
    editor->setProperty("pelican.keymap.binding.draft", draft);
    editor->setToolTip(
        KeymapDialog::tr("Record one key or key chord. Empty or modifier-only "
                         "input keeps the current binding."));
    return editor;
}

} // namespace

KeymapDialog::KeymapDialog(MainWindow &window, QWidget *parent)
    : QDialog(parent), window_(window) {
    setObjectName(QStringLiteral("pelican.keymap.dialog"));
    setWindowTitle(tr("Keyboard Shortcuts"));
    setModal(false);
    resize(980, 620);

    buildRows();
    buildTable();
    refreshPlan();
}

void KeymapDialog::buildRows() {
    struct Metadata {
        int surface = 0;
        QString title;
    };
    std::map<QString, Metadata> metadata;
    std::map<QString, QList<QKeySequence>> live;
    for (const KeymapObjectState &state : window_.keymap_objects_) {
        auto &command = metadata[state.id];
        if (command.title.isEmpty() && state.object != nullptr) {
            command.surface =
                state.object->property("pelican.command.surface").toInt();
            command.title =
                state.object->property("pelican.command.title").toString();
        }
        appendUnique(live[state.id], liveKeys(state.object));
    }

    const KeymapDefaults fixed = window_.keymap_fixed_keys_;
    std::set<QString> hand_ids;
    for (const StudioKey &key : studioKeys()) {
        const QString id = QString::fromLatin1(key.id);
        hand_ids.insert(id);
        Row row;
        row.id = id;
        row.surface = static_cast<int>(key.surface);
        row.title = QString::fromLatin1(key.title);
        row.fixed = true;
        const auto fixed_keys = fixed.find(id);
        if (fixed_keys != fixed.end()) row.fixed_keys = fixed_keys->second;
        rows_.push_back(std::move(row));
    }

    for (const auto &[id, command] : metadata) {
        if (hand_ids.contains(id)) continue;
        Row row;
        row.id = id;
        row.surface = command.surface;
        row.title = command.title;
        const auto defaults = window_.keymap_defaults_.find(id);
        if (defaults != window_.keymap_defaults_.end())
            row.defaults = defaults->second;
        pending_[id] = live[id];
        rows_.push_back(std::move(row));
    }

    int gesture_index = 0;
    for (const StudioGesture &gesture : studioGestures()) {
        Row row;
        row.id = QStringLiteral("gesture.%1").arg(gesture_index++);
        row.surface = static_cast<int>(gesture.surface);
        row.title = QString::fromLatin1(gesture.title);
        row.fixed = true;
        row.fixed_text = QString::fromLatin1(gesture.input);
        rows_.push_back(std::move(row));
    }

    std::stable_sort(rows_.begin(), rows_.end(), [](const Row &left, const Row &right) {
        if (left.surface != right.surface) return left.surface < right.surface;
        if (left.title != right.title) return left.title < right.title;
        return left.id < right.id;
    });
    for (std::size_t index = 0; index < rows_.size(); ++index)
        rows_[index].table_row = static_cast<int>(index);
}

void KeymapDialog::buildTable() {
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        tr("Each row is a command or gesture. Change a binding, add another one, or clear "
           "them all. Changes are checked before OK saves them; a rejected "
           "command is marked in red and OK stays disabled."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    table_ = new QTableWidget(static_cast<int>(rows_.size()), 4, this);
    table_->setObjectName(QStringLiteral("pelican.keymap.table"));
    table_->setHorizontalHeaderLabels(
        {tr("Surface"), tr("Command"), tr("Keys"), tr("Default")});
    table_->verticalHeader()->hide();
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setSectionResizeMode(
        SurfaceColumn, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(
        CommandColumn, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(KeysColumn,
                                                      QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(
        DefaultColumn, QHeaderView::ResizeToContents);

    for (const Row &row : rows_) {
        auto *surface = new QTableWidgetItem(
            QString::fromLatin1(commandSurfaceLabel(
                static_cast<CommandSurface>(row.surface))));
        surface->setFlags(Qt::ItemIsEnabled);
        table_->setItem(row.table_row, SurfaceColumn, surface);
        auto *command = new QTableWidgetItem(row.title);
        command->setFlags(Qt::ItemIsEnabled);
        table_->setItem(row.table_row, CommandColumn, command);
        // The fixed rows show the keys they actually hold in Default too;
        // the "fixed" note beside the recorder slot is what says they cannot
        // be edited.
        const QString default_text = row.fixed_text.isEmpty()
                                         ? keysText(row.fixed ? row.fixed_keys : row.defaults)
                                         : row.fixed_text;
        auto *defaults = new QTableWidgetItem(default_text);
        defaults->setFlags(Qt::ItemIsEnabled);
        table_->setItem(row.table_row, DefaultColumn, defaults);
        rebuildKeysCell(row.table_row);
    }
    layout->addWidget(table_, 1);

    auto *note = new QLabel(
        tr("While the engine viewport owns the keyboard, only single-stroke "
           "bindings are forwarded to Studio; a multi-stroke binding is kept "
           "here and works while a Studio panel has the focus."),
        this);
    note->setObjectName(QStringLiteral("pelican.keymap.note"));
    note->setWordWrap(true);
    layout->addWidget(note);

    error_label_ = new QLabel(this);
    error_label_->setObjectName(QStringLiteral("pelican.keymap.error"));
    error_label_->setWordWrap(true);
    error_label_->setStyleSheet(QStringLiteral("color: #c00000;"));
    QPalette error_palette = error_label_->palette();
    error_palette.setColor(QPalette::WindowText, QColor(0xc0, 0x00, 0x00));
    error_label_->setPalette(error_palette);
    error_label_->hide();
    layout->addWidget(error_label_);

    auto *buttons = new QDialogButtonBox(this);
    ok_button_ = buttons->addButton(QDialogButtonBox::Ok);
    ok_button_->setObjectName(QStringLiteral("pelican.keymap.ok"));
    auto *cancel = buttons->addButton(QDialogButtonBox::Cancel);
    cancel->setObjectName(QStringLiteral("pelican.keymap.cancel"));
    auto *reset_all = buttons->addButton(tr("Reset all"),
                                         QDialogButtonBox::ResetRole);
    reset_all->setObjectName(QStringLiteral("pelican.keymap.resetAll"));
    connect(ok_button_, &QPushButton::clicked, this,
            [this] { saveAndApply(); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(reset_all, &QPushButton::clicked, this, [this] {
        for (Row &row : rows_) {
            if (row.fixed) continue;
            pending_[row.id] = row.defaults;
        }
        for (const Row &row : rows_)
            if (!row.fixed)
                QTimer::singleShot(0, this,
                                   [this, table_row = row.table_row] {
                                       rebuildKeysCell(table_row);
                                   });
        refreshPlan();
    });
    layout->addWidget(buttons);
}

void KeymapDialog::rebuildKeysCell(int table_row) {
    if (table_row < 0 || table_row >= static_cast<int>(rows_.size())) return;
    Row &row = rows_[static_cast<std::size_t>(table_row)];
    rebuilding_ = true;
    drafts_.erase(row.id);

    auto *cell = new QWidget(table_);
    auto *cell_layout = new QVBoxLayout(cell);
    cell_layout->setContentsMargins(4, 4, 4, 4);
    cell_layout->setSpacing(3);

    if (row.fixed) {
        auto *keys = new QLabel(row.fixed_text.isEmpty() ? keysText(row.fixed_keys)
                                                         : row.fixed_text,
                                cell);
        keys->setObjectName(QStringLiteral("pelican.keymap.fixedKeys.") + row.id);
        cell_layout->addWidget(keys);
        auto *note = new QLabel(tr("fixed"), cell);
        note->setObjectName(QStringLiteral("pelican.keymap.fixedNote.") + row.id);
        cell_layout->addWidget(note);
        table_->setCellWidget(table_row, KeysColumn, cell);
        rebuilding_ = false;
        return;
    }

    auto *binding_host = new QWidget(cell);
    binding_host->setObjectName(
        QStringLiteral("pelican.keymap.bindings.") + row.id);
    auto *binding_layout = new QVBoxLayout(binding_host);
    binding_layout->setContentsMargins(0, 0, 0, 0);
    binding_layout->setSpacing(2);
    const QList<QKeySequence> keys = pending_[row.id];
    for (int index = 0; index < keys.size(); ++index) {
        auto *line = new QWidget(binding_host);
        auto *line_layout = new QHBoxLayout(line);
        line_layout->setContentsMargins(0, 0, 0, 0);
        auto *editor = makeEditor(row.id, keys[index], index, false, line);
        connect(editor, &QKeySequenceEdit::editingFinished, this,
                [this, table_row, index, editor] {
                    handleRecorderFinished(table_row, index, editor, false);
                });
        auto *remove = new QToolButton(line);
        remove->setText(QStringLiteral("\u00d7"));
        remove->setObjectName(QStringLiteral("pelican.keymap.remove.") + row.id);
        remove->setProperty("pelican.keymap.binding.index", index);
        remove->setToolTip(tr("Remove this binding"));
        connect(remove, &QToolButton::clicked, this,
                [this, table_row, index] { removeBinding(table_row, index); });
        line_layout->addWidget(editor, 1);
        line_layout->addWidget(remove);
        binding_layout->addWidget(line);
    }
    cell_layout->addWidget(binding_host);

    auto *actions = new QHBoxLayout();
    auto *add = new QToolButton(cell);
    add->setText(tr("Add"));
    add->setObjectName(QStringLiteral("pelican.keymap.add.") + row.id);
    add->setToolTip(tr("Add another key binding for this command"));
    connect(add, &QToolButton::clicked, this,
            [this, table_row] { addBinding(table_row); });
    auto *reset = new QPushButton(tr("Reset to default"), cell);
    reset->setObjectName(QStringLiteral("pelican.keymap.reset.") + row.id);
    connect(reset, &QPushButton::clicked, this,
            [this, table_row] { resetRow(table_row); });
    auto *clear = new QPushButton(tr("Clear"), cell);
    clear->setObjectName(QStringLiteral("pelican.keymap.clear.") + row.id);
    connect(clear, &QPushButton::clicked, this,
            [this, table_row] { clearRow(table_row); });
    actions->addWidget(add);
    actions->addWidget(reset);
    actions->addWidget(clear);
    actions->addStretch(1);
    cell_layout->addLayout(actions);

    auto *reason = new QLabel(cell);
    reason->setObjectName(QStringLiteral("pelican.keymap.reason.") + row.id);
    reason->setWordWrap(true);
    reason->setStyleSheet(QStringLiteral("color: #c00000;"));
    QPalette reason_palette = reason->palette();
    reason_palette.setColor(QPalette::WindowText, QColor(0xc0, 0x00, 0x00));
    reason->setPalette(reason_palette);
    cell_layout->addWidget(reason);

    table_->setCellWidget(table_row, KeysColumn, cell);
    table_->resizeRowToContents(table_row);
    rebuilding_ = false;
    refreshPlan();
}

void KeymapDialog::refreshPlan() {
    plan_ = planKeymap(window_.keymap_defaults_, window_.keymap_fixed_keys_,
                       overrides());
    for (const Row &row : rows_) {
        QWidget *cell = table_->cellWidget(row.table_row, KeysColumn);
        QLabel *reason =
            cell != nullptr
                ? cell->findChild<QLabel *>(
                      QStringLiteral("pelican.keymap.reason.") + row.id)
                : nullptr;
        QTableWidgetItem *command = table_->item(row.table_row, CommandColumn);
        const auto rejection = plan_.rejected.find(row.id);
        const bool rejected = rejection != plan_.rejected.end();
        if (reason != nullptr) reason->setText(rejected ? rejection->second.reason
                                                        : QString());
        if (command != nullptr) {
            if (rejected)
                command->setForeground(QBrush(QColor(0xc0, 0x00, 0x00)));
            else
                command->setForeground(QBrush());
        }
    }
    if (ok_button_ != nullptr) ok_button_->setEnabled(plan_.rejected.empty());
}

std::vector<KeymapEntry> KeymapDialog::overrides() const {
    std::vector<KeymapEntry> entries;
    for (const Row &row : rows_) {
        if (row.fixed) continue;
        const auto keys = pending_.find(row.id);
        if (keys == pending_.end()) continue;
        if (keys->second == row.defaults) continue;
        entries.push_back(KeymapEntry{row.id, keys->second});
    }
    return entries;
}

void KeymapDialog::resetRow(int table_row) {
    if (table_row < 0 || table_row >= static_cast<int>(rows_.size())) return;
    const Row &row = rows_[static_cast<std::size_t>(table_row)];
    if (row.fixed) return;
    pending_[row.id] = row.defaults;
    refreshPlan();
    QTimer::singleShot(0, this, [this, table_row] {
        rebuildKeysCell(table_row);
    });
}

void KeymapDialog::clearRow(int table_row) {
    if (table_row < 0 || table_row >= static_cast<int>(rows_.size())) return;
    const Row &row = rows_[static_cast<std::size_t>(table_row)];
    if (row.fixed) return;
    pending_[row.id].clear();
    refreshPlan();
    QTimer::singleShot(0, this, [this, table_row] {
        rebuildKeysCell(table_row);
    });
}

void KeymapDialog::addBinding(int table_row) {
    if (table_row < 0 || table_row >= static_cast<int>(rows_.size())) return;
    const Row &row = rows_[static_cast<std::size_t>(table_row)];
    if (row.fixed) return;
    const auto existing = drafts_.find(row.id);
    if (existing != drafts_.end() && existing->second != nullptr) {
        existing->second->setFocus();
        return;
    }
    QWidget *cell = table_->cellWidget(table_row, KeysColumn);
    QWidget *host =
        cell != nullptr
            ? cell->findChild<QWidget *>(
                  QStringLiteral("pelican.keymap.bindings.") + row.id)
            : nullptr;
    auto *layout = host != nullptr ? qobject_cast<QVBoxLayout *>(host->layout())
                                   : nullptr;
    if (layout == nullptr) return;
    auto *line = new QWidget(host);
    auto *line_layout = new QHBoxLayout(line);
    line_layout->setContentsMargins(0, 0, 0, 0);
    auto *editor = makeEditor(row.id, QKeySequence(), pending_[row.id].size(),
                              true, line);
    drafts_[row.id] = editor;
    connect(editor, &QKeySequenceEdit::editingFinished, this,
            [this, table_row, editor] {
                handleRecorderFinished(table_row, pending_[rows_[static_cast<std::size_t>(table_row)].id].size(),
                                       editor, true);
            });
    line_layout->addWidget(editor, 1);
    layout->addWidget(line);
    editor->setFocus();
}

void KeymapDialog::removeBinding(int table_row, int binding_index) {
    if (table_row < 0 || table_row >= static_cast<int>(rows_.size())) return;
    const Row &row = rows_[static_cast<std::size_t>(table_row)];
    if (row.fixed) return;
    QList<QKeySequence> &keys = pending_[row.id];
    if (binding_index < 0 || binding_index >= keys.size()) return;
    keys.removeAt(binding_index);
    refreshPlan();
    QTimer::singleShot(0, this, [this, table_row] {
        rebuildKeysCell(table_row);
    });
}

void KeymapDialog::handleRecorderFinished(int table_row, int binding_index,
                                          QKeySequenceEdit *editor, bool draft) {
    if (rebuilding_ || editor == nullptr ||
        table_row < 0 || table_row >= static_cast<int>(rows_.size()))
        return;
    const Row &row = rows_[static_cast<std::size_t>(table_row)];
    if (row.fixed) return;
    const QKeySequence sequence = editor->keySequence();
    if (sequence.isEmpty()) {
        // A modifier-only press, a bare Backspace-to-empty or a focus move
        // before a complete chord must not unbind: Clear is the way to do that.
        if (draft) {
            drafts_.erase(row.id);
            if (QWidget *line = editor->parentWidget()) line->deleteLater();
        } else {
            const QList<QKeySequence> &keys = pending_[row.id];
            if (binding_index >= 0 && binding_index < keys.size()) {
                const QSignalBlocker blocker(editor);
                editor->setKeySequence(keys[binding_index]);
            }
        }
        return;
    }
    if (draft) {
        pending_[row.id].push_back(sequence);
        drafts_.erase(row.id);
        refreshPlan();
        QTimer::singleShot(0, this, [this, table_row] {
            rebuildKeysCell(table_row);
        });
        return;
    }
    QList<QKeySequence> &keys = pending_[row.id];
    if (binding_index < 0 || binding_index >= keys.size()) return;
    if (keys[binding_index] == sequence) return;
    keys[binding_index] = sequence;
    refreshPlan();
}

void KeymapDialog::showSaveError(const QString &message) {
    if (error_label_ == nullptr) return;
    error_label_->setText(tr("The keymap could not be saved: %1").arg(message));
    error_label_->show();
}

bool KeymapDialog::saveAndApply() {
    if (error_label_ != nullptr) error_label_->hide();
    // Re-run the plan at the moment of saving: OK was disabled while a
    // rejection stood, and a stale plan must never be written.
    refreshPlan();
    if (!plan_.rejected.empty()) return false;

    QJsonObject bindings;
    for (const KeymapEntry &entry : overrides()) {
        QJsonArray keys;
        for (const QKeySequence &key : entry.keys)
            keys.push_back(key.toString(QKeySequence::PortableText));
        bindings.insert(entry.id, keys);
    }
    const QString path =
        QDir(window_.application_config_directory_)
            .filePath(QStringLiteral("keymap.json"));
    const bool had_file = QFileInfo::exists(path);
    QByteArray previous;
    if (had_file) {
        QFile current(path);
        if (!current.open(QIODevice::ReadOnly)) {
            showSaveError(current.errorString());
            return false;
        }
        previous = current.readAll();
    }
    // A command this build has no row for (a newer Studio's, say) is not the
    // dialog's to remove: its entry stays in the file as it was.
    const QJsonObject on_disk = QJsonDocument::fromJson(previous)
                                    .object()
                                    .value(QStringLiteral("bindings"))
                                    .toObject();
    for (auto entry = on_disk.begin(); entry != on_disk.end(); ++entry) {
        const bool has_row = std::any_of(rows_.begin(), rows_.end(), [&](const Row &row) {
            return row.id == entry.key();
        });
        if (!has_row) bindings.insert(entry.key(), entry.value());
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("bindings"), bindings);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);

    QString error;
    // The backup is committed first.  If it fails nothing has been replaced,
    // and the dialog leaves the live keys as they were.
    if (had_file && !window_.writeKeymapFile(path + QStringLiteral(".bak"), previous, &error)) {
        showSaveError(error);
        return false;
    }
    // The old keymap.json is still there until this commit.  If it fails the
    // file is untouched, and the live keys below are not reached.
    if (!window_.writeKeymapFile(path, bytes, &error)) {
        showSaveError(error);
        return false;
    }
    applyKeymapPlan(window_.keymap_objects_, plan_);
    accept();
    return true;
}

} // namespace PelicanStudio
