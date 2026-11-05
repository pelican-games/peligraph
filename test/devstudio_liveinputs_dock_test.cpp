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
#include <QtTest/QTest>
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
    QHash<qint64, QJsonObject> parameters;

    LiveInputsDock::Hooks hooks()
    {
        return LiveInputsDock::Hooks{
            .send = [this](const QString &method, const QJsonObject &params, QString *) {
                methods << method;
                sent << next_id;
                parameters.insert(next_id, params);
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

    QPushButton *unbind(const char *name, int index)
    {
        auto *tree = inputs();
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
            auto *item = tree->topLevelItem(row);
            if (name && item->text(0) != QString::fromLatin1(name)) continue;
            auto *bound = itemWidget(tree, item);
            if (!bound) continue;
            if (auto *button = bound->findChild<QPushButton *>(
                    QStringLiteral("pelican.liveInputs.unbind.%1").arg(index)))
                return button;
        }
        return nullptr;
    }

    void clickUnbind(int index)
    {
        auto *button = unbind(nullptr, index);
        REQUIRE(button != nullptr);
        button->click();
    }

    static QWidget *itemWidget(QTreeWidget *tree, QTreeWidgetItem *item)
    {
        return tree->itemWidget(item, 4);
    }

    qint64 takeRequest()
    {
        INFO("queued RPC methods: " << engine.methods.join(", ").toStdString());
        REQUIRE_FALSE(engine.sent.isEmpty());
        return engine.sent.takeFirst();
    }

    void reply(qint64 id, const Json &body)
    {
        const bool received = dock->receiveResult(id, engine.answer(id, body));
        application().processEvents();
        REQUIRE(received);
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

Json learnSnapshot(Json recent, std::uint64_t last_sequence)
{
    auto result = listening(last_sequence, 9000, std::move(recent));
    result["osc"]["last_sequence"] = last_sequence;
    result["inputs"] = Json::array({input("gain", "float", 0.5, "project"),
                                    input("steps", "int", 1, "project"),
                                    input("enabled", "bool", false, "project")});
    return result;
}

Json heard(std::uint64_t sequence, const char *address, double value,
           const char *sender = "127.0.0.1:5000")
{
    return {{"sequence", sequence}, {"address", address}, {"input", ""},
            {"bound", false}, {"reason", "unbound"}, {"sender", sender},
            {"arg_type", "f"}, {"value", value}};
}

Json bindingsResult(Json live, const char *digest = "d1", Json applied = nullptr)
{
    if (applied.is_null()) applied = live;
    return {{"profile", {{"name", "live"}, {"reference", "live/profile.json"}}},
            {"digest", digest}, {"live", std::move(live)}, {"applied_live", std::move(applied)}};
}

void primeBindings(Fixture &fixture, const Json &live)
{
    fixture.dock->engineAvailable();
    fixture.reply(fixture.takeRequest(), learnSnapshot(Json::array(), 0));
    const auto read = fixture.takeRequest();
    REQUIRE(fixture.engine.methods.last() == "get_osc_bindings");
    fixture.reply(read, bindingsResult(live));
}

void replyLive(Fixture &fixture, qint64 id, const Json &snapshot, const Json &bindings)
{
    fixture.reply(id, snapshot);
    REQUIRE_FALSE(fixture.engine.sent.isEmpty());
    const auto read = fixture.takeRequest();
    REQUIRE(fixture.engine.methods.last() == "get_osc_bindings");
    fixture.reply(read, bindings);
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


TEST_CASE("OSC-3d Learn binds an exact array and Undo follows returned digests",
          "[osc3d][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    const Json original = Json::array({
        {{"address", "/old"}, {"input", "steps"},
         {"map", {{"from", Json::array({0, 1})}, {"to", Json::array({0, 8})}}}},
    });
    primeBindings(fixture, original);
    auto *learn = fixture.dock->findChild<QPushButton *>(QStringLiteral("pelican.liveInputs.learn.gain"));
    auto *row = fixture.dock->findChild<QWidget *>(QStringLiteral("pelican.liveInputs.learnRow"));
    auto *candidates = fixture.dock->findChild<QComboBox *>(QStringLiteral("pelican.liveInputs.learnCandidates"));
    auto *bind = fixture.dock->findChild<QPushButton *>(QStringLiteral("pelican.liveInputs.learnBind"));
    auto *undo = fixture.dock->findChild<QPushButton *>(QStringLiteral("pelican.liveInputs.undoBindings"));
    REQUIRE(learn != nullptr);
    REQUIRE(learn->isEnabled());
    learn->click();
    application().processEvents();
    CHECK(row->isVisible());
    auto arming = fixture.takeRequest();
    replyLive(fixture, arming, learnSnapshot(Json::array(), 10), bindingsResult(original));
    fixture.dock->readNow();
    auto polled = fixture.takeRequest();
    const Json recent = Json::array({heard(11, "/fader/1", 0.1), heard(12, "/fader/1", 0.2)});
    replyLive(fixture, polled, learnSnapshot(recent, 12), bindingsResult(original));
    REQUIRE(candidates->count() == 1);
    CHECK(candidates->currentText().contains(QStringLiteral("/fader/1")));
    CHECK(candidates->currentText().contains(QStringLiteral("2 messages")));
    REQUIRE(bind->isEnabled());
    bind->click();
    application().processEvents();
    const auto edit = fixture.takeRequest();
    REQUIRE(fixture.engine.methods.last() == "edit_osc_bindings");
    const auto params = Json::parse(QJsonDocument(fixture.engine.parameters.value(edit)).toJson().constData());
    CHECK(params.at("base_digest") == "d1");
    CHECK(params.at("profile").at("name") == "live");
    const Json bound = Json::array({original[0], {{"address", "/fader/1"}, {"input", "gain"}}});
    CHECK(params.at("live") == bound);
    fixture.reply(edit, {{"committed", true}, {"changed", true}, {"digest", "d2"},
                         {"previous_digest", "d1"}, {"previous", original}, {"applied", bound}});
    const auto read = fixture.takeRequest();
    fixture.reply(read, bindingsResult(bound, "d2"));
    REQUIRE(undo->isEnabled());
    undo->click();
    application().processEvents();
    const auto undo_edit = fixture.takeRequest();
    const auto undo_params = Json::parse(QJsonDocument(fixture.engine.parameters.value(undo_edit)).toJson().constData());
    CHECK(undo_params.at("base_digest") == "d2");
    CHECK(undo_params.at("live") == original);
    fixture.reply(undo_edit, {{"committed", true}, {"changed", true}, {"digest", "d3"},
                              {"previous_digest", "d2"}, {"previous", bound}, {"applied", original}});
    fixture.reply(fixture.takeRequest(), bindingsResult(original, "d3"));
    CHECK_FALSE(undo->isEnabled());

    auto *remove = fixture.dock->findChild<QPushButton *>(QStringLiteral("pelican.liveInputs.unbind.0"));
    REQUIRE(remove != nullptr);
    remove->click();
    application().processEvents();
    const auto unbind = fixture.takeRequest();
    const auto unbind_params = Json::parse(QJsonDocument(fixture.engine.parameters.value(unbind)).toJson().constData());
    CHECK(unbind_params.at("base_digest") == "d3");
    CHECK(unbind_params.at("live") == Json::array());
    fixture.reply(unbind, {{"committed", true}, {"changed", true}, {"digest", "d4"},
                           {"previous_digest", "d3"}, {"previous", original}, {"applied", Json::array()}});
    fixture.reply(fixture.takeRequest(), bindingsResult(Json::array(), "d4"));
    CHECK(undo->isEnabled());
    fixture.dock->hide();
    application().processEvents();
    fixture.engine.sent.clear();
    fixture.dock->readNow();
    CHECK(fixture.engine.sent.isEmpty());
}


namespace {

Json editParameters(Fixture &fixture, qint64 id)
{
    return Json::parse(QJsonDocument(fixture.engine.parameters.value(id)).toJson().constData());
}

void hearForGain(Fixture &fixture, const Json &live, const Json &recent,
                 std::uint64_t last_sequence)
{
    auto *learn = fixture.dock->findChild<QPushButton *>(
        QStringLiteral("pelican.liveInputs.learn.gain"));
    REQUIRE(learn != nullptr);
    REQUIRE(learn->isEnabled());
    learn->click();
    application().processEvents();
    replyLive(fixture, fixture.takeRequest(),
              learnSnapshot(Json::array(), last_sequence > 0 ? last_sequence - recent.size() : 0),
              bindingsResult(live));
    fixture.dock->readNow();
    replyLive(fixture, fixture.takeRequest(), learnSnapshot(recent, last_sequence),
              bindingsResult(live));
}

} // namespace

TEST_CASE("OSC-3d Replace moves an address without its map and Already bound is explicit",
          "[osc3d][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    const Json original = Json::array({
        {{"address", "/old"}, {"input", "steps"},
         {"map", {{"from", Json::array({0, 1})}, {"to", Json::array({0, 8})}}}},
        {{"address", "/held"}, {"input", "gain"}},
        {{"address", "/other"}, {"input", "steps"}},
    });
    primeBindings(fixture, original);
    hearForGain(fixture, original,
                Json::array({heard(11, "/old", 0.1), heard(12, "/old", 0.2)}), 12);
    auto *candidates = fixture.dock->findChild<QComboBox *>(
        QStringLiteral("pelican.liveInputs.learnCandidates"));
    auto *bind = fixture.dock->findChild<QPushButton *>(
        QStringLiteral("pelican.liveInputs.learnBind"));
    CHECK(bind->text() == QStringLiteral("Replace \"steps\""));
    bind->click();
    application().processEvents();
    const auto edit = fixture.takeRequest();
    const Json replaced = Json::array({
        {{"address", "/old"}, {"input", "gain"}}, original[1], original[2]});
    CHECK(editParameters(fixture, edit).at("live") == replaced);
    fixture.reply(edit, {{"committed", true}, {"changed", true}, {"digest", "d2"},
                         {"previous_digest", "d1"}, {"previous", original}, {"applied", replaced}});
    fixture.reply(fixture.takeRequest(), bindingsResult(replaced, "d2"));
    auto read = fixture.dock->findChild<QTreeWidget *>(
        QStringLiteral("pelican.liveInputs.table"));
    REQUIRE(read != nullptr);
    CHECK(read->topLevelItem(0)->text(4) == QStringLiteral("/old, /held"));

    hearForGain(fixture, replaced,
                Json::array({heard(13, "/held", 0.2), heard(14, "/held", 0.3)}), 14);
    CHECK_FALSE(bind->isEnabled());
    CHECK(bind->toolTip() == QStringLiteral("Already bound"));
    auto *row = fixture.dock->findChild<QWidget *>(
        QStringLiteral("pelican.liveInputs.learnRow"));
    auto *escape_target = row->findChild<QComboBox *>(
        QStringLiteral("pelican.liveInputs.learnCandidates"));
    fixture.dock->activateWindow();
    escape_target->setFocus();
    QTest::keyClick(escape_target, Qt::Key_Escape);
    application().processEvents();
    CHECK_FALSE(row->isVisible());
}

