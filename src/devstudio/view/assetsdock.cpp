#include "assetsdock.hpp"

#include "panelhelp.hpp"
#include "studiotheme.hpp"

#include <QAction>
#include <QBrush>
#include <QFileSystemWatcher>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

constexpr int NameColumn = 0;
constexpr int ReloadColumn = 1;
constexpr int ContentsColumn = 2;

QString qtext(const std::string &text) { return QString::fromStdString(text); }

QString nativePath(const std::filesystem::path &path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

// How a project path reads on a row's tooltip: without the assets/ every
// row of the first root starts with.
QString shownPath(const std::string &logical) {
    constexpr std::string_view assets = "assets/";
    if (logical.starts_with(assets) && logical.size() > assets.size())
        return qtext(logical.substr(assets.size()));
    return qtext(logical);
}

} // namespace

struct AssetsDock::Impl {
    AssetsDock &owner;
    Hooks hooks;
    AssetsDockSession session;
    QLabel *watch_line = nullptr;
    QToolButton *undo_button = nullptr;
    QTreeWidget *tree = nullptr;
    QLabel *notice = nullptr;
    QPushButton *restart = nullptr;
    QPushButton *track = nullptr;
    QFrame *verification = nullptr;
    QLabel *summary = nullptr;
    QToolButton *details = nullptr;
    QPushButton *verify = nullptr;
    QTreeWidget *issues = nullptr;
    QWidget *store_rows = nullptr;
    QVBoxLayout *store_rows_layout = nullptr;
    std::map<std::string, QPushButton *> store_buttons;
    QWidget *confirm_row = nullptr;
    QLabel *confirm_label = nullptr;
    QPushButton *confirm = nullptr;
    QPushButton *cancel = nullptr;
    std::string confirm_store;
    QAction *reload_action = nullptr;
    QAction *undo_action = nullptr;
    QFileSystemWatcher *watcher = nullptr;
    QTimer *rescan_timer = nullptr;
    std::vector<AssetRoot> roots;
    std::vector<AssetEntry> entries;
    bool scanned = false;
    // logical path -> whether it is a folder, for what the tree listed.
    std::map<std::string, bool> folders;
    bool updating_tree = false;
    std::set<std::string> placed_missing;
    // The files with a mark that were already opened to.
    std::set<std::string> revealed;

    Impl(AssetsDock &dock, Hooks dock_hooks)
        : owner{dock}, hooks{std::move(dock_hooks)},
          session{AssetsDockSession::Transport{
              .request = [this](const std::string &method, const Json &params,
                                std::string *error) -> std::int64_t {
                  if (!hooks.send) return 0;
                  const auto object =
                      QJsonDocument::fromJson(QByteArray::fromStdString(params.dump())).object();
                  QString message;
                  const qint64 id = hooks.send(qtext(method), object, &message);
                  if (id == 0 && error != nullptr) *error = message.toStdString();
                  return id;
              },
              .schedule = [this](int milliseconds, std::function<void()> task) {
                  QTimer::singleShot(milliseconds, &owner, std::move(task));
              },
          }} {
        session.changed = [this] { present(); };
        build();
    }

