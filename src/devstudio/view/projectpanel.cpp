#include "projectpanel.hpp"

#include "panelhelp.hpp"
#include "panelshown.hpp"

#include <QAction>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <set>
#include <unordered_map>

namespace PelicanStudio {
namespace {

constexpr int pathRole = Qt::UserRole + 1;
constexpr int kindRole = Qt::UserRole + 2;

std::string fromQString(const QString &text) {
    return text.toUtf8().toStdString();
}

QString utf8(const std::string &text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string utf8(const std::filesystem::path &path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

std::filesystem::path pathFromUtf8(const std::string &value) {
    const auto *first = reinterpret_cast<const char8_t *>(value.data());
    return std::filesystem::path{std::u8string{first, first + value.size()}};
}

std::uint64_t microseconds(const QElapsedTimer &timer) {
    return static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000);
}

} // namespace

struct ProjectPanel::Impl {
    struct Row {
        QTreeWidgetItem *item = nullptr;
        bool hidden = false;
    };
    struct Anchor {
        std::string path;
        int top = 0;
        bool valid = false;
    };

    explicit Impl(ProjectPanel &widget)
        : owner{widget}, pool{std::make_unique<QThreadPool>()},
          shown{owner, [this] { becameShown(); }, [this] { becameHidden(); }} {
        pool->setMaxThreadCount(1);
        auto *layout = new QVBoxLayout(&owner);
        layout->setContentsMargins(0, 0, 0, 0);
        search = new QLineEdit(&owner);
        search->setObjectName(QStringLiteral("pelican.project.search"));
        search->setPlaceholderText(owner.tr("Filter by path"));
        search->setClearButtonEnabled(true);
        layout->addWidget(search);

        auto *filter_row = new QHBoxLayout;
        kinds = new QComboBox(&owner);
        kinds->setObjectName(QStringLiteral("pelican.project.kind"));
        kinds->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        kinds->setMinimumContentsLength(6);
        kinds->addItem(owner.tr("All kinds"));
        for (int i = 0; i <= static_cast<int>(ProjectFileKind::Other); ++i)
            kinds->addItem(utf8(projectFileKindName(static_cast<ProjectFileKind>(i))));
        refresh_button = new QPushButton(owner.tr("Refresh"), &owner);
        refresh_button->setObjectName(QStringLiteral("pelican.project.refresh"));
        filter_row->addWidget(kinds, 1);
        filter_row->addWidget(refresh_button);
        layout->addLayout(filter_row);

        tree_widget = new QTreeWidget(&owner);
        tree_widget->setObjectName(QStringLiteral("pelican.projectTree"));
        tree_widget->setColumnCount(2);
        tree_widget->setHeaderLabels({owner.tr("Name"), owner.tr("Kind")});
        tree_widget->setUniformRowHeights(true);
        tree_widget->setAnimated(false);
        tree_widget->setSortingEnabled(false);
        tree_widget->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tree_widget->setDragEnabled(false);
        tree_widget->setSelectionMode(QAbstractItemView::SingleSelection);
        tree_widget->setTextElideMode(Qt::ElideMiddle);
        tree_widget->header()->setStretchLastSection(false);
        tree_widget->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        tree_widget->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        tree_widget->header()->setResizeContentsPrecision(100);
        tree_widget->setContextMenuPolicy(Qt::CustomContextMenu);
        copy_path = new QAction(owner.tr("Copy Path"), &owner);
        copy_path->setObjectName(QStringLiteral("pelican.project.copyPath"));
        copy_path->setEnabled(false);
        tree_widget->addAction(copy_path);
        layout->addWidget(tree_widget, 1);

        truncated_line = new QLabel(&owner);
        truncated_line->setObjectName(QStringLiteral("pelican.project.truncated"));
        applyPanelNoticeStyle(truncated_line);
        truncated_line->setWordWrap(true);
        truncated_line->hide();
        layout->addWidget(truncated_line);
        notice_line = new QLabel(&owner);
        notice_line->setObjectName(QStringLiteral("pelican.project.notice"));
        applyPanelNoticeStyle(notice_line);
        notice_line->setWordWrap(true);
        notice_line->hide();
        layout->addWidget(notice_line);

        QObject::connect(refresh_button, &QPushButton::clicked, &owner, [this] { refresh(); });
        QObject::connect(search, &QLineEdit::textChanged, &owner, [this] { filterChanged(); });
        QObject::connect(kinds, &QComboBox::currentIndexChanged, &owner, [this] { filterChanged(); });
        QObject::connect(tree_widget, &QTreeWidget::currentItemChanged, &owner,
                         [this] { updateCopyAction(); });
        QObject::connect(copy_path, &QAction::triggered, &owner, [this] { copyPath(); });
        QObject::connect(tree_widget, &QTreeWidget::customContextMenuRequested, &owner,
                         [this](const QPoint &point) {
                             if (auto *item = tree_widget->itemAt(point)) tree_widget->setCurrentItem(item);
                             updateCopyAction();
                             QMenu menu{&owner};
                             menu.addAction(copy_path);
                             menu.exec(tree_widget->viewport()->mapToGlobal(point));
                         });
        owner.setEnabled(false);
    }

