#pragma once

// KM-1a: the Studio's keyboard commands described once, beside the code that
// handles them.  The Help > Keyboard Shortcuts table is generated from these
// declarations; nothing in that table is typed by hand.

#include <QAction>
#include <QKeyEvent>
#include <QKeySequence>
#include <QObject>
#include <QShortcut>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace PelicanStudio {

enum class CommandSurface {
    Window,
    Scene,
    Rendering,
    Graph,
    FramePlan,
    GraphAndFramePlan,
    Materials,
    Assets,
    TextEntry,
};

inline const char *commandSurfaceLabel(CommandSurface surface) {
    switch (surface) {
    case CommandSurface::Window:
        return "Window";
    case CommandSurface::Scene:
        return "Scene: Viewport, Outliner, Inspector, Problems, Play bar";
    case CommandSurface::Rendering:
        return "Rendering: Frame Plan, Pass Editor, Features, Settings";
    case CommandSurface::Graph:
        return "Graph tab";
    case CommandSurface::FramePlan:
        return "Frame Plan";
    case CommandSurface::GraphAndFramePlan:
        return "Graph / Frame Plan";
    case CommandSurface::Materials:
        return "Materials";
    case CommandSurface::Assets:
        return "Assets";
    case CommandSurface::TextEntry:
        return "Text fields / inline editors";
    }
    return "";
}

// Stamp the machine-readable description on the object that really handles the
// command (an action or a shortcut).  The help table reads these properties
// back; the handler's branch body never learns that they exist.
// `viewport_order` is this command's position in the handler that forwards a
// key from the engine's own window; 0 means the command is not forwarded.  An
// int, not a bool, because the old hand-written list ran these commands in a
// fixed order and the first match wins (KM-2a).
inline void setCommandHelp(QObject &object, const char *id, CommandSurface surface,
                           const QString &title, int viewport_order = 0) {
    object.setProperty("pelican.command.id", QString::fromLatin1(id));
    object.setProperty("pelican.command.surface", static_cast<int>(surface));
    object.setProperty("pelican.command.title", title);
    object.setProperty("pelican.command.viewport", viewport_order);
}

enum class KeyMatch {
    StandardUndo,
    StandardRedo,
    KeyOnly,
    ExactNoModifiers,
};

struct StudioKey {
    const char *id;
    CommandSurface surface;
    KeyMatch match;
    std::vector<Qt::Key> keys;
    const char *title;
};

inline const std::vector<StudioKey> &studioKeys() {
    static const std::vector<StudioKey> keys = {
        {"graph.undo", CommandSurface::Graph, KeyMatch::StandardUndo, {}, "Undo graph edit"},
        {"graph.redo", CommandSurface::Graph, KeyMatch::StandardRedo, {}, "Redo graph edit"},
        {"graph.delete", CommandSurface::Graph, KeyMatch::KeyOnly,
         {Qt::Key_Delete, Qt::Key_Backspace}, "Delete selected"},
        {"graph.fit", CommandSurface::Graph, KeyMatch::ExactNoModifiers, {Qt::Key_F}, "Fit view"},
        {"frameplan.fit", CommandSurface::FramePlan, KeyMatch::ExactNoModifiers, {Qt::Key_F},
         "Fit view"},
        {"frameplan.delete", CommandSurface::FramePlan, KeyMatch::KeyOnly,
         {Qt::Key_Delete, Qt::Key_Backspace}, "Delete selected"},
        {"frameplan.escape", CommandSurface::FramePlan, KeyMatch::KeyOnly, {Qt::Key_Escape},
         "Cancel edit / leave group"},
        {"inline.cancel", CommandSurface::TextEntry, KeyMatch::KeyOnly, {Qt::Key_Escape},
         "Cancel inline edit"},
        {"inline.commit", CommandSurface::TextEntry, KeyMatch::KeyOnly,
         {Qt::Key_Return, Qt::Key_Enter}, "Commit inline edit"},
    };
    return keys;
}

inline bool studioKeyMatches(const char *id, const QKeyEvent &event) {
    const std::vector<StudioKey> &declared = studioKeys();
    const auto key = std::find_if(declared.begin(), declared.end(),
                                  [id](const StudioKey &candidate) {
                                      return std::strcmp(candidate.id, id) == 0;
                                  });
    if (key == declared.end()) {
        Q_ASSERT_X(false, "studioKeyMatches", "unknown Studio command id");
        return false;
    }
    switch (key->match) {
    case KeyMatch::StandardUndo:
        return event.matches(QKeySequence::Undo);
    case KeyMatch::StandardRedo:
        return event.matches(QKeySequence::Redo);
    case KeyMatch::KeyOnly:
        return std::find(key->keys.begin(), key->keys.end(), event.key()) != key->keys.end();
    case KeyMatch::ExactNoModifiers:
        return event.modifiers() == Qt::NoModifier &&
               std::find(key->keys.begin(), key->keys.end(), event.key()) != key->keys.end();
    }
    return false;
}

struct StudioGesture {
    CommandSurface surface;
    const char *input;
    const char *title;
};