TEST_CASE("OSC-3d candidate choice survives polling and Show all reaches later controls",
          "[osc3d][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    primeBindings(fixture, Json::array());
    Json recent = Json::array();
    std::uint64_t sequence = 1;
    for (int control = 0; control < 6; ++control) {
        const auto address = QStringLiteral("/fader/%1").arg(control).toStdString();
        recent.push_back(heard(sequence++, address.c_str(), 0.1));
        recent.push_back(heard(sequence++, address.c_str(), 0.2));
    }
    hearForGain(fixture, Json::array(), recent, sequence - 1);
    auto *candidates = fixture.dock->findChild<QComboBox *>(
        QStringLiteral("pelican.liveInputs.learnCandidates"));
    auto *bind = fixture.dock->findChild<QPushButton *>(
        QStringLiteral("pelican.liveInputs.learnBind"));
    auto *row = fixture.dock->findChild<QWidget *>(
        QStringLiteral("pelican.liveInputs.learnRow"));
    REQUIRE(candidates->count() == 7);
    INFO("combo index " << candidates->currentIndex() << ", text "
                        << candidates->currentText().toStdString());
    CHECK(candidates->itemText(6) == QStringLiteral("Show all"));
    CHECK(candidates->currentData().toInt() == -1);
    CHECK_FALSE(bind->isEnabled());
    candidates->setCurrentIndex(4);
    const auto selected = candidates->currentText();
    fixture.dock->readNow();
    replyLive(fixture, fixture.takeRequest(), learnSnapshot(recent, sequence - 1),
              bindingsResult(Json::array()));
    CHECK(candidates->currentText() == selected);
    candidates->setCurrentIndex(6);
    REQUIRE(candidates->count() == 7);
    candidates->setCurrentIndex(6);
    CHECK(candidates->currentData().toInt() == 5);
    CHECK(candidates->currentText().contains(QStringLiteral("/fader/0")));
    CHECK(bind->isEnabled());

    fixture.engine.sent.clear();
    QTest::qWait(160);
    CHECK(fixture.engine.sent.size() == 1);
    replyLive(fixture, fixture.takeRequest(), learnSnapshot(recent, sequence - 1),
              bindingsResult(Json::array()));
    fixture.dock->activateWindow();
    candidates->setFocus();
    QTest::keyClick(candidates, Qt::Key_Escape);
    application().processEvents();
    CHECK_FALSE(row->isVisible());
    fixture.engine.sent.clear();
    QTest::qWait(160);
    CHECK(fixture.engine.sent.isEmpty());
    QTest::qWait(450);
    CHECK(fixture.engine.sent.size() == 1);
}