    ~Impl() {
        {
            const std::lock_guard lock{dispatch->mutex};
            dispatch->owner = nullptr;
        }
        if (cancel) cancel->store(true);
        if (!pool->waitForDone(2000)) {
            qWarning() << "Project folder scan is still blocked in filesystem I/O; "
                          "its thread pool is intentionally left allocated";
            (void)pool.release();
        }
    }

    struct Dispatch {
        std::mutex mutex;
        QObject *owner = nullptr;
    };

    ProjectPanel &owner;
    std::shared_ptr<Dispatch> dispatch = std::make_shared<Dispatch>();
    ProjectScanGate gate;
    std::filesystem::path root;
    bool stale = false;
    bool scan_running = false;
    bool rescan_wanted = false;
    std::shared_ptr<std::atomic_bool> cancel{std::make_shared<std::atomic_bool>(false)};
    std::unique_ptr<QThreadPool> pool;
    std::shared_ptr<const ProjectListing> listing{std::make_shared<ProjectListing>()};
    std::shared_ptr<const ProjectListing> pending;
    std::deque<ProjectTreeEdit> queue;
    std::unordered_map<std::string, Row> rows;
    ProjectFilter filter;
    bool filter_was_active = false;
    bool filter_dirty = false;
    std::uint64_t filter_revision = 0;
    std::uint64_t prepared_filter_revision = 0;
    std::set<std::string> saved_open;
    int apply_budget_ms = 8;
    ScanFunction scan;
    ProjectPanel::Counters stats;
    QElapsedTimer scan_clock;
    QElapsedTimer apply_clock;
    QElapsedTimer filter_clock;
    std::uint64_t tree_epoch = 0;
    bool drain_scheduled = false;
    bool pending_had_edits = false;
    std::deque<int> visibility_queue;
    std::vector<char> pending_visible;
    bool expanding_filter = false;
    bool restoring_expansion = false;
    bool any_visible = false;
    QLineEdit *search = nullptr;
    QComboBox *kinds = nullptr;
    QPushButton *refresh_button = nullptr;
    QTreeWidget *tree_widget = nullptr;
    QLabel *truncated_line = nullptr;
    QLabel *notice_line = nullptr;
    QAction *copy_path = nullptr;
    PanelShown shown;