inline const std::vector<StudioGesture> &studioGestures() {
    static const std::vector<StudioGesture> gestures = {
        {CommandSurface::GraphAndFramePlan, "Middle button drag", "Pan"},
        {CommandSurface::GraphAndFramePlan, "Space + drag", "Pan"},
        {CommandSurface::GraphAndFramePlan, "Wheel", "Zoom"},
        {CommandSurface::Graph, "Ctrl + click", "Add to selection"},
        {CommandSurface::Graph, "Double-click an edge", "Insert a reroute"},
        {CommandSurface::Graph, "Shift + drag a curve key", "Move the key in time only"},
        {CommandSurface::Graph, "Ctrl + drag a curve key", "Move the key in value only"},
        {CommandSurface::FramePlan, "Double-click a resource", "Edit the resource"},
    };
    return gestures;
}

namespace detail {

struct CommandRow {
    CommandSurface surface = CommandSurface::Window;
    QString title;
    std::vector<QKeySequence> keys;
    // Gesture rows carry their input text; they have no key sequence.
    QString input;
};

inline void appendUnique(std::vector<QKeySequence> &keys, const QList<QKeySequence> &sequences) {
    for (const QKeySequence &sequence : sequences) {
        if (sequence.isEmpty()) continue;
        if (std::find(keys.begin(), keys.end(), sequence) == keys.end()) keys.push_back(sequence);
    }
}

inline QString shortcutText(const CommandRow &row) {
    if (!row.input.isEmpty()) return row.input;
    QStringList text;
    for (const QKeySequence &key : row.keys) {
        QString name = key.toString(QKeySequence::NativeText);
        // KB-1 T5, and the table before it, spell these two out. Qt's native
        // text abbreviates them ("Del", "Esc") on this platform; the help has
        // always shown the full words, so keep them.
        if (name == QLatin1String("Del")) name = QStringLiteral("Delete");
        else if (name == QLatin1String("Esc")) name = QStringLiteral("Escape");
        text.push_back(name);
    }
    return text.join(QStringLiteral(" / "));
}

} // namespace detail

inline std::vector<std::array<QString, 3>> studioShortcutRows(QWidget &main_window) {
    std::map<QString, detail::CommandRow> commands;
    std::set<QObject *> seen;

    const auto collect = [&](QObject *object) {
        if (object == nullptr || !seen.insert(object).second) return;
        QList<QKeySequence> keys;
        if (auto *action = qobject_cast<QAction *>(object)) {
            keys = action->shortcuts();
        } else if (auto *shortcut = qobject_cast<QShortcut *>(object)) {
            keys = shortcut->keys();
        } else {
            return;
        }
        const QVariant id_property = object->property("pelican.command.id");
        if (!id_property.isValid()) return;
        const QString id = id_property.toString();
        detail::CommandRow &command = commands[id];
        command.surface = static_cast<CommandSurface>(
            object->property("pelican.command.surface").toInt());
        command.title = object->property("pelican.command.title").toString();
        detail::appendUnique(command.keys, keys);
    };

    for (QObject *object : main_window.findChildren<QObject *>()) {
        if (qobject_cast<QAction *>(object) != nullptr ||
            qobject_cast<QShortcut *>(object) != nullptr) {
            collect(object);
        }
    }
    for (QAction *action : main_window.actions()) collect(action);
    for (QWidget *widget : main_window.findChildren<QWidget *>()) {
        for (QAction *action : widget->actions()) collect(action);
    }

    // Commands whose key test has no action of its own (StandardKey handlers and
    // keys matched directly in an event handler) are declared here instead.
    for (const StudioKey &key : studioKeys()) {
        const QString id = QString::fromLatin1(key.id);
        detail::CommandRow &command = commands[id];
        if (command.title.isEmpty()) {
            command.surface = key.surface;
            command.title = QString::fromLatin1(key.title);
        }
        switch (key.match) {
        case KeyMatch::StandardUndo:
            detail::appendUnique(command.keys, QKeySequence::keyBindings(QKeySequence::Undo));
            break;
        case KeyMatch::StandardRedo:
            detail::appendUnique(command.keys, QKeySequence::keyBindings(QKeySequence::Redo));
            break;
        case KeyMatch::KeyOnly:
        case KeyMatch::ExactNoModifiers: {
            QList<QKeySequence> sequences;
            for (Qt::Key pressed : key.keys) sequences.push_back(QKeySequence(pressed));
            detail::appendUnique(command.keys, sequences);
            break;
        }
        }
    }

    std::vector<detail::CommandRow> rows;
    rows.reserve(commands.size() + studioGestures().size());
    for (const auto &[id, command] : commands) {
        Q_UNUSED(id);
        rows.push_back(command);
    }
    for (const StudioGesture &gesture : studioGestures()) {
        detail::CommandRow row;
        row.surface = gesture.surface;
        row.title = QString::fromLatin1(gesture.title);
        row.input = QString::fromLatin1(gesture.input);
        rows.push_back(row);
    }

    std::stable_sort(rows.begin(), rows.end(), [](const detail::CommandRow &left,
                                                  const detail::CommandRow &right) {
        if (left.surface != right.surface) {
            return static_cast<int>(left.surface) < static_cast<int>(right.surface);
        }
        if (left.title != right.title) return left.title < right.title;
        return detail::shortcutText(left) < detail::shortcutText(right);
    });

    std::vector<std::array<QString, 3>> result;
    result.reserve(rows.size());
    for (const detail::CommandRow &row : rows) {
        result.push_back({QString::fromLatin1(commandSurfaceLabel(row.surface)),
                          detail::shortcutText(row), row.title});
    }
    return result;
}

} // namespace PelicanStudio