    void build() {
        owner.setFont(studioPanelFont());
        auto *layout = new QVBoxLayout(&owner);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(4);

        reload_action = new QAction(owner.tr("Reload when saved outside"), &owner);
        reload_action->setObjectName(QStringLiteral("pelican.assets.reloadWhenSavedOutside"));
        reload_action->setCheckable(true);
        reload_action->setToolTip(owner.tr(
            "The engine reads this file, or what is in this folder, again when it is saved "
            "outside Studio (project.json's hot_reload)"));
        QObject::connect(reload_action, &QAction::triggered, &owner, [this] { toggleCurrent(); });
        undo_action = new QAction(owner.tr("Undo"), &owner);
        undo_action->setObjectName(QStringLiteral("pelican.assets.undo"));
        undo_action->setShortcut(QKeySequence::Undo);
        undo_action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        owner.addAction(undo_action);
        QObject::connect(undo_action, &QAction::triggered, &owner, [this] { session.undo(); });

        auto *top = new QHBoxLayout;
        top->setSpacing(4);
        watch_line = new QLabel(&owner);
        watch_line->setObjectName(QStringLiteral("pelican.assets.watchLine"));
        applyPanelNoticeStyle(watch_line);
        watch_line->hide();
        undo_button = new QToolButton(&owner);
        undo_button->setObjectName(QStringLiteral("pelican.assets.undoButton"));
        undo_button->setDefaultAction(undo_action);
        undo_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        top->addWidget(watch_line, 1);
        top->addStretch();
        top->addWidget(undo_button);
        layout->addLayout(top);

        tree = new QTreeWidget(&owner);
        tree->setObjectName(QStringLiteral("pelican.assetsTree"));
        tree->setColumnCount(3);
        tree->setHeaderLabels({owner.tr("Asset"), owner.tr("Reload"), owner.tr("Contents")});
        tree->headerItem()->setToolTip(
            ReloadColumn, owner.tr("Reload when saved outside: ticked, the engine reads the file "
                                   "again when an editor of your own saves it"));
        tree->headerItem()->setToolTip(
            ContentsColumn, owner.tr("What the check against the asset manifest found"));
        tree->header()->setStretchLastSection(true);
        tree->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
        tree->header()->setSectionResizeMode(ReloadColumn, QHeaderView::ResizeToContents);
        tree->header()->setStretchLastSection(false);
        tree->header()->setSectionResizeMode(ContentsColumn, QHeaderView::ResizeToContents);
        tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tree->setContextMenuPolicy(Qt::CustomContextMenu);
        tree->addAction(reload_action);
        QObject::connect(tree, &QTreeWidget::itemChanged, &owner,
                         [this](QTreeWidgetItem *item, int column) { checked(item, column); });
        QObject::connect(tree, &QTreeWidget::currentItemChanged, &owner,
                         [this](QTreeWidgetItem *, QTreeWidgetItem *) { updateReloadAction(); });
        QObject::connect(tree, &QTreeWidget::customContextMenuRequested, &owner,
                         [this](const QPoint &point) {
                             if (QTreeWidgetItem *item = tree->itemAt(point))
                                 tree->setCurrentItem(item);
                             updateReloadAction();
                             QMenu menu{&owner};
                             menu.addAction(reload_action);
                             menu.addSeparator();
                             menu.addAction(undo_action);
                             menu.exec(tree->viewport()->mapToGlobal(point));
                         });
        layout->addWidget(tree, 1);

        auto *notice_row = new QHBoxLayout;
        notice = new QLabel(&owner);
        notice->setObjectName(QStringLiteral("pelican.assets.notice"));
        applyPanelNoticeStyle(notice);
        notice->hide();
        restart = new QPushButton(owner.tr("Restart engine"), &owner);
        restart->setObjectName(QStringLiteral("pelican.assets.restartEngine"));
        restart->hide();
        QObject::connect(restart, &QPushButton::clicked, &owner, [this] {
            if (hooks.restart) hooks.restart(qtext(session.restartReason()));
        });
        notice_row->addWidget(notice, 1);
        notice_row->addWidget(restart);
        layout->addLayout(notice_row);

        track = new QPushButton(owner.tr("Track asset contents\xE2\x80\xA6"), &owner);
        track->setObjectName(QStringLiteral("pelican.assets.trackContents"));
        track->setToolTip(owner.tr(
            "Declare an asset manifest in project.json (the store main at assets/, written to "
            "assets.manifest.json) and check the files against it. Commit the manifest with the "
            "project; Update manifest accepts a change you meant."));
        track->hide();
        QObject::connect(track, &QPushButton::clicked, &owner, [this] { session.trackContents(); });
        layout->addWidget(track);

        verification = new QFrame(&owner);
        verification->setObjectName(QStringLiteral("pelican.assets.verification"));
        auto *verification_layout = new QVBoxLayout(verification);
        verification_layout->setContentsMargins(4, 4, 4, 4);
        verification_layout->setSpacing(4);
        auto *summary_row = new QHBoxLayout;
        summary = new QLabel(verification);
        summary->setObjectName(QStringLiteral("pelican.assets.verificationSummary"));
        summary->setWordWrap(true);
        summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
        details = new QToolButton(verification);
        details->setObjectName(QStringLiteral("pelican.assets.showIssues"));
        details->setText(owner.tr("Details"));
        details->setCheckable(true);
        details->setToolTip(owner.tr("The files that differ, store by store"));
        verify = new QPushButton(owner.tr("Verify again"), verification);
        verify->setObjectName(QStringLiteral("pelican.assets.verifyAgain"));
        verify->setToolTip(owner.tr(
            "Check every file against the manifest again (verify_assets). A file read again "
            "after a save outside is not checked until then."));
        QObject::connect(verify, &QPushButton::clicked, &owner, [this] { session.verifyAgain(); });
        summary_row->addWidget(summary, 1);
        summary_row->addWidget(details);
        summary_row->addWidget(verify);
        verification_layout->addLayout(summary_row);
        issues = new QTreeWidget(verification);
        issues->setObjectName(QStringLiteral("pelican.assets.issues"));
        issues->setColumnCount(3);
        issues->setHeaderLabels({owner.tr("Store / kind"), owner.tr("File"), owner.tr("Message")});
        issues->setEditTriggers(QAbstractItemView::NoEditTriggers);
        issues->hide();
        QObject::connect(details, &QToolButton::toggled, &owner,
                         [this](bool shown) { issues->setVisible(shown); });
        verification_layout->addWidget(issues);
        store_rows = new QWidget(verification);
        store_rows_layout = new QVBoxLayout(store_rows);
        store_rows_layout->setContentsMargins(0, 0, 0, 0);
        store_rows_layout->setSpacing(2);
        verification_layout->addWidget(store_rows);
        confirm_row = new QWidget(verification);
        auto *confirm_layout = new QHBoxLayout(confirm_row);
        confirm_layout->setContentsMargins(0, 0, 0, 0);
        confirm_label = new QLabel(confirm_row);
        confirm_label->setObjectName(QStringLiteral("pelican.assets.updateConfirm"));
        confirm_label->setWordWrap(true);
        confirm = new QPushButton(owner.tr("Update"), confirm_row);
        confirm->setObjectName(QStringLiteral("pelican.assets.updateConfirmed"));
        cancel = new QPushButton(owner.tr("Cancel"), confirm_row);
        cancel->setObjectName(QStringLiteral("pelican.assets.updateCancel"));
        confirm_layout->addWidget(confirm_label, 1);
        confirm_layout->addWidget(confirm);
        confirm_layout->addWidget(cancel);
        confirm_row->hide();
        QObject::connect(confirm, &QPushButton::clicked, &owner, [this] {
            const auto store = std::exchange(confirm_store, {});
            session.updateManifest(store);
            present();
        });
        QObject::connect(cancel, &QPushButton::clicked, &owner, [this] {
            confirm_store.clear();
            present();
        });
        verification_layout->addWidget(confirm_row);
        verification->hide();
        layout->addWidget(verification);

        watcher = new QFileSystemWatcher(&owner);
        rescan_timer = new QTimer(&owner);
        rescan_timer->setSingleShot(true);
        rescan_timer->setInterval(RescanDebounceMs);
        QObject::connect(rescan_timer, &QTimer::timeout, &owner, [this] { rescan(); });
        QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, &owner,
                         [this](const QString &) { rescan_timer->start(); });
        QObject::connect(watcher, &QFileSystemWatcher::fileChanged, &owner,
                         [this](const QString &) { rescan_timer->start(); });
        present();
    }

    // --- the tree

    std::vector<AssetRoot> currentRoots() const {
        return assetRoots(session.projectRoot(), session.assetStores(), session.statusStores());
    }

    static bool sameRoots(const std::vector<AssetRoot> &left, const std::vector<AssetRoot> &right) {
        return std::ranges::equal(left, right, [](const AssetRoot &a, const AssetRoot &b) {
            return a.logical == b.logical && a.physical == b.physical;
        });
    }

    void addEntries(QTreeWidgetItem *parent, const std::vector<AssetEntry> &children,
                    std::map<std::string, QTreeWidgetItem *> &items) {
        for (const auto &entry : children) {
            auto *item = parent == nullptr ? new QTreeWidgetItem(tree) : new QTreeWidgetItem(parent);
            item->setText(NameColumn, qtext(entry.name));
            item->setData(NameColumn, AssetLogicalPathRole, qtext(entry.logical));
            item->setData(NameColumn, AssetPhysicalKeyRole, qtext(assetPathKey(entry.physical)));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            item->setCheckState(ReloadColumn, Qt::Unchecked);
            folders[entry.logical] = entry.directory;
            items[entry.logical] = item;
            if (entry.directory) addEntries(item, entry.children, items);
        }
    }

    void rescan() {
        rescan_timer->stop();
        roots = currentRoots();
        auto next = scanAssetRoots(roots);
        if (scanned) {
            auto changed = changedAssetFiles(entries, next);
            if (!changed.empty()) session.filesChanged(changed);
        }
        entries = std::move(next);
        scanned = !session.projectRoot().empty();
        // What was open stays open, and the row in hand stays in hand.
        QSet<QString> open;
        bool first = tree->topLevelItemCount() == 0;
        for (QTreeWidgetItemIterator it{tree}; *it; ++it)
            if ((*it)->isExpanded()) open.insert((*it)->data(NameColumn, AssetLogicalPathRole).toString());
        QString current;
        if (QTreeWidgetItem *item = tree->currentItem())
            current = item->data(NameColumn, AssetLogicalPathRole).toString();
        updating_tree = true;
        {
            const QSignalBlocker block{tree};
            tree->clear();
            folders.clear();
            std::map<std::string, QTreeWidgetItem *> items;
            addEntries(nullptr, entries, items);
            addMissingRows(items);
            placed_missing = missingKeys();
            for (QTreeWidgetItemIterator it{tree}; *it; ++it) {
                const QString logical = (*it)->data(NameColumn, AssetLogicalPathRole).toString();
                if (open.contains(logical) || (first && (*it)->parent() == nullptr))
                    (*it)->setExpanded(true);
                if (!current.isEmpty() && logical == current) tree->setCurrentItem(*it);
            }
        }
        updating_tree = false;
        rewatch();
        applyMarks();
        updateReloadAction();
    }

    // AV-1s: a file the manifest lists and the disk does not has no row of
    // its own; it gets a grey one where it would be.
    void addMissingRows(std::map<std::string, QTreeWidgetItem *> &items) {
        for (const auto &store : session.verificationDetail())
            for (const auto &issue : store.issues) {
                if (issue.kind != "missing" || issue.physical.empty()) continue;
                const auto key = assetPathKey(issue.physical);
                const AssetRoot *root = nullptr;
                std::string relative;
                for (const auto &candidate : roots) {
                    const auto prefix = assetPathKey(candidate.physical) + "/";
                    if (key.starts_with(prefix)) {
                        root = &candidate;
                        relative = key.substr(prefix.size());
                        break;
                    }
                }
                if (root == nullptr) continue;
                // The spelling the manifest has, for the row's name.
                auto spelled = issue.file;
                std::ranges::replace(spelled, '\\', '/');
                const auto logical = root->logical + "/" + spelled;
                if (items.contains(logical)) continue;
                const auto slash = logical.rfind('/');
                QTreeWidgetItem *parent = nullptr;
                for (auto folder = logical.substr(0, slash); !folder.empty();) {
                    if (const auto found = items.find(folder); found != items.end()) {
                        parent = found->second;
                        break;
                    }
                    const auto up = folder.rfind('/');
                    if (up == std::string::npos) break;
                    folder.erase(up);
                }
                if (parent == nullptr) continue;
                auto *item = new QTreeWidgetItem(parent);
                item->setText(NameColumn, qtext(logical.substr(slash + 1)));
                item->setData(NameColumn, AssetLogicalPathRole, qtext(logical));
                item->setData(NameColumn, AssetPhysicalKeyRole, qtext(key));
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                QFont font = item->font(NameColumn);
                font.setItalic(true);
                item->setFont(NameColumn, font);
                item->setForeground(NameColumn, owner.palette().brush(QPalette::Disabled, QPalette::Text));
                items[logical] = item;
                (void)relative;
            }
    }

    // The folders, and the files the watch covers (an editor's save is a
    // write to the file, which its folder does not always report).
    void rewatch() {
        QStringList directories;
        QStringList files;
        const auto watch = session.shownWatch();
        const std::function<void(const std::vector<AssetEntry> &)> walk =
            [&](const std::vector<AssetEntry> &children) {
                for (const auto &entry : children) {
                    if (entry.directory) {
                        directories.push_back(nativePath(entry.physical));
                        walk(entry.children);
                    } else if (files.size() < WatchedFileLimit &&
                               assetWatchState(entry.logical, watch).mark != AssetWatchMark::None) {
                        files.push_back(nativePath(entry.physical));
                    }
                }
            };
        walk(entries);
        const QStringList wanted = directories + files;
        const QStringList held = watcher->directories() + watcher->files();
        QStringList gone;
        for (const QString &path : held)
            if (!wanted.contains(path)) gone.push_back(path);
        if (!gone.isEmpty()) watcher->removePaths(gone);
        QStringList added;
        for (const QString &path : wanted)
            if (!held.contains(path)) added.push_back(path);
        if (!added.isEmpty()) watcher->addPaths(added);
    }

    void applyMarks() {
        const auto watch = session.shownWatch();
        std::map<std::string, std::vector<const AssetIssue *>> by_file;
        for (const auto &store : session.verificationDetail())
            for (const auto &issue : store.issues)
                if (!issue.physical.empty()) by_file[assetPathKey(issue.physical)].push_back(&issue);
        std::set<std::string> documents;
        if (hooks.render_documents)
            for (const auto &path : hooks.render_documents()) documents.insert(assetPathKey(path));
        const QBrush dim = owner.palette().brush(QPalette::Disabled, QPalette::Text);
        updating_tree = true;
        {
            const QSignalBlocker block{tree};
            for (QTreeWidgetItemIterator it{tree}; *it; ++it) {
                QTreeWidgetItem *item = *it;
                const auto logical = item->data(NameColumn, AssetLogicalPathRole).toString().toStdString();
                const auto key = item->data(NameColumn, AssetPhysicalKeyRole).toString().toStdString();
                if (item->flags().testFlag(Qt::ItemIsUserCheckable)) {
                    const auto state = assetWatchState(logical, watch);
                    switch (state.mark) {
                    case AssetWatchMark::Direct:
                        item->setCheckState(ReloadColumn, Qt::Checked);
                        item->setText(ReloadColumn, {});
                        item->setToolTip(ReloadColumn,
                                         owner.tr("Read again when it is saved outside Studio. "
                                                  "Untick to stop."));
                        break;
                    case AssetWatchMark::Inherited:
                        item->setCheckState(ReloadColumn, Qt::PartiallyChecked);
                        item->setText(ReloadColumn, owner.tr("via %1").arg(shownPath(state.through)));
                        item->setForeground(ReloadColumn, dim);
                        item->setToolTip(ReloadColumn,
                                         owner.tr("Read again when it is saved outside Studio, "
                                                  "because %1 is watched")
                                             .arg(qtext(state.through)));
                        break;
                    case AssetWatchMark::None:
                        item->setCheckState(ReloadColumn, Qt::Unchecked);
                        item->setText(ReloadColumn, {});
                        item->setToolTip(ReloadColumn,
                                         owner.tr("Tick to have the engine read it again when it "
                                                  "is saved outside Studio"));
                        break;
                    }
                }
                const auto found = by_file.find(key);
                if (found == by_file.end()) {
                    item->setText(ContentsColumn, {});
                    item->setToolTip(ContentsColumn, {});
                } else {
                    QString label = qtext(assetIssueKindLabel(found->second.front()->kind));
                    if (found->second.size() > 1)
                        label += QStringLiteral(" +%1").arg(found->second.size() - 1);
                    QStringList messages;
                    for (const auto *issue : found->second) messages.push_back(qtext(issue->message));
                    item->setText(ContentsColumn, label);
                    item->setToolTip(ContentsColumn, messages.join(QLatin1Char('\n')));
                }
                // A file the check newly marked is opened to, once.
                if (found != by_file.end() && !revealed.contains(key))
                    for (QTreeWidgetItem *parent = item->parent(); parent != nullptr;
                         parent = parent->parent())
                        parent->setExpanded(true);
                // RL-1: a render document is taken in by Reload from disk,
                // not by this watch.
                if (documents.contains(key))
                    item->setToolTip(NameColumn,
                                     owner.tr("A render document: a save outside Studio is taken "
                                              "in by Reload from disk (the Graph menu), not by "
                                              "Reload when saved outside"));
                else
                    item->setToolTip(NameColumn, shownPath(logical));
            }
        }
        updating_tree = false;
        revealed.clear();
        for (const auto &[key, issues_of_file] : by_file) revealed.insert(key);
    }

    // --- the switches

    void updateReloadAction() {
        QTreeWidgetItem *item = tree->currentItem();
        const bool checkable = item != nullptr && item->flags().testFlag(Qt::ItemIsUserCheckable);
        const auto state = checkable ? assetWatchState(item->data(NameColumn, AssetLogicalPathRole)
                                                           .toString()
                                                           .toStdString(),
                                                       session.shownWatch())
                                     : AssetWatchState{};
        const QSignalBlocker block{reload_action};
        reload_action->setChecked(state.mark != AssetWatchMark::None);
        reload_action->setEnabled(checkable && session.ready() && !session.editPending());
    }

    void setWatched(QTreeWidgetItem *item, bool on) {
        if (item == nullptr || !item->flags().testFlag(Qt::ItemIsUserCheckable)) return;
        const auto logical = item->data(NameColumn, AssetLogicalPathRole).toString().toStdString();
        const auto state = assetWatchState(logical, session.shownWatch());
        if (state.mark == AssetWatchMark::Inherited) {
            session.say(owner.tr("%1 is watched because %2 is; untick %2 to stop.")
                            .arg(qtext(logical), qtext(state.through))
                            .toStdString());
            return;
        }
        if (on == (state.mark == AssetWatchMark::Direct)) return;
        if (!session.ready()) {
            session.say(owner.tr("The engine is not running; the watch is set when it is.")
                            .toStdString());
            return;
        }
        session.setWatched(logical, on);
    }

    void toggleCurrent() {
        QTreeWidgetItem *item = tree->currentItem();
        if (item == nullptr) return;
        const auto logical = item->data(NameColumn, AssetLogicalPathRole).toString().toStdString();
        const auto state = assetWatchState(logical, session.shownWatch());
        setWatched(item, state.mark == AssetWatchMark::None);
        present();
    }

    void checked(QTreeWidgetItem *item, int column) {
        if (updating_tree || column != ReloadColumn || item == nullptr) return;
        setWatched(item, item->checkState(ReloadColumn) == Qt::Checked);
        // The box shows what was asked for, or (refused) what was.
        present();
    }

    // --- everything else

    void presentWatchLine() {
        const auto watch = session.shownWatch();
        const auto line = assetsWatchLine(
            watch,
            [this](const std::string &logical) {
                if (const auto found = folders.find(logical); found != folders.end())
                    return !found->second;
                std::error_code error;
                return std::filesystem::is_regular_file(
                    session.projectRoot() / std::filesystem::path{std::u8string{
                                                reinterpret_cast<const char8_t *>(logical.data()),
                                                logical.size()}},
                    error);
            },
            session.lastReload());
        QStringList listed;
        for (const auto &path : watch) listed.push_back(qtext(path));
        QString tip = owner.tr("Saved outside Studio, these are read again by the engine "
                               "(project.json's hot_reload): %1")
                          .arg(listed.join(QStringLiteral(", ")));
        if (session.watchSwitchedOn())
            tip += QLatin1Char('\n') + owner.tr("Changes saved before now are not reloaded.");
        setPanelNotice(watch_line, qtext(line), tip);
    }

    void presentStores() {
        const auto &detail = session.verificationDetail();
        std::set<std::string> wanted;
        for (const auto &store : detail)
            if (!store.manifest.empty()) wanted.insert(store.name);
        std::set<std::string> held;
        for (const auto &[name, button] : store_buttons) held.insert(name);
        if (wanted != held) {
            while (QLayoutItem *item = store_rows_layout->takeAt(0)) {
                if (QWidget *widget = item->widget()) widget->deleteLater();
                delete item;
            }
            store_buttons.clear();
            for (const auto &name : wanted) {
                auto *row = new QWidget(store_rows);
                auto *row_layout = new QHBoxLayout(row);
                row_layout->setContentsMargins(0, 0, 0, 0);
                auto *label = new QLabel(row);
                label->setObjectName(QStringLiteral("pelican.assets.storeLine.") + qtext(name));
                auto *button = new QPushButton(owner.tr("Update manifest"), row);
                button->setObjectName(QStringLiteral("pelican.assets.updateManifest.") + qtext(name));
                button->setToolTip(owner.tr(
                    "Write the manifest again from the files as they are now "
                    "(update_assets_manifest): what differs is accepted"));
                QObject::connect(button, &QPushButton::clicked, &owner, [this, name] {
                    confirm_store = name;
                    present();
                });
                row_layout->addWidget(label, 1);
                row_layout->addWidget(button);
                store_rows_layout->addWidget(row);
                store_buttons[name] = button;
            }
        }
        const bool idle = session.ready() && !session.verificationRunning();
        for (const auto &store : detail) {
            const auto found = store_buttons.find(store.name);
            if (found == store_buttons.end()) continue;
            found->second->setEnabled(idle);
            if (auto *label = found->second->parentWidget()->findChild<QLabel *>())
                label->setText(owner.tr("%1 \xE2\x80\x94 %2")
                                   .arg(qtext(store.name),
                                        nativePath(store.manifest.filename())));
        }
        // Asked before the manifest is written: how many files it changes.
        const auto confirming = std::ranges::find(detail, confirm_store, &AssetStoreVerification::name);
        if (confirm_store.empty() || confirming == detail.end() || !idle) {
            confirm_store.clear();
            confirm_row->hide();
        } else {
            confirm_label->setText(qtext(manifestUpdateLine(*confirming)));
            confirm_row->show();
        }
    }

    void presentIssues() {
        issues->clear();
        for (const auto &store : session.verificationDetail()) {
            if (store.issues.empty()) continue;
            auto *top = new QTreeWidgetItem(issues);
            top->setText(0, qtext(store.name));
            top->setText(1, nativePath(store.manifest.filename()));
            for (const auto &issue : store.issues) {
                auto *row = new QTreeWidgetItem(top);
                row->setText(0, qtext(assetIssueKindLabel(issue.kind)));
                row->setText(1, qtext(issue.file));
                row->setText(2, qtext(issue.message));
                row->setToolTip(2, qtext(issue.message));
            }
            top->setExpanded(true);
        }
    }

    // The missing files the tree last placed rows for.
    std::set<std::string> missingKeys() const {
        std::set<std::string> missing;
        for (const auto &store : session.verificationDetail())
            for (const auto &issue : store.issues)
                if (issue.kind == "missing" && !issue.physical.empty())
                    missing.insert(assetPathKey(issue.physical));
        return missing;
    }

    void present() {
        presentWatchLine();
        // Undo names the edit it takes back.
        const auto &undo = session.undoEntries();
        undo_action->setEnabled(!undo.empty() && session.ready() && !session.editPending());
        undo_action->setText(undo.empty() ? owner.tr("Undo")
                                          : owner.tr("Undo %1").arg(qtext(undo.back().description)));
        undo_action->setToolTip(undo_action->text());
        QString line = qtext(session.notice());
        QString detail = qtext(session.noticeDetail());
        if (line.isEmpty() && !session.restartReason().empty()) {
            line = owner.tr("Restart the engine to apply: %1").arg(qtext(session.restartReason()));
            detail = qtext(session.restartReason());
        }
        setPanelNotice(notice, oneLine(line, 140), detail.isEmpty() ? line : detail);
        restart->setVisible(!session.restartReason().empty() && static_cast<bool>(hooks.restart));
        track->setVisible(session.trackOffered());
        track->setEnabled(!session.editPending());

        const auto &summary_state = session.verification();
        using State = AssetsVerificationSummary::State;
        const bool shown = summary_state.state == State::Running || summary_state.state == State::Verified;
        verification->setVisible(shown);
        const bool differs = summary_state.state == State::Verified && summary_state.issues() > 0;
        verification->setFrameShape(differs ? QFrame::StyledPanel : QFrame::NoFrame);
        QFont summary_font = summary->font();
        summary_font.setBold(differs);
        summary->setFont(summary_font);
        summary->setText(qtext(assetsVerificationLine(summary_state, session.verificationDetail())));
        verify->setEnabled(session.ready() && !session.verificationRunning());
        details->setVisible(differs);
        if (!differs && details->isChecked()) details->setChecked(false);
        presentIssues();
        presentStores();
        if (hooks.checked) hooks.checked(session.verificationDetail());
        if (scanned) {
            // A new store's mount, or the rows of missing files, change the
            // rows themselves; anything else only their marks.
            if (!sameRoots(roots, currentRoots()) || missingKeys() != placed_missing) {
                rescan();
                return;
            }
            rewatch();
            applyMarks();
        }
        updateReloadAction();
    }
};