    void open(const std::filesystem::path &next_root) {
        gate.openProject();
        ++tree_epoch;
        if (cancel) cancel->store(true);
        queue.clear();
        visibility_queue.clear();
        pending_visible.clear();
        pending.reset();
        tree_widget->clear();
        rows.clear();
        listing = std::make_shared<ProjectListing>();
        scan_running = false;
        rescan_wanted = false;
        drain_scheduled = false;
        pending_had_edits = false;
        root = next_root;
        {
            const QSignalBlocker block_search{search};
            const QSignalBlocker block_kinds{kinds};
            search->clear();
            kinds->setCurrentIndex(0);
        }
        filter = {};
        filter_dirty = false;
        ++filter_revision;
        prepared_filter_revision = filter_revision;
        filter_was_active = false;
        saved_open.clear();
        any_visible = false;
        truncated_line->hide();
        notice_line->hide();
        owner.setEnabled(!root.empty());
        tree_widget->setEnabled(!root.empty());
        updateCopyAction();
        if (root.empty()) {
            stale = false;
            return;
        }
        stale = true;
        if (shown.isShown()) startScan();
    }

    void refresh() {
        if (root.empty()) return;
        if (!shown.isShown()) {
            stale = true;
            return;
        }
        if (!queue.empty() || !visibility_queue.empty() || drain_scheduled || pending) {
            rescan_wanted = true;
            return;
        }
        startScan();
    }

    void becameShown() {
        if (root.empty() || !stale) return;
        if (!queue.empty() || !visibility_queue.empty() || drain_scheduled || pending) {
            rescan_wanted = true;
            return;
        }
        startScan();
    }

    void becameHidden() {
        if (scan_running) {
            cancel->store(true);
            gate.invalidate();
            scan_running = false;
        }
        stale = true;
    }

    void startScan() {
        if (root.empty() || !queue.empty() || !visibility_queue.empty() || pending) return;
        if (cancel) cancel->store(true);
        const auto tag = gate.beginScan();
        ++stats.scans_started;
        stale = false;
        scan_running = true;
        cancel = std::make_shared<std::atomic_bool>(false);
        auto worker_cancel = cancel;
        const auto scan_root = root;
        const auto base = listing;
        const auto scan_function = scan;
        ProjectScanOptions options;
        options.should_stop = [worker_cancel] { return worker_cancel->load(); };
        if (rows.empty()) {
            notice_line->setText(owner.tr("Reading the project folder..."));
            notice_line->show();
        }
        scan_clock.start();
        const auto worker_dispatch = dispatch;
        worker_dispatch->owner = &owner;
        pool->start([this, tag, scan_root, base, options, scan_function, worker_cancel,
                     worker_dispatch] {
            QElapsedTimer timer;
            timer.start();
            auto next = std::make_shared<ProjectListing>();
            try {
                *next = scan_function ? scan_function(scan_root, options)
                                      : scanProjectFolder(scan_root, options);
            } catch (...) {
                next->root_unreadable = true;
            }
            const auto scan_us = static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000);
            timer.restart();
            auto edits = std::make_shared<std::vector<ProjectTreeEdit>>(
                diffProjectListings(*base, *next));
            const auto diff_us = static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000);
            const std::lock_guard lock{worker_dispatch->mutex};
            if (worker_dispatch->owner == nullptr) return;
            QMetaObject::invokeMethod(
                worker_dispatch->owner,
                [this, tag, next, edits, scan_us, diff_us] {
                    deliver(tag, std::move(next), std::move(edits), scan_us, diff_us);
                },
                Qt::QueuedConnection);
        });
    }

    void deliver(ProjectScanTag tag, std::shared_ptr<const ProjectListing> next,
                 std::shared_ptr<std::vector<ProjectTreeEdit>> edits,
                 std::uint64_t scan_us, std::uint64_t diff_us) {
        if (!gate.isCurrent(tag)) {
            ++stats.results_discarded;
            return;
        }
        scan_running = false;
        stats.last_scan_us = scan_us;
        stats.last_diff_us = diff_us;
        if (next->cancelled) {
            stale = true;
            updateNotice();
            return;
        }
        ++stats.scans_completed;
        pending = std::move(next);
        pending_had_edits = !edits->empty();
        stats.last_apply_us = 0;
        stats.last_filter_us = 0;
        apply_clock.start();
        queue.clear();
        for (auto &edit : *edits) queue.push_back(std::move(edit));
        updateTreeEnabled();
        if (queue.empty()) {
            commitPending();
            finishWork();
        } else {
            drain();
        }
    }

