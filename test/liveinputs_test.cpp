#include "../src/core/loader/scene.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/os/liveinputs.hpp"
#include "../src/core/renderingpass/graphinputs.hpp"
#include "../src/core/userpublic/valuegraph.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <initializer_list>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace Pelican;
using Catch::Matchers::StartsWith;

namespace {

using Json = nlohmann::json;

constexpr std::string_view validDeclaration = R"({
  "schema": "pelican.live_inputs", "version": 1,
  "inputs": [
    {"name": "intensity", "type": "float", "default": 0.25, "min": 0.0, "max": 1.0},
    {"name": "count", "type": "int", "default": 2, "min": 0, "max": 10},
    {"name": "enabled", "type": "bool", "default": false}
  ]
})";

std::string refusal(std::string_view document) {
    try {
        GET_MODULE(LiveInputs).setDeclarationJson(document);
    } catch (const std::runtime_error &error) {
        return error.what();
    }
    return {};
}

void expectRefusal(std::string_view document, std::string_view code) {
    const auto message = refusal(document);
    INFO(message);
    REQUIRE_THAT(message, StartsWith(std::string{code} + ":"));
}

Json graphDocument(std::initializer_list<std::string> inputs) {
    Json nodes = Json::array();
    Json refs = Json::array();
    std::size_t index = 0;
    for (const auto &name : inputs) {
        const auto id = "input" + std::to_string(index++);
        nodes.push_back(Json{{"id", id}, {"op", "input"}, {"name", name}});
        refs.push_back(id);
    }
    return Json{{"schema", "pelican.value_graph"},
                {"version", 1},
                {"nodes", std::move(nodes)},
                {"outputs", {{"out", std::move(refs)}}}};
}

struct ExplicitStartTime {
    bool previous = GET_MODULE(EngineLaunchConfig).start_time_explicit;
    explicit ExplicitStartTime(bool value) {
        GET_MODULE(EngineLaunchConfig).start_time_explicit = value;
    }
    ~ExplicitStartTime() { GET_MODULE(EngineLaunchConfig).start_time_explicit = previous; }
};

} // namespace

TEST_CASE("OSC-1a liveinputs declaration names valid live inputs and refuses every malformed shape by code",
          "[osc1a][liveinputs]") {
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(validDeclaration);
    REQUIRE(live.isDeclared("intensity"));
    REQUIRE(live.isDeclared("count"));
    REQUIRE(live.isDeclared("enabled"));
    REQUIRE_FALSE(live.isDeclared("missing"));
    const auto intensity = live.declaration("intensity");
    REQUIRE(intensity.has_value());
    REQUIRE(intensity->type == LiveInputType::floating);
    REQUIRE(intensity->default_value == Catch::Approx(0.25));
    REQUIRE(intensity->minimum == Catch::Approx(0.0));
    REQUIRE(intensity->maximum == Catch::Approx(1.0));
    REQUIRE(live.declarations().size() == 3);

    // A project with no declaration is valid and empty.
    live.setDeclarationJson(R"({"schema":"pelican.live_inputs","version":1,"inputs":[]})");
    REQUIRE(live.declarations().empty());

    expectRefusal("not json", "live_input.schema@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":2,"inputs":[]})",
                  "live_input.version@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1.0,"inputs":[]})",
                  "live_input.version@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":18446744073709551615,"inputs":[]})",
                  "live_input.version@1");
    expectRefusal(R"({"schema":"other","version":1,"inputs":[]})", "live_input.schema@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1})", "live_input.inputs@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[3]})",
                  "live_input.declaration@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{}]})",
                  "live_input.name@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"Bad","type":"float","default":0}]})",
                  "live_input.name@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[
        {"name":"a","type":"float","default":0},{"name":"a","type":"float","default":0}]})",
                  "live_input.duplicate@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"a"}]})",
                  "live_input.type@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"a","type":"vec2","default":0}]})",
                  "live_input.type@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"a","type":"trigger","default":false}]})",
                  "live_input.trigger_unsupported@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"a","type":"float"}]})",
                  "live_input.default_required@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"a","type":"float","default":"x"}]})",
                  "live_input.default_type@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"a","type":"int","default":1.5}]})",
                  "live_input.default_type@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"a","type":"int","default":2147483648}]})",
                  "live_input.default_type@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"a","type":"bool","default":0}]})",
                  "live_input.default_type@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"a","type":"bool","default":false,"min":0}]})",
                  "live_input.range@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"a","type":"float","default":0,"min":"x"}]})",
                  "live_input.range@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"a","type":"float","default":0,"min":2,"max":1}]})",
                  "live_input.range@1");
    expectRefusal(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"a","type":"float","default":2,"min":0,"max":1}]})",
                  "live_input.default_range@1");

    // The refused documents left the last valid declaration (the empty one)
    // in place.
    REQUIRE(live.declarations().empty());
}

