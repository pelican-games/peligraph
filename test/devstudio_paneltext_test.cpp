// N-7 rule 6: the text budget. Each panel is built in its default state --
// nothing selected, nothing typed -- and every string it puts on screen is
// counted: QLabel text, QGroupBox titles, button text and the header labels
// and rows of the trees a panel uses as a pane. Tooltips are not counted,
// because a tooltip is text a reader asked for.
//
// The limits below were measured on this branch and then written down. The
// control that says they are measurements rather than round numbers is in the
// report: halving any one of them turns its case red.

#include "frameplanwidget.hpp"
#include "fullscreenpasswidget.hpp"
#include "graphcanvaswidget.hpp"
#include "inspectorwidget.hpp"
#include "panelhelp.hpp"
#include "renderfeatureswidget.hpp"
#include "studiotheme.hpp"

#include "embeddedviewport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QWidget>

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

#ifndef PELICAN_TEST_FRAME_PLAN_FIXTURE
#error "PELICAN_TEST_FRAME_PLAN_FIXTURE must name the frame plan fixture"
#endif

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_panel_text_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

// A widget is on screen when nothing between it and the panel is hidden.
// `isVisible()` cannot answer that here: none of these panels is shown, and a
// panel that had to be shown would need a screen the offscreen platform does
// not have.
bool shownWithin(const QWidget &panel, const QWidget *widget) {
    // The panel itself is never shown in this test -- a top-level widget is
    // "hidden" until show(), and showing one needs a screen the offscreen
    // platform does not have -- so the walk stops at the panel rather than
    // asking whether the panel is on a desktop.
    for (const QWidget *step = widget; step != nullptr;
         step = step->parentWidget()) {
        if (step == &panel) return true;
        if (step->isHidden()) return false;
    }
    return false;
}

int treeTextLength(const QWidget &panel, const QTreeWidget &tree) {
    if (!shownWithin(panel, &tree)) return 0;
    int total = 0;
    for (int column = 0; column < tree.columnCount(); ++column)
        total += static_cast<int>(
            tree.headerItem()->text(column).trimmed().size());
    QTreeWidgetItemIterator entry{const_cast<QTreeWidget *>(&tree)};
    for (; *entry != nullptr; ++entry) {
        if ((*entry)->isHidden()) continue;
        for (int column = 0; column < tree.columnCount(); ++column)
            total += static_cast<int>((*entry)->text(column).trimmed().size());
    }
    return total;
}

// Everything the panel is currently spelling out, in characters.
int visibleTextLength(const QWidget &panel) {
    int total = 0;
    for (const QLabel *label : panel.findChildren<QLabel *>())
        if (shownWithin(panel, label))
            total += static_cast<int>(label->text().trimmed().size());
    for (const QGroupBox *box : panel.findChildren<QGroupBox *>())
        if (shownWithin(panel, box))
            total += static_cast<int>(box->title().trimmed().size());
    for (const QAbstractButton *button :
         panel.findChildren<QAbstractButton *>())
        if (shownWithin(panel, button))
            total += static_cast<int>(button->text().trimmed().size());
    for (const QTreeWidget *tree : panel.findChildren<QTreeWidget *>())
        total += treeTextLength(panel, *tree);
    return total;
}

// Rule 1 is only kept if the sentence that left the panel is still reachable.
// A tooltip anywhere under the panel counts, because that is where a reader
// looks: on the control, on the group, or on the `?`.
bool someTooltipContains(const QWidget &panel, const QString &opening) {
    if (panel.toolTip().contains(opening)) return true;
    for (const QWidget *child : panel.findChildren<QWidget *>())
        if (child->toolTip().contains(opening)) return true;
    for (const QTreeWidget *tree : panel.findChildren<QTreeWidget *>()) {
        QTreeWidgetItemIterator entry{const_cast<QTreeWidget *>(tree)};
        for (; *entry != nullptr; ++entry)
            for (int column = 0; column < tree->columnCount(); ++column)
                if ((*entry)->toolTip(column).contains(opening)) return true;
    }
    return false;
}

void report(const char *panel, int measured, int limit) {
    std::cout << "N-7 budget: " << panel << " = " << measured
              << " (limit " << limit << ")\n";
}

FramePlanReadCapability idleFramePlanCapability() {
    FramePlanReadCapability capability;
    capability.ready = [](QObject *, FramePlanReadCapability::ReadyHandler
                                         handler) { handler(false, {}); };
    capability.requestFramePlan = [](QString *) { return qint64{0}; };
    capability.result = [](QObject *, FramePlanReadCapability::ResultHandler) {};
    capability.failure = [](QObject *,
                            FramePlanReadCapability::FailureHandler) {};
    return capability;
}

