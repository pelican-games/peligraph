// OSC-3c: the Live Inputs dock on the real widget, with the test answering for
// the engine through the dock's own hooks.  The settings are per user; the
// listener and the traffic are whatever the engine last reported.
#include "../src/devstudio/viewport/studioplayerarguments.hpp"
#include "liveinputsdock.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using namespace PelicanStudio;
using Json = nlohmann::json;

namespace {

QApplication &application()
{
    static int argc = 1;
    static char name[] = "devstudio_liveinputs_dock_test";
    static char *argv[] = {name, nullptr};
    static QApplication app{argc, argv};
    return app;
}

struct FakeEngine {
    qint64 next_id = 1;
    qint64 generation = 1;
    qint64 now = 0;
    QList<qint64> sent;
    QStringList methods;

    LiveInputsDock::Hooks hooks()
    {
        return LiveInputsDock::Hooks{
            .send = [this](const QString &method, const QJsonObject &, QString *) {
                methods << method;
                sent << next_id;
                return next_id++;
            },
            .engine_identity = [this] { return generation; },
            .restart = [this](const QString &reason) {
                restart_reason = reason;
                ++restart_count;
            },
            .applied = [this](const StudioOscSettings &settings) { applied = settings; },
            .configured_arguments = [this] { return environment; },
            .clock_ms = [this] { return now; },
        };
    }

    QByteArray answer(qint64 id, const Json &reply) const
    {
        return QByteArray::fromStdString(reply.dump());
    }

    QStringList environment;
    QString restart_reason;
    StudioOscSettings applied;
    int restart_count = 0;
};

struct Fixture {
    QTemporaryDir directory;
    QSettings settings{directory.filePath(QStringLiteral("studio.ini")),
                       QSettings::IniFormat};
    FakeEngine engine;
    std::unique_ptr<LiveInputsDock> dock;

    Fixture()
    {
        dock = std::make_unique<LiveInputsDock>(engine.hooks(), settings);
        dock->show();
        application().processEvents();
    }

    QTreeWidget *inputs()
    {
        return dock->findChild<QTreeWidget *>(QStringLiteral("pelican.liveInputs.table"));
    }
    QTreeWidget *recent()
    {
        return dock->findChild<QTreeWidget *>(QStringLiteral("pelican.liveInputs.recent"));
    }
    QLabel *listener()
    {
        return dock->findChild<QLabel *>(QStringLiteral("pelican.liveInputs.listener"));
    }
    QLabel *rate()
    {
        return dock->findChild<QLabel *>(QStringLiteral("pelican.liveInputs.rate"));
    }
    QLabel *note()
    {
        return dock->findChild<QLabel *>(QStringLiteral("pelican.liveInputs.note"));
    }
    QGroupBox *settingsGroup()
    {
        return dock->findChild<QGroupBox *>(QStringLiteral("pelican.liveInputs.settings"));
    }
    QCheckBox *enabled()
    {
        return dock->findChild<QCheckBox *>(QStringLiteral("pelican.liveInputs.enabled"));
    }
    QSpinBox *port()
    {
        return dock->findChild<QSpinBox *>(QStringLiteral("pelican.liveInputs.port"));
    }
    QComboBox *interface()
    {
        return dock->findChild<QComboBox *>(QStringLiteral("pelican.liveInputs.interface"));
    }
    QLineEdit *allow()
    {
        return dock->findChild<QLineEdit *>(QStringLiteral("pelican.liveInputs.allow"));
    }
    QPushButton *apply()
    {
        return dock->findChild<QPushButton *>(QStringLiteral("pelican.liveInputs.apply"));
    }

    qint64 takeRequest()
    {
        REQUIRE_FALSE(engine.sent.isEmpty());
        return engine.sent.takeFirst();
    }

    void reply(qint64 id, const Json &body)
    {
        REQUIRE(dock->receiveResult(id, engine.answer(id, body)));
    }
};

Json input(const char *name, const char *type, const Json &value, const char *source)
{
    return Json{{"name", name}, {"type", type}, {"value", value},
                {"default", value}, {"source", source}, {"changed_frame", 3},
                {"clamped", false}};
}

Json listening(qint64 received, int port, Json recent = Json::array())
{
    return Json{{"inputs", Json::array()},
                {"dropped_during_replay", 0},
                {"osc",
                 {{"received", received},
                  {"applied", received},
                  {"unbound", 0},
                  {"dropped", {{"unknown_input", 0}, {"unbound", 0}, {"bad_value", 0}}},
                  {"recent", std::move(recent)},
                  {"listener",
                   {{"state", "listening"},
                    {"interface", "127.0.0.1"},
                    {"port", port},
                    {"error", ""},
                    {"sender_not_allowed", 0},
                    {"receive_errors", 0}}}}}};
}

} // namespace