TEST_CASE("OSC-3d several edits chain Undo digests and external edits clear history",
          "[osc3d][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    const Json first = Json::array({{{"address", "/one"}, {"input", "gain"}},
                                    {{"address", "/two"}, {"input", "steps"}},
                                    {{"address", "/three"}, {"input", "enabled"}}});
    const Json second = Json::array({first[1], first[2]});
    const Json third = Json::array({first[2]});
    primeBindings(fixture, first);
    auto *undo = fixture.dock->findChild<QPushButton *>(
        QStringLiteral("pelican.liveInputs.undoBindings"));

    fixture.clickUnbind(0);
    application().processEvents();
    auto edit = fixture.takeRequest();
    fixture.reply(edit, {{"committed", true}, {"changed", true}, {"digest", "d2"},
                         {"previous", first}, {"applied", second}});
    fixture.reply(fixture.takeRequest(), bindingsResult(second, "d2"));
    auto *remove = fixture.unbind("steps", 0);
    REQUIRE(remove != nullptr);
    CHECK(remove->isEnabled());
    remove->click();
    application().processEvents();
    edit = fixture.takeRequest();
    fixture.reply(edit, {{"committed", true}, {"changed", true}, {"digest", "d3"},
                         {"previous", second}, {"applied", third}});
    fixture.reply(fixture.takeRequest(), bindingsResult(third, "d3"));

    fixture.clickUnbind(0);
    application().processEvents();
    edit = fixture.takeRequest();
    fixture.reply(edit, {{"committed", true}, {"changed", false}, {"digest", "d3"},
                         {"applied", third}});
    CHECK(undo->toolTip() == QStringLiteral("Unbind /two from steps"));

    undo->click();
    application().processEvents();
    edit = fixture.takeRequest();
    CHECK(editParameters(fixture, edit).at("base_digest") == "d3");
    CHECK(editParameters(fixture, edit).at("live") == second);
    fixture.reply(edit, {{"committed", true}, {"changed", true}, {"digest", "d4"},
                         {"previous", third}, {"applied", second}});
    const auto first_undo_read = fixture.takeRequest();
    undo->click();
    application().processEvents();
    edit = fixture.takeRequest();
    CHECK(editParameters(fixture, edit).at("base_digest") == "d4");
    CHECK(editParameters(fixture, edit).at("live") == first);
    fixture.reply(edit, {{"committed", true}, {"changed", true}, {"digest", "d5"},
                         {"previous", second}, {"applied", first}});
    fixture.reply(first_undo_read, bindingsResult(first, "d5"));
    CHECK_FALSE(undo->isEnabled());

    INFO("Unbind after both Undo replies");
    fixture.clickUnbind(0);
    application().processEvents();
    edit = fixture.takeRequest();
    fixture.reply(edit, {{"committed", true}, {"changed", true}, {"digest", "d6"},
                         {"previous", first}, {"applied", second}});
    fixture.reply(fixture.takeRequest(), bindingsResult(second, "d6"));
    fixture.clickUnbind(0);
    application().processEvents();
    edit = fixture.takeRequest();
    CHECK(fixture.dock->receiveRefusal(edit, QStringLiteral("external_modification"),
                                      QStringLiteral("profile changed outside Studio")));
    application().processEvents();
    CHECK_FALSE(undo->isEnabled());
    fixture.reply(fixture.takeRequest(), bindingsResult(third, "disk", second));
    CHECK(fixture.note()->text().contains(QStringLiteral("Not applied: the file changed on disk")));

    fixture.clickUnbind(0);
    application().processEvents();
    edit = fixture.takeRequest();
    fixture.reply(edit, {{"committed", true}, {"changed", true}, {"digest", "d7"},
                         {"previous", third}, {"applied", Json::array()}});
    fixture.reply(fixture.takeRequest(), bindingsResult(Json::array(), "d7"));
    CHECK(undo->isEnabled());
    fixture.dock->readNow();
    auto stopped = learnSnapshot(Json::array(), 40);
    stopped["osc"]["listener"]["state"] = "off";
    stopped["osc"]["listener"]["port"] = 0;
    replyLive(fixture, fixture.takeRequest(), stopped,
              bindingsResult(Json::array(), "d7"));
    CHECK_FALSE(fixture.dock->findChild<QPushButton *>(
                    QStringLiteral("pelican.liveInputs.learn.gain"))->isEnabled());
    CHECK(undo->isEnabled());
    undo->click();
    application().processEvents();
    edit = fixture.takeRequest();
    CHECK(editParameters(fixture, edit).at("base_digest") == "d7");
    CHECK(editParameters(fixture, edit).at("live") == third);
    fixture.reply(edit, {{"committed", true}, {"changed", true}, {"digest", "d8"},
                         {"previous", Json::array()}, {"applied", third}});
    fixture.reply(fixture.takeRequest(), bindingsResult(third, "d8"));
    CHECK_FALSE(undo->isEnabled());
}