    void commitPending() {
        if (!pending) return;
        listing = std::move(pending);
        stats.last_apply_us = microseconds(apply_clock);
        stats.visited_entries = listing->examined_entries;
        stats.json_bytes = listing->json_bytes_inspected;
        if (pending_had_edits) filter_dirty = true;
        pending_had_edits = false;
        updateLabels();
    }

    void updateTreeEnabled() {
        tree_widget->setEnabled(!root.empty() && queue.empty() && visibility_queue.empty() &&
                                !drain_scheduled && !pending);
    }

    Anchor anchor() const {
        Anchor result;
        if (const auto *item = tree_widget->itemAt(QPoint{2, 2})) {
            result.path = fromQString(item->data(0, pathRole).toString());
            result.top = tree_widget->visualItemRect(item).top();
            result.valid = true;
        }
        return result;
    }

    void restoreAnchor(const Anchor &saved) {
        if (!saved.valid) return;
        const auto found = rows.find(saved.path);
        if (found == rows.end() || found->second.hidden) return;
        tree_widget->scrollToItem(found->second.item, QAbstractItemView::PositionAtTop);
        tree_widget->verticalScrollBar()->setValue(
            tree_widget->verticalScrollBar()->value() - saved.top);
    }

    void drain() {
        drain_scheduled = false;
        ++stats.apply_turns;
        QElapsedTimer turn;
        turn.start();
        bool did_work = false;
        while (true) {
            if (!queue.empty()) {
                const auto edit = std::move(queue.front());
                queue.pop_front();
                applyEdit(edit);
                ++stats.row_edits_applied;
                did_work = true;
            } else if (pending) {
                commitPending();
                continue;
            } else if (!visibility_queue.empty()) {
                filter_clock.restart();
                applyVisibility(visibility_queue.front());
                visibility_queue.pop_front();
                stats.last_filter_us += microseconds(filter_clock);
                did_work = true;
            } else if (filter_dirty || prepared_filter_revision != filter_revision) {
                prepareFilter();
                continue;
            } else {
                finishWork();
                return;
            }
            if (!queue.empty() || !visibility_queue.empty() || pending) {
                if (turn.elapsed() >= apply_budget_ms) break;
                if (apply_budget_ms == 0) break;
            }
        }
        if (!did_work) finishWork();
        if (!queue.empty() || !visibility_queue.empty() || pending ||
            prepared_filter_revision != filter_revision) {
            drain_scheduled = true;
            updateTreeEnabled();
            const auto epoch = tree_epoch;
            QTimer::singleShot(0, &owner, [this, epoch] {
                if (epoch != tree_epoch) return;
                drain();
            });
        } else {
            finishWork();
        }
    }

    QString kindTooltip(const ProjectFileEntry &entry) const {
        if (entry.kind != ProjectFileKind::Prefab) return {};
        switch (entry.registration) {
        case PrefabRegistration::Registered:
            return owner.tr("project.json lists this prefab. This panel doesn't know whether the "
                            "running engine has loaded it.");
        case PrefabRegistration::NotRegistered:
            return owner.tr("project.json doesn't list this prefab.");
        case PrefabRegistration::Unknown:
            return owner.tr("project.json could not be read.");
        case PrefabRegistration::NotPrefab:
            return {};
        }
        return {};
    }

    void updateRow(QTreeWidgetItem *item, const ProjectFileEntry &entry) {
        item->setText(1, utf8(projectFileKindLabel(entry)));
        item->setData(1, kindRole, utf8(projectFileKindLabel(entry)));
        item->setToolTip(1, kindTooltip(entry));
    }

    void eraseRows(QTreeWidgetItem *item) {
        for (int i = 0; i < item->childCount(); ++i) eraseRows(item->child(i));
        rows.erase(fromQString(item->data(0, pathRole).toString()));
    }