GraphCanvasReadCapability idleGraphCapability() {
    GraphCanvasReadCapability capability;
    capability.ready = [](QObject *, GraphCanvasReadCapability::ReadyHandler
                                         handler) { handler(false, {}); };
    capability.requestGraphInstances = [](QString *) { return qint64{0}; };
    capability.requestGraphCatalog = [](QString *) { return qint64{0}; };
    capability.requestGraphValues = [](const QString &, QString *) {
        return qint64{0};
    };
    capability.requestGraphNodeValues = [](const QString &, QString *) {
        return qint64{0};
    };
    capability.requestStatus = [](QString *) { return qint64{0}; };
    capability.requestSetTime = [](double, QString *) { return qint64{0}; };
    capability.requestRenderFrame = [](QString *) { return qint64{0}; };
    capability.requestEditGraphDocument = [](const QString &,
                                             const QByteArray &, QString *) {
        return qint64{0};
    };
    capability.requestEditResult = [](const QString &, QString *) {
        return qint64{0};
    };
    capability.result = [](QObject *, GraphCanvasReadCapability::ResultHandler) {
    };
    capability.failure = [](QObject *,
                            GraphCanvasReadCapability::FailureHandler) {};
    return capability;
}

// The shipped fixture plus the two fields a runtime snapshot carries, the
// same way the frame-plan graph test prepares it: without them the model
// refuses the document and the panel would be measured in its failure state.
std::string readFixture() {
    std::ifstream input{PELICAN_TEST_FRAME_PLAN_FIXTURE, std::ios::binary};
    REQUIRE(input.good());
    Json plan = Json::parse(std::string{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}});
    plan["profile"] = "runtime";
    for (auto &node : plan.at("nodes"))
        node["shader_resolution"] = Json{{"state", "not_applicable"}};
    return plan.dump();
}

// The limits, measured on this branch. Halving one is the control.
constexpr int PassEditorLimit = 400;
constexpr int RenderFeaturesLimit = 300;
constexpr int InspectorLimit = 200;
constexpr int FramePlanLimit = 300;
constexpr int GraphRightPaneLimit = 150;

} // namespace

TEST_CASE("N-7 the Pass Editor spends its text budget on names, not sentences",
          "[devstudio][n7][panel-text][pass-editor]") {
    (void)application();
    FullscreenPassWidget form{idleFramePlanCapability()};

    const int measured = visibleTextLength(form);
    report("Pass Editor", measured, PassEditorLimit);
    REQUIRE(measured <= PassEditorLimit);

    // Rule 1: each sentence that left the panel is still one hover away.
    REQUIRE(someTooltipContains(form, QStringLiteral("Save sends this draft")));
    REQUIRE(someTooltipContains(form,
                                QStringLiteral("Engine authoring context")));
    REQUIRE(someTooltipContains(form, QStringLiteral("No selection emits")));
    REQUIRE(someTooltipContains(form, QStringLiteral("Expected passes[]")));

    // Rule 3: an axis is a mark and a short word, and the sentence it stands
    // for is on the mark.
    auto *axis = form.findChild<QLabel *>(
        QStringLiteral("pelican.fullscreenPass.axis.targetNames"));
    REQUIRE(axis != nullptr);
    REQUIRE(axis->text().size() <= 10);
    REQUIRE(axis->toolTip().contains(QStringLiteral("Target names")));

    // Rule 2: nothing is omitted yet, so the row is not there at all.
    auto *omitted = form.findChild<QLabel *>(
        QStringLiteral("pelican.fullscreenPass.omittedKeys"));
    REQUIRE(omitted != nullptr);
    REQUIRE(omitted->isHidden());
    REQUIRE(omitted->text().isEmpty());

    // Rule 1: the panel names the manual section it explains itself from.
    auto *help = form.findChild<QWidget *>(
        QStringLiteral("pelican.fullscreenPass.help"));
    REQUIRE(help != nullptr);
    REQUIRE_FALSE(
        help->property("pelicanManualAnchor").toString().isEmpty());
}

TEST_CASE("N-7 Render features shows two lists and no instructions",
          "[devstudio][n7][panel-text][render-features]") {
    (void)application();
    EmbeddedViewport viewport;
    RenderFeaturesWidget panel{&viewport};

    const int measured = visibleTextLength(panel);
    report("Render features", measured, RenderFeaturesLimit);
    REQUIRE(measured <= RenderFeaturesLimit);

    REQUIRE(someTooltipContains(panel,
                                QStringLiteral("Edit the authored render")));

    auto *notice = panel.findChild<QLabel *>(
        QStringLiteral("pelican.renderFeatures.notice"));
    REQUIRE(notice != nullptr);
    // Rule 2: with no engine the panel says one line, not a paragraph.
    REQUIRE(notice->text().size() <= 90);
    REQUIRE_FALSE(notice->text().contains(QLatin1Char('\n')));
}