AssetsDock::AssetsDock(Hooks hooks, QWidget *parent) : QWidget(parent) {
    impl_ = std::make_unique<Impl>(*this, std::move(hooks));
}

AssetsDock::~AssetsDock() {
    // The session's last word, and the signals the children send as they
    // go, must not reach a half-destroyed panel.
    impl_->session.changed = nullptr;
    for (QObject *child : findChildren<QObject *>()) QObject::disconnect(child, nullptr, this, nullptr);
}

void AssetsDock::open(const std::filesystem::path &project_root) {
    impl_->confirm_store.clear();
    impl_->revealed.clear();
    impl_->scanned = false;
    impl_->entries.clear();
    impl_->roots.clear();
    {
        const QSignalBlocker block{impl_->tree};
        impl_->tree->clear();
    }
    impl_->session.open(project_root);
    if (!project_root.empty()) impl_->rescan();
    else impl_->rewatch();
}

void AssetsDock::engineAvailable() { impl_->session.engineReady(); }

void AssetsDock::engineUnavailable() { impl_->session.engineGone(); }

void AssetsDock::observeStatus(qint64 request_id, const nlohmann::json &status) {
    impl_->session.observeStatus(request_id, status);
}

bool AssetsDock::receiveResult(qint64 request_id, const QByteArray &result_json) {
    if (!impl_->session.owns(request_id)) return false;
    const auto result = Json::parse(result_json.constData(),
                                    result_json.constData() + result_json.size(), nullptr, false);
    return impl_->session.receiveResult(request_id, result.is_discarded() ? Json{} : result);
}

