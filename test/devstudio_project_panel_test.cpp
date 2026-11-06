#include "projectpanel.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QClipboard>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace PelicanStudio;
using namespace std::chrono_literals;

namespace {

constexpr int pathRole = Qt::UserRole + 1;

QApplication &application() {
    static int argc = 1;
    static char name[] = "devstudio_project_panel_test";
    static char *argv[] = {name, nullptr};
    static QApplication app{argc, argv};
    return app;
}

void write(const std::filesystem::path &path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::filesystem::path rootOf(const QTemporaryDir &temporary) {
    return std::filesystem::path{temporary.path().toStdWString()};
}

QTreeWidgetItem *row(QTreeWidget *tree, const std::string &path) {
    QTreeWidgetItemIterator iterator{tree};
    while (*iterator) {
        if ((*iterator)->data(0, pathRole).toString().toUtf8().toStdString() == path)
            return *iterator;
        ++iterator;
    }
    return nullptr;
}

QTreeWidgetItem *requiredRow(QTreeWidget *tree, const std::string &path) {
    auto *result = row(tree, path);
    REQUIRE(result != nullptr);
    return result;
}

std::map<std::string, QTreeWidgetItem *> rows(QTreeWidget *tree) {
    std::map<std::string, QTreeWidgetItem *> result;
    QTreeWidgetItemIterator iterator{tree};
    while (*iterator) {
        result[(*iterator)->data(0, pathRole).toString().toUtf8().toStdString()] = *iterator;
        ++iterator;
    }
    return result;
}

struct Heartbeat {
    qint64 largest_gap = 0;
};

void step(const std::function<void()> &action, const std::function<bool()> &condition,
          Heartbeat *heartbeat = nullptr) {
    QEventLoop loop;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer timer;
    timer.setInterval(1);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        if (heartbeat) heartbeat->largest_gap = std::max(heartbeat->largest_gap, elapsed.restart());
        if (condition()) loop.quit();
    });
    timer.start();
    QTimer::singleShot(120000, &loop, [&] {
        WARN("120 s Studio test watchdog expired");
        loop.quit();
    });
    QTimer::singleShot(0, [&] {
        if (action) action();
    });
    loop.exec();
    INFO("condition wait elapsed ms=" << elapsed.elapsed());
    REQUIRE(condition());
    timer.stop();
}

void settle(ProjectPanel &panel, Heartbeat *heartbeat = nullptr) {
    step({}, [&] { return panel.idle(); }, heartbeat);
}

void showPanel(ProjectPanel &panel) {
    application();
    panel.resize(640, 480);
    panel.show();
    QApplication::processEvents();
}

std::map<std::string, std::pair<std::uintmax_t, std::string>> snapshot(
    const std::filesystem::path &root) {
    std::map<std::string, std::pair<std::uintmax_t, std::string>> result;
    for (const auto &entry : std::filesystem::recursive_directory_iterator{root}) {
        if (!entry.is_regular_file()) continue;
        const auto path = entry.path();
        std::ifstream input{path, std::ios::binary};
        std::string bytes{std::istreambuf_iterator<char>{}, std::istreambuf_iterator<char>{}};
        bytes.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
        result[path.lexically_relative(root).generic_string()] = {entry.file_size(), bytes};
    }
    return result;
}

} // namespace

struct QtFixture {
    QtFixture() { application(); }
};