    void applyEdit(const ProjectTreeEdit &edit) {
        const auto saved_anchor = anchor();
        if (edit.op == ProjectTreeEdit::Op::Remove) {
            const auto found = rows.find(edit.path);
            if (found == rows.end()) return;
            auto *item = found->second.item;
            if (auto *current = tree_widget->currentItem()) {
                for (auto *candidate = current; candidate; candidate = candidate->parent()) {
                    if (candidate == item) {
                        tree_widget->setCurrentItem(nullptr);
                        tree_widget->clearSelection();
                        break;
                    }
                }
            }
            eraseRows(item);
            delete item;
        } else if (edit.op == ProjectTreeEdit::Op::Insert) {
            auto *item = new QTreeWidgetItem;
            item->setText(0, utf8(edit.entry.name));
            item->setData(0, pathRole, utf8(edit.entry.path));
            item->setToolTip(0, utf8(edit.entry.path));
            updateRow(item, edit.entry);
            QTreeWidgetItem *parent = nullptr;
            if (!edit.parent_path.empty()) {
                const auto found = rows.find(edit.parent_path);
                if (found == rows.end()) {
                    delete item;
                    return;
                }
                parent = found->second.item;
            }
            if (parent)
                parent->insertChild(edit.row, item);
            else
                tree_widget->insertTopLevelItem(edit.row, item);
            rows[edit.path] = {item, false};
        } else {
            const auto found = rows.find(edit.path);
            if (found != rows.end()) updateRow(found->second.item, edit.entry);
        }
        restoreAnchor(saved_anchor);
    }

    void filterChanged() {
        filter.text = fromQString(search->text());
        const int index = kinds->currentIndex();
        if (index > 0)
            filter.kind = static_cast<ProjectFileKind>(index - 1);
        else
            filter.kind.reset();
        ++filter_revision;
        filter_dirty = true;
        if (!queue.empty() || !visibility_queue.empty() || drain_scheduled || pending) return;
        drain();
    }

    void prepareFilter() {
        filter_dirty = false;
        prepared_filter_revision = filter_revision;
        expanding_filter = !filter.text.empty() || filter.kind.has_value();
        restoring_expansion = !expanding_filter && filter_was_active;
        if (expanding_filter && !filter_was_active) {
            saved_open.clear();
            for (const auto &entry : listing->entries) {
                if (!entry.directory) continue;
                const auto found = rows.find(entry.path);
                if (found != rows.end() && found->second.item->isExpanded())
                    saved_open.insert(entry.path);
            }
        }
        pending_visible = projectFilterVisible(*listing, filter);
        any_visible = std::any_of(pending_visible.begin(), pending_visible.end(),
                                  [](char value) { return value != 0; });
        visibility_queue.clear();
        for (std::size_t i = 0; i < listing->entries.size(); ++i)
            visibility_queue.push_back(static_cast<int>(i));
    }