TEST_CASE("OSC-1a liveinputs staging is typed, atomic, clamped, and only freeze changes the frame snapshot",
          "[osc1a][liveinputs]") {
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(validDeclaration);

    // Before the boundary a staged value is invisible.
    REQUIRE(live.stageValues({{"intensity", 0.5}}) == 1);
    REQUIRE(live.frozenInputs().at("live:intensity") == Catch::Approx(0.25));
    live.freeze(false, 10);
    auto snapshot = live.snapshot();
    REQUIRE(snapshot.at(0).value == Catch::Approx(0.5));
    REQUIRE(snapshot.at(0).from_rpc);
    REQUIRE(snapshot.at(0).changed_frame == 10);

    // The same value again is accepted but did not change the frame.
    REQUIRE(live.stageValues({{"intensity", 0.5}}) == 1);
    live.freeze(false, 11);
    REQUIRE(live.snapshot().at(0).changed_frame == 10);
    REQUIRE(live.snapshot().at(0).value == Catch::Approx(0.5));

    // A changed value stamps the frame it froze on.
    REQUIRE(live.stageValues({{"intensity", 0.75}}) == 1);
    live.freeze(false, 12);
    REQUIRE(live.snapshot().at(0).value == Catch::Approx(0.75));
    REQUIRE(live.snapshot().at(0).changed_frame == 12);

    // Every name and type is validated before any of them commits.
    REQUIRE_THROWS_WITH(live.stageValues({{"intensity", 0.9}, {"missing", 1}}),
                        StartsWith("live_input.unknown@1:"));
    REQUIRE_THROWS_WITH(live.stageValues({{"intensity", 0.9}, {"count", 1.5}}),
                        StartsWith("live_input.type@1:"));
    REQUIRE_THROWS_WITH(live.stageValues({{"enabled", 1}}),
                        StartsWith("live_input.type@1:"));
    REQUIRE_THROWS_WITH(live.stageValues({{"count", std::int64_t{1} << 40}}),
                        StartsWith("live_input.type@1:"));
    live.freeze(false, 13);
    REQUIRE(live.frozenInputs().at("live:intensity") == Catch::Approx(0.75));

    // Integers are integers in the snapshot, booleans are 0/1, and a declared
    // bound clamps and marks the value rather than refusing it.
    REQUIRE(live.stageValues({{"count", 99}, {"enabled", true}}) == 2);
    live.freeze(false, 14);
    snapshot = live.snapshot();
    REQUIRE(snapshot.at(1).value == Catch::Approx(10));
    REQUIRE(snapshot.at(1).clamped);
    REQUIRE(snapshot.at(2).value == Catch::Approx(1));

    // Replay discards pending writes and freezes the defaults; a live write
    // during replay is refused and counted.
    REQUIRE(live.stageValues({{"intensity", 0.1}}) == 1);
    live.freeze(true, 15);
    REQUIRE(live.isReplaying());
    REQUIRE(live.snapshot().at(0).value == Catch::Approx(0.25));
    REQUIRE_THROWS_WITH(live.stageValues({{"intensity", 0.2}}),
                        StartsWith("live_input.replay@1:"));
    REQUIRE(live.droppedDuringReplay() == 1);
    live.freeze(false, 16);
    REQUIRE_FALSE(live.isReplaying());
    REQUIRE(live.snapshot().at(0).value == Catch::Approx(0.25));
}