TEST_CASE_METHOD(QtFixture, "PRJ-1a o filtering works on rows and restores expansion", "[prj1a]") {
    QTemporaryDir temporary;
    const auto root = rootOf(temporary);
    write(root / "keep" / "nested" / "toon.material.json",
          R"({"schema":"pelican.material","version":1})");
    write(root / "other" / "sound.wav", "sound");
    write(root / "other" / "toon.texture.png", "texture");
    std::filesystem::create_directories(root / "empty");
    ProjectPanel panel;
    showPanel(panel);
    panel.open(root);
    settle(panel);
    auto *tree = panel.tree();
    auto *keep = requiredRow(tree, "keep");
    auto *nested = requiredRow(tree, "keep/nested");
    keep->setExpanded(true);
    nested->setExpanded(true);
    auto *material = requiredRow(tree, "keep/nested/toon.material.json");
    tree->setCurrentItem(material);
    keep->setExpanded(true);
    nested->setExpanded(false);
    auto *other = requiredRow(tree, "other");
    other->setExpanded(false);

    panel.searchBox()->setText(QStringLiteral("TOON"));
    panel.kindFilter()->setCurrentText(QStringLiteral("material"));
    settle(panel);
    CHECK(material == tree->currentItem());
    CHECK_FALSE(material->isHidden());
    CHECK(keep->isExpanded());
    CHECK(nested->isExpanded());
    CHECK(other->isHidden());
    CHECK(panel.copyPathAction()->isEnabled());
    panel.searchBox()->setText(QStringLiteral("no-match"));
    settle(panel);
    CHECK(material->isHidden());
    CHECK_FALSE(panel.copyPathAction()->isEnabled());
    CHECK(panel.noticeLine()->text().contains(QStringLiteral("No files match this filter.")));
    panel.searchBox()->clear();
    panel.kindFilter()->setCurrentText(QStringLiteral("All kinds"));
    settle(panel);
    CHECK(keep->isExpanded());
    CHECK_FALSE(nested->isExpanded());
    CHECK_FALSE(other->isExpanded());
    CHECK_FALSE(panel.noticeLine()->text().contains(QStringLiteral("No files match this filter.")));
    CHECK(panel.copyPathAction()->isEnabled());
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a p a no-change refresh preserves rows, selection and scroll", "[prj1a]") {
    QTemporaryDir temporary;
    const auto root = rootOf(temporary);
    for (int i = 0; i < 500; ++i)
        write(root / "folder" / ("file" + std::to_string(1000 + i) + ".txt"), "same");
    ProjectPanel panel;
    showPanel(panel);
    panel.resize(420, 180);
    panel.open(root);
    settle(panel);
    auto *tree = panel.tree();
    auto *folder = requiredRow(tree, "folder");
    folder->setExpanded(true);
    QApplication::processEvents();
    auto *current = requiredRow(tree, "folder/file1400.txt");
    tree->setCurrentItem(current);
    tree->scrollToItem(current, QAbstractItemView::PositionAtCenter);
    QApplication::processEvents();
    const auto before = rows(tree);
    const auto edits = panel.counters().row_edits_applied;
    const auto completed = panel.counters().scans_completed;
    const int scroll = tree->verticalScrollBar()->value();
    panel.refresh();
    settle(panel);
    const auto after = rows(tree);
    REQUIRE(after.size() == before.size());
    for (const auto &[path, item] : before) CHECK(after.at(path) == item);
    CHECK(panel.counters().row_edits_applied == edits);
    CHECK(panel.counters().scans_completed == completed + 1);
    CHECK(tree->currentItem() == current);
    CHECK(folder->isExpanded());
    CHECK(tree->verticalScrollBar()->value() == scroll);
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a q edits preserve the visible anchor and clear removed selection", "[prj1a]") {
    QTemporaryDir temporary;
    const auto root = rootOf(temporary);
    for (int i = 0; i < 100; ++i)
        write(root / ("file" + std::to_string(1000 + i) + ".txt"), "same");
    write(root / "folder" / "selected.txt", "selected");
    ProjectPanel panel;
    showPanel(panel);
    panel.resize(420, 180);
    panel.open(root);
    settle(panel);
    auto *tree = panel.tree();
    auto *current = requiredRow(tree, "file1050.txt");
    tree->setCurrentItem(current);
    tree->scrollToItem(current, QAbstractItemView::PositionAtCenter);
    QApplication::processEvents();
    auto *top = tree->itemAt(QPoint{2, 2});
    REQUIRE(top != nullptr);
    const std::string top_path = top->data(0, pathRole).toString().toUtf8().toStdString();
    const int top_offset = tree->visualItemRect(top).top();
    write(root / "aaa-new.txt", "new");
    std::filesystem::remove(root / "file1099.txt");
    panel.refresh();
    settle(panel);
    CHECK(tree->currentItem() == current);
    auto *new_top = tree->itemAt(QPoint{2, 2});
    REQUIRE(new_top != nullptr);
    CHECK(new_top->data(0, pathRole).toString().toUtf8().toStdString() == top_path);
    CHECK(std::abs(tree->visualItemRect(new_top).top() - top_offset) <= 1);

    auto *folder = requiredRow(tree, "folder");
    folder->setExpanded(true);
    auto *selected = requiredRow(tree, "folder/selected.txt");
    tree->setFocus(Qt::OtherFocusReason);
    tree->setCurrentItem(selected);
    REQUIRE(tree->currentItem() == selected);
    std::filesystem::remove_all(root / "folder");
    panel.refresh();
    settle(panel);
    CHECK(tree->currentItem() == nullptr);
    CHECK(tree->selectedItems().empty());
    CHECK(row(tree, "folder") == nullptr);
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a r hidden and tabified panels read only when shown", "[prj1a]") {
    QTemporaryDir temporary;
    const auto root = rootOf(temporary);
    write(root / "initial.txt", "initial");
    QMainWindow window;
    window.setCentralWidget(new QWidget(&window));
    auto *other = new QDockWidget(QStringLiteral("Other"), &window);
    auto *dock = new QDockWidget(QStringLiteral("Project"), &window);
    auto *panel = new ProjectPanel(dock);
    dock->setWidget(panel);
    window.addDockWidget(Qt::LeftDockWidgetArea, other);
    window.addDockWidget(Qt::LeftDockWidgetArea, dock);
    window.tabifyDockWidget(other, dock);
    window.resize(800, 600);
    window.show();
    other->raise();
    QApplication::processEvents();
    CHECK(panel->isVisible());
    panel->open(root);
    panel->refresh();
    QApplication::processEvents();
    CHECK(panel->counters().scans_started == 0);
    dock->raise();
    settle(*panel);
    CHECK(panel->counters().scans_started == 1);
    other->raise();
    QApplication::processEvents();
    write(root / "shown.txt", "shown");
    dock->raise();
    settle(*panel);
    CHECK(panel->counters().scans_started == 2);
    CHECK(row(panel->tree(), "shown.txt") != nullptr);

    dock->hide();
    QApplication::processEvents();
    write(root / "hidden.txt", "hidden");
    panel->refresh();
    QApplication::processEvents();
    CHECK(panel->counters().scans_started == 2);
    dock->show();
    dock->raise();
    settle(*panel);
    CHECK(panel->counters().scans_started == 3);
    CHECK(row(panel->tree(), "hidden.txt") != nullptr);
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a r hiding during a scan discards its delivery", "[prj1a]") {
    QTemporaryDir temporary;
    const auto root = rootOf(temporary);
    write(root / "late.txt", "late");
    std::atomic_bool entered = false;
    std::atomic_bool release = false;
    ProjectPanel panel;
    panel.setScanFunctionForTest([&](const auto &path, const auto &options) {
        entered = true;
        for (int i = 0; i < 100 && !release && !options.should_stop(); ++i)
            QThread::msleep(10);
        return scanProjectFolder(path, options);
    });
    showPanel(panel);
    panel.open(root);
    INFO("visible=" << panel.isVisible() << " scans_started=" << panel.counters().scans_started);
    step({}, [&] { return entered.load(); });
    panel.hide();
    release = true;
    QApplication::processEvents();
    step({}, [&] { return panel.counters().results_discarded >= 1; });
    settle(panel);
    CHECK(row(panel.tree(), "late.txt") == nullptr);
    CHECK(panel.counters().results_discarded >= 1);
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a s a stale scan cannot restore deleted rows", "[prj1a]") {
    QTemporaryDir temporary;
    const auto root = rootOf(temporary);
    write(root / "delete.txt", "delete");
    write(root / "keep.txt", "keep");
    std::atomic_int calls = 0;
    std::atomic_bool entered = false;
    std::atomic_bool release = false;
    ProjectPanel panel;
    panel.setScanFunctionForTest([&](const auto &path, const auto &options) {
        auto listing = scanProjectFolder(path, options);
        if (++calls == 1) {
            entered = true;
            for (int i = 0; i < 1000 && !release; ++i) QThread::msleep(10);
        }
        return listing;
    });
    showPanel(panel);
    panel.open(root);
    step({}, [&] { return entered.load(); });
    std::filesystem::remove(root / "delete.txt");
    panel.refresh();
    release = true;
    settle(panel);
    CHECK(calls >= 2);
    CHECK(row(panel.tree(), "delete.txt") == nullptr);
    CHECK(row(panel.tree(), "keep.txt") != nullptr);
    CHECK(panel.counters().results_discarded >= 1);
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a t project switching cancels the old scan and pending drains", "[prj1a]") {
    QTemporaryDir first;
    QTemporaryDir second;
    for (int i = 0; i < 100; ++i)
        write(rootOf(first) / ("a" + std::to_string(i) + ".txt"), "a");
    write(rootOf(second) / "b.txt", "b");
    std::atomic_bool entered = false;
    std::atomic_bool saw_cancel = false;
    std::atomic_bool release = false;
    ProjectPanel panel;
    panel.setApplyBudgetMsForTest(0);
    panel.setScanFunctionForTest([&](const auto &path, const auto &options) {
        if (path == rootOf(first)) {
            entered = true;
            for (int i = 0; i < 1000 && !release; ++i) {
                saw_cancel = saw_cancel || options.should_stop();
                if (saw_cancel) break;
                QThread::msleep(10);
            }
        }
        return scanProjectFolder(path, options);
    });
    showPanel(panel);
    panel.open(rootOf(first));
    step({}, [&] { return entered.load(); });
    panel.open({});
    step({}, [&] { return saw_cancel.load(); });
    panel.open(rootOf(second));
    release = true;
    settle(panel);
    CHECK(row(panel.tree(), "b.txt") != nullptr);
    for (const auto &[path, item] : rows(panel.tree())) CHECK(!path.starts_with('a'));
    panel.open({});
    CHECK(panel.tree()->topLevelItemCount() == 0);
    CHECK(panel.idle());
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a t refresh and reveal coalesce during a partial apply", "[prj1a]") {
    QTemporaryDir temporary;
    const auto root = rootOf(temporary);
    for (int i = 0; i < 100; ++i) write(root / ("f" + std::to_string(i) + ".txt"), "x");
    ProjectPanel panel;
    panel.setApplyBudgetMsForTest(0);
    showPanel(panel);
    panel.open(root);
    step({}, [&] { return panel.counters().scans_completed == 1; });
    panel.refresh();
    panel.refresh();
    step({}, [&] { return panel.counters().row_edits_applied >= 100; });
    settle(panel);
    CHECK(panel.counters().scans_started <= 3);
    CHECK(panel.idle());
    panel.hide();
    panel.show();
    settle(panel);
    CHECK(panel.idle());
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a u scanning and blocked I/O stay off the GUI thread", "[prj1a]") {
    QTemporaryDir temporary;
    write(rootOf(temporary) / "file.txt", "file");
    ProjectPanel panel;
    Heartbeat heartbeat;
    QThread *scan_thread = nullptr;
    panel.setScanFunctionForTest([&](const auto &path, const auto &options) {
        scan_thread = QThread::currentThread();
        QThread::msleep(1500);
        return scanProjectFolder(path, options);
    });
    showPanel(panel);
    panel.open(rootOf(temporary));
    settle(panel, &heartbeat);
    CHECK(scan_thread != QThread::currentThread());
    CHECK(scan_thread != nullptr);
    INFO("PRJ-1a u largest GUI gap ms=" << heartbeat.largest_gap);
    CHECK(panel.counters().scans_completed == 1);
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a v a 20000-file project reports each GUI stage", "[prj1a]") {
    QTemporaryDir temporary;
    const auto root = rootOf(temporary);
    for (int folder = 0; folder < 20; ++folder) {
        for (int file = 0; file < 1000; ++file) {
            const auto name = "f" + std::to_string(file) +
                              (file % 10 == 0 ? ".json" : file % 3 == 0 ? ".frag" : ".txt");
            write(root / ("folder" + std::to_string(folder)) / name,
                  file % 10 == 0 ? R"({"schema":"pelican.scene","version":1})" : "x");
        }
    }
    ProjectPanel panel;
    showPanel(panel);
    panel.resize(800, 600);
    Heartbeat heartbeat;
    std::map<std::string, qint64> gaps;
    QElapsedTimer duration;
    duration.start();
    panel.open(root);
    settle(panel, &heartbeat);
    gaps["load"] = heartbeat.largest_gap;
    const auto load_ms = duration.elapsed();
    CAPTURE(load_ms);
    CHECK(panel.truncatedLine()->isVisible());
    CHECK(panel.truncatedLine()->text().contains(QStringLiteral("10000")));
    CHECK(rows(panel.tree()).size() == 10000);
    const auto stats = panel.counters();
    INFO("PRJ-1a v visits=" << stats.visited_entries << " listed=" << rows(panel.tree()).size()
                            << " json_bytes=" << stats.json_bytes
                            << " discarded=" << stats.results_discarded);
    INFO("PRJ-1a v load scan_us=" << stats.last_scan_us << " diff_us=" << stats.last_diff_us
                                  << " apply_us=" << stats.last_apply_us
                                  << " filter_us=" << stats.last_filter_us
                                  << " idle_us=" << stats.last_idle_us);

    heartbeat.largest_gap = 0;
    for (int i = 0; i < panel.tree()->topLevelItemCount(); ++i)
        panel.tree()->topLevelItem(i)->setExpanded(true);
    step({}, [&] { return QApplication::processEvents(), true; }, &heartbeat);
    gaps["top-level folders expanded"] = heartbeat.largest_gap;

    heartbeat.largest_gap = 0;
    duration.restart();
    panel.kindFilter()->setCurrentText(QStringLiteral("shader"));
    settle(panel, &heartbeat);
    gaps["kind filter on"] = heartbeat.largest_gap;
    CAPTURE(duration.elapsed());
    heartbeat.largest_gap = 0;
    duration.restart();
    panel.kindFilter()->setCurrentText(QStringLiteral("All kinds"));
    settle(panel, &heartbeat);
    gaps["filter off"] = heartbeat.largest_gap;
    CAPTURE(duration.elapsed());
    heartbeat.largest_gap = 0;
    duration.restart();
    const auto edits = panel.counters().row_edits_applied;
    panel.refresh();
    settle(panel, &heartbeat);
    gaps["no-change Refresh"] = heartbeat.largest_gap;
    CAPTURE(duration.elapsed());
    CAPTURE(gaps["load"]);
    CAPTURE(gaps["top-level folders expanded"]);
    CAPTURE(gaps["kind filter on"]);
    CAPTURE(gaps["filter off"]);
    CAPTURE(gaps["no-change Refresh"]);
    CHECK(panel.counters().row_edits_applied == edits);
    CHECK(gaps.size() == 5);

    for (int folder = 0; folder < 12; ++folder)
        std::filesystem::remove_all(root / ("folder" + std::to_string(folder)));
    panel.refresh();
    settle(panel);
    CHECK(panel.truncatedLine()->isHidden());
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a w a zero apply budget performs one row edit per turn", "[prj1a]") {
    QTemporaryDir temporary;
    for (int i = 0; i < 200; ++i)
        write(rootOf(temporary) / ("f" + std::to_string(i) + ".txt"), "x");
    ProjectPanel panel;
    panel.setApplyBudgetMsForTest(0);
    showPanel(panel);
    panel.open(rootOf(temporary));
    settle(panel);
    CHECK(panel.counters().apply_turns >= 200);
    CHECK(panel.counters().row_edits_applied == 200);
    CHECK(panel.idle());
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a x the panel never changes files and copies a native absolute path", "[prj1a]") {
    QTemporaryDir temporary;
    const auto root = rootOf(temporary);
    write(root / "folder" / "file.txt", "contents");
    const auto before = snapshot(root);
    ProjectPanel panel;
    showPanel(panel);
    panel.open(root);
    settle(panel);
    panel.searchBox()->setText(QStringLiteral("file"));
    settle(panel);
    panel.refresh();
    settle(panel);
    auto *file = requiredRow(panel.tree(), "folder/file.txt");
    panel.tree()->setCurrentItem(file);
    panel.copyPathAction()->trigger();
    const auto expected = QDir::toNativeSeparators(
        QString::fromStdString((root / "folder" / "file.txt").generic_string()));
    CHECK(QGuiApplication::clipboard()->text() == expected);
    CHECK(snapshot(root) == before);
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a y destruction cancels a cooperative scan", "[prj1a]") {
    QTemporaryDir temporary;
    std::atomic_bool entered = false;
    std::atomic_bool worker_done = false;
    auto *panel = new ProjectPanel;
    panel->setScanFunctionForTest([&](const auto &path, const auto &options) {
        entered = true;
        while (!options.should_stop()) QThread::msleep(5);
        worker_done = true;
        return scanProjectFolder(path, options);
    });
    showPanel(*panel);
    panel->open(rootOf(temporary));
    step({}, [&] { return entered.load(); });
    delete panel;
    step({}, [&] { return worker_done.load(); });
    CHECK(worker_done);
    SUCCEED();
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a blocked filesystem I/O has a bounded, non-hanging teardown", "[prj1a]") {
    QTemporaryDir temporary;
    std::atomic_bool entered = false;
    std::atomic_bool release = false;
    std::atomic_bool worker_done = false;
    auto *panel = new ProjectPanel;
    panel->setScanFunctionForTest([&](const auto &path, const auto &options) {
        (void)options;
        entered = true;
        for (int i = 0; i < 1000 && !release; ++i) QThread::msleep(10);
        worker_done = true;
        return scanProjectFolder(path, {});
    });
    showPanel(*panel);
    panel->open(rootOf(temporary));
    step({}, [&] { return entered.load(); });
    QElapsedTimer teardown;
    teardown.start();
    delete panel;
    const auto elapsed = teardown.elapsed();
    release = true;
    step({}, [&] { return worker_done.load(); });
    INFO("PRJ-1a blocked-I/O teardown ms=" << elapsed);
    CHECK(elapsed >= 1500);
    CHECK(elapsed < 10000);
    CHECK(worker_done);
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a switching to an empty or hidden project only cancels", "[prj1a]") {
    QTemporaryDir temporary;
    std::atomic_bool entered = false;
    std::atomic_bool cancelled = false;
    std::atomic_bool worker_done = false;
    ProjectPanel panel;
    panel.setScanFunctionForTest([&](const auto &path, const auto &options) {
        entered = true;
        for (int i = 0; i < 1000; ++i) {
            if (options.should_stop()) {
                cancelled = true;
                break;
            }
            QThread::msleep(5);
        }
        worker_done = true;
        auto listing = scanProjectFolder(path, options);
        listing.cancelled = true;
        return listing;
    });
    showPanel(panel);
    panel.open(rootOf(temporary));
    step({}, [&] { return entered.load(); });
    panel.hide();
    panel.open({});
    step({}, [&] { return worker_done.load(); });
    CHECK(cancelled);
    CHECK(panel.counters().scans_started == 1);
    CHECK(panel.idle());
}

TEST_CASE_METHOD(QtFixture, "PRJ-1a z wrapped notices do not raise the panel width floor", "[prj1a]") {
    ProjectPanel panel;
    showPanel(panel);
    const int baseline = panel.minimumSizeHint().width();
    const QString long_text = [] {
        QString result;
        for (int i = 0; i < 50; ++i) result += QStringLiteral("short ");
        return result.trimmed().repeated(6);
    }();
    REQUIRE(long_text.size() >= 300);
    panel.truncatedLine()->setText(long_text);
    panel.noticeLine()->setText(long_text);
    panel.truncatedLine()->show();
    panel.noticeLine()->show();
    QApplication::processEvents();
    CHECK(panel.minimumSizeHint().width() == baseline);
    CHECK(panel.truncatedLine()->wordWrap());
    CHECK(panel.noticeLine()->wordWrap());
}