TEST_CASE("OSC-3d Learn needs a profile and listener and only one arming request runs",
          "[osc3d][devstudio][live_inputs]")
{
    application();
    Fixture fixture;
    fixture.dock->engineAvailable();
    fixture.reply(fixture.takeRequest(), learnSnapshot(Json::array(), 5));
    fixture.reply(fixture.takeRequest(), {{"profile", nullptr}, {"digest", nullptr},
                                          {"live", Json::array()}, {"applied_live", Json::array()}});
    auto *learn = fixture.dock->findChild<QPushButton *>(
        QStringLiteral("pelican.liveInputs.learn.gain"));
    CHECK_FALSE(learn->isEnabled());
    CHECK(learn->toolTip() == QStringLiteral("Set an input profile for this project first"));

    fixture.dock->engineUnavailable();
    fixture.dock->engineAvailable();
    fixture.reply(fixture.takeRequest(), learnSnapshot(Json::array(), 5));
    fixture.reply(fixture.takeRequest(), bindingsResult(Json::array()));
    application().processEvents();
    learn = qobject_cast<QPushButton *>(
        fixture.inputs()->itemWidget(fixture.inputs()->topLevelItem(0), 5));
    REQUIRE(learn != nullptr);
    REQUIRE(learn->isEnabled());
    learn->click();
    application().processEvents();
    CHECK_FALSE(learn->isEnabled());
    auto *other = fixture.dock->findChild<QPushButton *>(
        QStringLiteral("pelican.liveInputs.learn.steps"));
    CHECK_FALSE(other->isEnabled());
    CHECK(fixture.engine.sent.size() == 1);
    auto *row = fixture.dock->findChild<QWidget *>(
        QStringLiteral("pelican.liveInputs.learnRow"));
    auto *escape_target = row->findChild<QComboBox *>(
        QStringLiteral("pelican.liveInputs.learnCandidates"));
    fixture.dock->activateWindow();
    escape_target->setFocus();
    QTest::keyClick(escape_target, Qt::Key_Escape);
    application().processEvents();
    CHECK_FALSE(row->isVisible());
    CHECK(learn->isEnabled());
}