TEST_CASE("OSC-3c the declared inputs and listener come from the engine, with or without osc",
          "[osc3c][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    fixture.dock->engineAvailable();
    REQUIRE_FALSE(fixture.engine.sent.isEmpty());
    const qint64 request = fixture.takeRequest();
    const Json body{{"inputs",
                     Json::array({input("gain", "float", 0.5, "project"),
                                  input("tint", "color", "#ff0000", "default")})},
                    {"dropped_during_replay", 0}};
    fixture.reply(request, body);

    QTreeWidget *table = fixture.inputs();
    REQUIRE(table != nullptr);
    REQUIRE(table->topLevelItemCount() == 2);
    CHECK(std::string{table->topLevelItem(0)->text(0).toStdString()} == "gain");
    CHECK(std::string{table->topLevelItem(0)->text(1).toStdString()} == "float");
    CHECK(std::string{table->topLevelItem(0)->text(2).toStdString()} == "0.5");
    CHECK(std::string{table->topLevelItem(0)->text(3).toStdString()} == "project");
    CHECK(std::string{table->topLevelItem(1)->text(0).toStdString()} == "tint");
    // A build without OSC has no osc object, but the declared inputs are
    // still the engine's.
    CHECK(std::string{fixture.listener()->text().toStdString()} == "off");
    CHECK(std::string{fixture.rate()->text().toStdString()} == "—");
}

TEST_CASE("OSC-3c the settings are off by default and only Studio adds the flags",
          "[osc3c][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    CHECK_FALSE(fixture.enabled()->isChecked());
    CHECK_FALSE(fixture.apply()->isEnabled());

    StudioOscSettings settings;
    const auto plain = studioPlayerLaunch({"project.json"}, "/project", settings);
    for (const std::string &argument : plain.arguments) {
        CHECK(argument.find("--osc-") == std::string::npos);
    }
    if (studioBuildHasOsc()) {
        CHECK(std::string{fixture.note()->text().toStdString()}.empty());
    } else {
        CHECK(std::string{fixture.note()->text().toStdString()}.find(
                  "built without OSC") != std::string::npos);
    }

    settings.enabled = true;
    settings.port = 0;
    const auto listening = studioPlayerLaunch({"project.json"}, "/project", settings);
    bool saw_port = false;
    for (const std::string &argument : listening.arguments) {
        if (argument == "--osc-port") saw_port = true;
        if (!studioBuildHasOsc())
            CHECK(argument.find("--osc-") == std::string::npos);
    }
    CHECK(saw_port == studioBuildHasOsc());
}

TEST_CASE("OSC-3c the environment's OSC options win as a whole",
          "[osc3c][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    fixture.engine.environment = {QStringLiteral("--osc-port=12000")};
    // The dock reads the environment at construction; rebuild it with this
    // engine so the group state follows the same list the launcher sees.
    fixture.dock = std::make_unique<LiveInputsDock>(fixture.engine.hooks(), fixture.settings);
    fixture.dock->show();
    application().processEvents();
    CHECK_FALSE(fixture.settingsGroup()->isEnabled());
    CHECK_FALSE(fixture.apply()->isEnabled());
    CHECK(std::string{fixture.note()->text().toStdString()}.find(
              "PELICAN_STUDIO_PLAYER_ARGUMENTS") != std::string::npos);

    StudioOscSettings settings;
    settings.enabled = true;
    settings.port = 9999;
    for (const QStringList &environment :
         {QStringList{QStringLiteral("--osc-port"), QStringLiteral("12000")},
          QStringList{QStringLiteral("--osc-port=12000")},
          QStringList{QStringLiteral("--osc-interface=0.0.0.0")},
          QStringList{QStringLiteral("--osc-allow=192.0.2.4")}}) {
        std::vector<std::string> configured;
        for (const QString &argument : environment)
            configured.push_back(argument.toStdString());
        const auto launched = studioPlayerLaunch(configured, "/project", settings);
        CHECK(std::find(launched.arguments.begin(), launched.arguments.end(),
                        configured.front()) != launched.arguments.end());
        // The settings' port is not added a second time as a separate flag
        // and value.
        CHECK(std::find(launched.arguments.begin(), launched.arguments.end(),
                        std::string{"9999"}) == launched.arguments.end());
    }
}