bool AssetsDock::receiveRefusal(qint64 request_id, int code, const QString &message,
                                const QJsonValue &data) {
    (void)code;
    if (!impl_->session.owns(request_id)) return false;
    const QJsonObject object = data.toObject();
    const auto fields = Json::parse(
        QJsonDocument(object.value(QStringLiteral("error_fields")).toObject())
            .toJson(QJsonDocument::Compact)
            .toStdString(),
        nullptr, false);
    return impl_->session.receiveRefusal(
        request_id, object.value(QStringLiteral("error_code")).toString().toStdString(),
        message.toStdString(), fields.is_discarded() ? Json::object() : fields);
}

bool AssetsDock::receiveFailure(qint64 request_id, const QString &message) {
    return impl_->session.receiveFailure(request_id, message.toStdString());
}

void AssetsDock::rescan() { impl_->rescan(); }

bool AssetsDock::revealFile(const std::filesystem::path &path) {
    const auto key = qtext(assetPathKey(path));
    for (QTreeWidgetItemIterator it{impl_->tree}; *it; ++it) {
        QTreeWidgetItem *item = *it;
        if (item->data(NameColumn, AssetPhysicalKeyRole).toString() != key) continue;
        for (QTreeWidgetItem *parent = item->parent(); parent != nullptr; parent = parent->parent())
            parent->setExpanded(true);
        impl_->tree->setCurrentItem(item);
        impl_->tree->scrollToItem(item);
        return true;
    }
    return false;
}

AssetsDockSession &AssetsDock::session() noexcept { return impl_->session; }
QTreeWidget *AssetsDock::tree() const noexcept { return impl_->tree; }
QAction *AssetsDock::reloadAction() const noexcept { return impl_->reload_action; }
QAction *AssetsDock::undoAction() const noexcept { return impl_->undo_action; }
QLabel *AssetsDock::watchLine() const noexcept { return impl_->watch_line; }
QLabel *AssetsDock::noticeLine() const noexcept { return impl_->notice; }
QLabel *AssetsDock::verificationLine() const noexcept { return impl_->summary; }
QPushButton *AssetsDock::trackButton() const noexcept { return impl_->track; }
QPushButton *AssetsDock::verifyButton() const noexcept { return impl_->verify; }

} // namespace PelicanStudio