    void applyVisibility(int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= listing->entries.size()) return;
        const auto &entry = listing->entries[static_cast<std::size_t>(index)];
        const auto found = rows.find(entry.path);
        if (found == rows.end()) return;
        auto &row = found->second;
        const bool hidden = pending_visible.empty() ||
                            pending_visible[static_cast<std::size_t>(index)] == 0;
        if (row.hidden != hidden) {
            row.item->setHidden(hidden);
            row.hidden = hidden;
        }
        if (entry.directory && !hidden) {
            if (expanding_filter)
                row.item->setExpanded(true);
            else if (restoring_expansion)
                row.item->setExpanded(saved_open.contains(entry.path));
        }
        updateCopyAction();
    }

    void finishWork() {
        if (pending) {
            drain();
            return;
        }
        updateTreeEnabled();
        filter_was_active = expanding_filter;
        if (restoring_expansion) {
            saved_open.clear();
            restoring_expansion = false;
        }
        updateLabels();
        updateNotice();
        updateCopyAction();
        if (stats.scans_completed != 0) stats.last_idle_us = microseconds(scan_clock);
        if (rescan_wanted && queue.empty() && visibility_queue.empty() && !scan_running) {
            rescan_wanted = false;
            if (shown.isShown())
                startScan();
            else
                stale = true;
        }
        updateTreeEnabled();
    }

    void updateLabels() {
        truncated_line->setVisible(listing->truncated);
        if (listing->examined_limit_reached && listing->entries.size() < listing->max_entries) {
            truncated_line->setText(
                owner.tr("The folder listing is incomplete after examining %1 entries.")
                    .arg(static_cast<qulonglong>(listing->examined_entries)));
        } else {
            truncated_line->setText(
                owner.tr("The folder listing is incomplete after listing %1 entries. The rest of "
                         "the folder is not listed.")
                    .arg(static_cast<qulonglong>(listing->entries.size())));
        }
    }

    void updateNotice() {
        QStringList lines;
        if (listing->root_unreadable) lines << owner.tr("The project folder could not be read.");
        if (listing->declarations_unread)
            lines << owner.tr("project.json could not be read, so prefab registration is unknown.");
        if (listing->json_budget_exhausted)
            lines << owner.tr("Some JSON files were not opened; kinds could not be inspected and "
                              "may fall back to declarations or other.");
        if (!filter.text.empty() || filter.kind)
            if (!listing->entries.empty() && !any_visible)
                lines << owner.tr("No files match this filter.");
        notice_line->setText(lines.join(' '));
        notice_line->setVisible(!lines.isEmpty());
    }

    void updateCopyAction() {
        const auto *item = tree_widget->currentItem();
        copy_path->setEnabled(!root.empty() && item != nullptr && !item->isHidden());
    }

    void copyPath() {
        const auto *item = tree_widget->currentItem();
        if (!item || item->isHidden() || root.empty()) return;
        const auto relative = fromQString(item->data(0, pathRole).toString());
        const auto full = (root / pathFromUtf8(relative)).lexically_normal();
        QGuiApplication::clipboard()->setText(
            QDir::toNativeSeparators(QString::fromStdString(utf8(full))));
    }
};

ProjectPanel::ProjectPanel(QWidget *parent)
    : QWidget{parent}, impl_{std::make_unique<Impl>(*this)} {
    impl_->shown.locateDock();
}

ProjectPanel::~ProjectPanel() = default;

void ProjectPanel::open(const std::filesystem::path &root) { impl_->open(root); }
void ProjectPanel::refresh() { impl_->refresh(); }
QTreeWidget *ProjectPanel::tree() const noexcept { return impl_->tree_widget; }
QLineEdit *ProjectPanel::searchBox() const noexcept { return impl_->search; }
QComboBox *ProjectPanel::kindFilter() const noexcept { return impl_->kinds; }
QPushButton *ProjectPanel::refreshButton() const noexcept { return impl_->refresh_button; }
QLabel *ProjectPanel::truncatedLine() const noexcept { return impl_->truncated_line; }
QLabel *ProjectPanel::noticeLine() const noexcept { return impl_->notice_line; }
QAction *ProjectPanel::copyPathAction() const noexcept { return impl_->copy_path; }
ProjectPanel::Counters ProjectPanel::counters() const noexcept { return impl_->stats; }

bool ProjectPanel::idle() const noexcept {
    return !impl_->scan_running && !impl_->rescan_wanted && !impl_->pending &&
           impl_->queue.empty() && impl_->visibility_queue.empty() &&
           !impl_->drain_scheduled && !impl_->filter_dirty &&
           impl_->prepared_filter_revision == impl_->filter_revision;
}

void ProjectPanel::setScanFunctionForTest(ScanFunction scan) { impl_->scan = std::move(scan); }
void ProjectPanel::setApplyBudgetMsForTest(int milliseconds) {
    impl_->apply_budget_ms = std::max(0, milliseconds);
}

void ProjectPanel::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    impl_->shown.becameShown();
}

} // namespace PelicanStudio