TEST_CASE("OSC-3c a reply updates rows in place and keeps the selection",
          "[osc3c][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    fixture.dock->engineAvailable();
    const qint64 first = fixture.takeRequest();
    fixture.reply(first, Json{{"inputs", Json::array({input("gain", "float", 0.5, "project")})}});
    QTreeWidget *table = fixture.inputs();
    REQUIRE(table->topLevelItemCount() == 1);
    QTreeWidgetItem *row = table->topLevelItem(0);
    table->setCurrentItem(row);
    REQUIRE(table->currentItem() == row);

    fixture.dock->readNow();
    const qint64 second = fixture.takeRequest();
    fixture.reply(second, Json{{"inputs", Json::array({input("gain", "float", 0.75, "project"),
                                                       input("tint", "color", "#00ff00", "default")})}});
    REQUIRE(table->topLevelItemCount() == 2);
    CHECK(table->topLevelItem(0) == row);
    CHECK(std::string{row->text(2).toStdString()} == "0.75");
    CHECK(table->currentItem() == row);
    CHECK(std::string{table->topLevelItem(1)->text(0).toStdString()} == "tint");
}

TEST_CASE("OSC-3c the dock reads only while it is shown",
          "[osc3c][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    fixture.dock->engineAvailable();
    REQUIRE_FALSE(fixture.engine.sent.isEmpty());
    fixture.takeRequest();
    fixture.dock->hide();
    application().processEvents();
    fixture.engine.sent.clear();
    fixture.dock->readNow();
    CHECK(fixture.engine.sent.isEmpty());

    fixture.dock->show();
    application().processEvents();
    // A show reads at once, not on the first tick of the polling timer.
    REQUIRE(fixture.engine.sent.size() == 1);
    const qint64 request = fixture.takeRequest();
    fixture.dock->hide();
    application().processEvents();
    // The reply was for a request from before the hide: it is not the answer
    // to a current request, and it does not start one while hidden.
    CHECK_FALSE(fixture.dock->receiveResult(request, fixture.engine.answer(request, listening(1, 9000))));
    CHECK(fixture.engine.sent.isEmpty());
}

TEST_CASE("OSC-3c Apply saves the user's settings, hands them to the engine and asks for a restart",
          "[osc3c][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    if (!studioBuildHasOsc()) {
        CHECK_FALSE(fixture.settingsGroup()->isEnabled());
        CHECK_FALSE(fixture.apply()->isEnabled());
        CHECK(std::string{fixture.note()->text().toStdString()}.find(
                  "built without OSC") != std::string::npos);
        return;
    }
    fixture.dock->engineAvailable();
    const qint64 request = fixture.takeRequest();
    fixture.reply(request, listening(0, 9000));

    fixture.enabled()->setChecked(true);
    fixture.port()->setValue(0);
    fixture.interface()->setCurrentText(QStringLiteral("127.0.0.1"));
    application().processEvents();
    REQUIRE(fixture.apply()->isEnabled());
    fixture.apply()->click();
    CHECK(fixture.engine.applied.enabled);
    CHECK(fixture.engine.applied.port == 0);
    CHECK(fixture.engine.restart_count == 1);
    CHECK(std::string{fixture.engine.restart_reason.toStdString()} == "OSC settings changed");
    QSettings saved(fixture.directory.filePath(QStringLiteral("studio.ini")),
                    QSettings::IniFormat);
    CHECK(saved.value(QStringLiteral("osc/enabled")).toBool());
    CHECK(saved.value(QStringLiteral("osc/port")).toInt() == 0);

    const QString declined = QStringLiteral(
        "The engine was not restarted. The setting is saved and applies the next time the engine starts.");
    fixture.engine.restart_count = 0;
    fixture.dock->engineRestartDeclined(declined);
    CHECK(std::string{fixture.note()->text().toStdString()}.find(
              declined.toStdString()) != std::string::npos);
    CHECK(fixture.engine.restart_count == 0);
}