TEST_CASE("OSC-1a liveinputs graph evaluation reads the frozen snapshot and refuses undeclared live names",
          "[osc1a][liveinputs]") {
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(R"({"schema":"pelican.live_inputs","version":1,"inputs":[
        {"name":"a","type":"float","default":0.25},
        {"name":"b","type":"float","default":0.25}]})");

    const ValueGraph::Graph graph{graphDocument({"live:a", "live:b"}).dump()};
    validateGraphInputs(graph);
    REQUIRE(live.graphReadsLive());
    REQUIRE(live.firstLiveRead() == "live:a");

    REQUIRE(live.stageValues({{"a", 1.5}, {"b", -2.0}}) == 2);
    live.freeze(false, 5);
    auto &scene = GET_MODULE(SceneLoader);
    const auto inputs = collectGraphInputs(graph, scene, {}, 2.0, 1.0 / 60.0);
    REQUIRE(inputs.at("live:a") == Catch::Approx(1.5));
    REQUIRE(inputs.at("live:b") == Catch::Approx(-2.0));
    REQUIRE(graph.evaluate(inputs).at("out") == std::vector<double>{1.5, -2.0});

    // Values staged but not frozen are invisible to every evaluation of the
    // frame -- mutation 1 would read them here.
    REQUIRE(live.stageValues({{"a", 9.0}}) == 1);
    const auto again = collectGraphInputs(graph, scene, {}, 2.0, 1.0 / 60.0);
    REQUIRE(again.at("live:a") == Catch::Approx(1.5));
    live.freeze(false, 6);
    REQUIRE(collectGraphInputs(graph, scene, {}, 2.0, 1.0 / 60.0).at("live:a") ==
            Catch::Approx(9.0));

    // An undeclared live name is a named refusal, not the old generic one --
    // mutation 2 would accept it and fail later.
    const ValueGraph::Graph unknown{graphDocument({"live:missing"}).dump()};
    REQUIRE_THROWS_WITH(validateGraphInputs(unknown), StartsWith("live_input.unknown@1:"));
    REQUIRE_THROWS_WITH(collectGraphInputs(unknown, scene, {}, 0.0, 0.0),
                        StartsWith("live_input.unknown@1:"));

    // A non-live unknown name keeps the old refusal.
    const ValueGraph::Graph oldStyle{graphDocument({"external"}).dump()};
    REQUIRE_THROWS_WITH(validateGraphInputs(oldStyle), StartsWith("unbound frame graph input: external"));
}

TEST_CASE("OSC-1a liveinputs deterministic offline output that reads a live input is refused",
          "[osc1a][liveinputs]") {
    GET_MODULE(LiveInputs).setDeclarationJson(R"({"schema":"pelican.live_inputs","version":1,"inputs":[
        {"name":"a","type":"float","default":0.0}]})");
    const ValueGraph::Graph readsLive{graphDocument({"live:a"}).dump()};
    const ValueGraph::Graph plain{graphDocument({"time"}).dump()};

    {
        ExplicitStartTime offline{true};
        REQUIRE_THROWS_WITH(validateGraphInputs(readsLive),
                            StartsWith("offline.live_input_unrecorded@1:"));
        // A declaration alone does not make the output depend on it.
        REQUIRE_NOTHROW(validateGraphInputs(plain));
        auto &scene = GET_MODULE(SceneLoader);
        REQUIRE_THROWS_WITH(collectGraphInputs(readsLive, scene, {}, 0.0, 0.0),
                            StartsWith("offline.live_input_unrecorded@1:"));
        REQUIRE_NOTHROW(collectGraphInputs(plain, scene, {}, 0.0, 0.0));
    }
    // The same project is ordinary interactive output without an offline flag.
    REQUIRE_NOTHROW(validateGraphInputs(readsLive));
}