TEST_CASE("N-7 the Inspector with nothing selected is one dim line",
          "[devstudio][n7][panel-text][inspector]") {
    (void)application();
    EmbeddedViewport viewport;
    InspectorWidget panel{&viewport};
    panel.setSelection(std::nullopt, {});
    QApplication::processEvents();

    const int measured = visibleTextLength(panel);
    report("Inspector", measured, InspectorLimit);
    REQUIRE(measured <= InspectorLimit);

    // Rule 2: an empty notice takes its row with it, and a notice that has
    // something to say says it in one line.
    auto *notice =
        panel.findChild<QLabel *>(QStringLiteral("pelican.inspectorNotice"));
    REQUIRE(notice != nullptr);
    REQUIRE(notice->isHidden() == notice->text().isEmpty());
    REQUIRE(notice->text().size() <= 90);
    REQUIRE_FALSE(notice->text().contains(QLatin1Char('\n')));

    REQUIRE(someTooltipContains(panel, QStringLiteral("Edit the declaration")));
}

TEST_CASE("N-7 the Frame Plan states what it holds in one line",
          "[devstudio][n7][panel-text][frame-plan]") {
    (void)application();
    EmbeddedViewport viewport;
    FramePlanWidget panel{&viewport};
    panel.receiveResult(QByteArray::fromStdString(readFixture()));
    QApplication::processEvents();

    const int measured = visibleTextLength(panel);
    report("Frame Plan", measured, FramePlanLimit);
    REQUIRE(measured <= FramePlanLimit);

    auto *status =
        panel.findChild<QLabel *>(QStringLiteral("pelican.framePlanStatus"));
    REQUIRE(status != nullptr);
    REQUIRE_FALSE(status->text().isEmpty());
    REQUIRE_FALSE(status->text().contains(QLatin1Char('\n')));
    // The physical and execution planning, the response size and the refresh
    // time left the line for the tooltip, which still carries them.
    REQUIRE_FALSE(status->text().contains(QStringLiteral("response")));
    REQUIRE(status->toolTip().contains(QStringLiteral("response")));
    REQUIRE(status->toolTip().contains(QStringLiteral("not polled per frame")));

    REQUIRE(someTooltipContains(panel, QStringLiteral("Shift-drag")));
}

TEST_CASE("N-7 the Graph right pane is a heading and a table, not a paragraph",
          "[devstudio][n7][panel-text][graph-canvas]") {
    (void)application();
    GraphCanvasWidget panel{idleGraphCapability()};

    auto *details =
        panel.findChild<QTreeWidget *>(QStringLiteral("pelican.graphCanvasDetails"));
    REQUIRE(details != nullptr);
    QWidget *pane = details->parentWidget();
    REQUIRE(pane != nullptr);

    const int measured = visibleTextLength(*pane);
    report("Graph right pane", measured, GraphRightPaneLimit);
    REQUIRE(measured <= GraphRightPaneLimit);

    REQUIRE(details->headerItem()->text(0) == QStringLiteral("Field"));
    REQUIRE(details->headerItem()->text(1) == QStringLiteral("Value"));

    auto *notice =
        panel.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasNotice"));
    REQUIRE(notice != nullptr);
    REQUIRE(notice->isHidden() == notice->text().isEmpty());
    REQUIRE(notice->text().size() <= 90);
}

TEST_CASE("N-7 the panel font comes from the theme table, not from a panel",
          "[devstudio][n7][panel-text][theme]") {
    (void)application();
    const StudioTheme &theme = currentStudioTheme();
    REQUIRE_FALSE(theme.font.family.isEmpty());
    REQUIRE(theme.font.size > 0.0);
    REQUIRE(theme.font.size_small > 0.0);
    REQUIRE(theme.font.size_small < theme.font.size);

    const QFont panel_font = studioPanelFont();
    const QFont small_font = studioSmallFont();
    REQUIRE(panel_font.families().contains(theme.font.family));
    REQUIRE(panel_font.families().contains(theme.font.fallback));
    REQUIRE(panel_font.pixelSize() > small_font.pixelSize());

    // The control: a table that names another size hands out that size.
    setStudioThemeTable(R"({
      "default": "probe",
      "themes": {
        "probe": {
          "label": "Probe",
          "font": {"family": "Probe Sans", "fallback": "Probe Mincho",
                   "size": 20, "size_small": 17}
        }
      }
    })");
    REQUIRE(studioPanelFont().pixelSize() == 20);
    REQUIRE(studioSmallFont().pixelSize() == 17);
    REQUIRE(studioPanelFont().families().contains(
        QStringLiteral("Probe Sans")));
    resetStudioThemeTable();
    REQUIRE(studioPanelFont().pixelSize() ==
            static_cast<int>(currentStudioTheme().font.size));
}

} // namespace PelicanStudio