TEST_CASE("OSC-3c the rate follows osc.received and resets on a new listening session",
          "[osc3c][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    fixture.dock->engineAvailable();
    qint64 request = fixture.takeRequest();
    fixture.reply(request, listening(10, 45123,
        Json::array({{{"sequence", 1}, {"address", "/tint"}, {"input", ""},
                      {"bound", false}, {"reason", "unknown address"},
                      {"sender", "127.0.0.1"}},
                     {{"sequence", 2}, {"address", "/gain"}, {"input", "gain"},
                      {"bound", true}, {"reason", ""}, {"sender", "127.0.0.1"}}})));
    CHECK(std::string{fixture.listener()->text().toStdString()} ==
          "listening on 127.0.0.1:45123");
    CHECK(std::string{fixture.rate()->text().toStdString()} == "—");
    REQUIRE(fixture.recent()->topLevelItemCount() == 2);
    CHECK(std::string{fixture.recent()->topLevelItem(0)->text(0).toStdString()} == "/gain");
    CHECK(std::string{fixture.recent()->topLevelItem(0)->text(3).toStdString()} == "applied");
    CHECK(std::string{fixture.recent()->topLevelItem(1)->text(3).toStdString()} ==
          "unknown address");

    fixture.engine.now = 1000;
    fixture.dock->readNow();
    request = fixture.takeRequest();
    fixture.reply(request, listening(20, 45123));
    CHECK(std::string{fixture.rate()->text().toStdString()} == "10.0/s");

    fixture.dock->readNow();
    request = fixture.takeRequest();
    fixture.reply(request, listening(5, 45123));
    CHECK(std::string{fixture.rate()->text().toStdString()} == "—");

    fixture.engine.now = 2000;
    fixture.dock->readNow();
    request = fixture.takeRequest();
    fixture.reply(request, listening(9, 45123));
    CHECK(std::string{fixture.rate()->text().toStdString()} == "4.0/s");
    fixture.engine.now = 3000;
    fixture.dock->readNow();
    request = fixture.takeRequest();
    fixture.reply(request, listening(19, 45123));
    CHECK(std::string{fixture.rate()->text().toStdString()} == "10.0/s");

    // A missing listener / no osc object resets the count.
    fixture.dock->readNow();
    request = fixture.takeRequest();
    fixture.reply(request, Json{{"inputs", Json::array()}});
    CHECK(std::string{fixture.listener()->text().toStdString()} == "off");
    CHECK(std::string{fixture.rate()->text().toStdString()} == "—");

    // Reconnecting starts a new baseline even if the counter went backwards.
    fixture.dock->engineUnavailable();
    fixture.dock->engineAvailable();
    fixture.engine.now = 4000;
    fixture.dock->readNow();
    request = fixture.takeRequest();
    fixture.reply(request, listening(100, 45123));
    CHECK(std::string{fixture.rate()->text().toStdString()} == "—");
    fixture.engine.now = 5000;
    fixture.dock->readNow();
    request = fixture.takeRequest();
    fixture.reply(request, listening(110, 45123));
    CHECK(std::string{fixture.rate()->text().toStdString()} == "10.0/s");

    // A failed listener is the engine's error, not a generic absence.
    fixture.dock->readNow();
    request = fixture.takeRequest();
    fixture.reply(request, Json{{"inputs", Json::array()},
                                {"osc",
                                 {{"received", 110},
                                  {"recent", Json::array()},
                                  {"listener",
                                   {{"state", "failed"},
                                    {"interface", "127.0.0.1"},
                                    {"port", 0},
                                    {"error", "address in use"}}}}}});
    CHECK(std::string{fixture.listener()->text().toStdString()} ==
          "failed: address in use");
}

TEST_CASE("OSC-3c a stale engine reply is consumed but changes nothing",
          "[osc3c][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    fixture.dock->engineAvailable();
    const qint64 request = fixture.takeRequest();
    fixture.reply(request, listening(10, 45123));
    CHECK(std::string{fixture.listener()->text().toStdString()} ==
          "listening on 127.0.0.1:45123");

    fixture.dock->readNow();
    const qint64 stale = fixture.takeRequest();
    ++fixture.engine.generation;
    const Json failed{{"inputs", Json::array()},
                      {"osc",
                       {{"received", 99},
                        {"recent", Json::array()},
                        {"listener",
                         {{"state", "failed"}, {"port", 0}, {"error", "old engine"}}}}}};
    CHECK(fixture.dock->receiveResult(stale, fixture.engine.answer(stale, failed)));
    CHECK(std::string{fixture.listener()->text().toStdString()} ==
          "listening on 127.0.0.1:45123");
    CHECK(std::string{fixture.rate()->text().toStdString()} == "—");

}
