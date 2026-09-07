#pragma once
#include <nlohmann/json.hpp>

inline constexpr auto graphCompositionFixture = R"({"schema":"pelican.graph","version":1,
  "definitions":{"scaled_curve":{
    "nodes":[
      {"id":"time","op":"graph.input"},
      {"id":"gain","op":"graph.input"},
      {"id":"shape","op":"value.curve","inputs":{"time":{"node":"time","port":"value"}},
       "parameters":{"keys":[{"time":0,"value":0},{"time":1,"value":1}]},"outputs":["value"]},
      {"id":"scaled","op":"value.multiply","inputs":{"a":{"node":"shape","port":"value"},
       "b":{"node":"gain","port":"value"}},"outputs":["value"]}],
    "outputs":{"result":{"node":"scaled","port":"value"}}}},
  "nodes":[
    {"id":"clock","op":"value.input","parameters":{"name":"time"},"outputs":["value"]},
    {"id":"two","op":"value.constant","parameters":{"value":2},"outputs":["value"]},
    {"id":"four","op":"value.constant","parameters":{"value":4},"outputs":["value"]},
    {"id":"left","op":"graph.instance","definition":"scaled_curve","inputs":{
      "time":{"node":"clock","port":"value"},"gain":{"node":"two","port":"value"}}},
    {"id":"right","op":"graph.instance","definition":"scaled_curve","inputs":{
      "time":{"node":"clock","port":"value"},"gain":{"node":"four","port":"value"}}},
    {"id":"render","op":"render.scalar","inputs":{"value":{"node":"left","port":"result"}},
      "parameters":{"target":"scoped_value"},"outputs":[]}],
  "outputs":{"left":{"node":"left","port":"result"},"right":{"node":"right","port":"result"}}})";

inline nlohmann::json mixedGraphCompositionFixture() {
    using Json = nlohmann::json;
    auto document = Json::parse(graphCompositionFixture);
    auto &buffer = document["nodes"][5];
    buffer["op"] = "render.buffer";
    buffer["parameters"] = {{"element", "float"}};
    buffer["outputs"] = {"resource"};
    document["nodes"].push_back({{"id", "result"}, {"op", "render.buffer"},
        {"parameters", {{"size", 4}, {"element", "float"}, {"host_source", "runtime_f32"}}},
        {"outputs", {"resource"}}});
    document["nodes"].push_back({{"id", "probe"}, {"op", "render.compute"},
        {"parameters", {{"shader", "shaders/graph_probe"}, {"dispatch", {{"groups", {1, 1, 1}}}},
            {"ports", {{"source", {{"element", "float"}, {"access", "read"}}},
                       {"destination", {{"element", "float"}, {"access", "write"}}}}}}},
        {"inputs", {{"source", {{"node", "render"}, {"port", "resource"}}},
                    {"destination", {{"node", "result"}, {"port", "resource"}}}}},
        {"outputs", {"destination"}}});
    document["outputs"]["gpu"] = {{"node", "probe"}, {"port", "destination"}};
    document["rendering"] = {{"render_targets", Json::array()}, {"rendering_passes", Json::array()}};
    return document;
}

inline nlohmann::json parameterGraphCompositionFixture() {
    auto document = mixedGraphCompositionFixture();
    document["definitions"]["scaled_curve"]["nodes"][1]["default"] = 1;
    for (const auto index : {3, 4}) document["nodes"][index]["inputs"].erase("gain");
    document["nodes"][3]["parameters"] = {{"gain", 2}};
    document["nodes"][4]["parameters"] = {{"gain", 4}};
    return document;
}

inline void addGraphTriggerClock(nlohmann::json &document) {
    auto &nodes = document["definitions"]["scaled_curve"]["nodes"];
    nodes[2]["inputs"]["time"] = {{"node", "playhead"}, {"port", "value"}};
    for (const auto *name : {"fire", "paused", "reset"})
        nodes.push_back({{"id", name}, {"op", "graph.input"}, {"default", 0}});
    nodes.push_back({{"id", "playhead"}, {"op", "value.trigger_clock"},
        {"inputs", {{"time", {{"node", "time"}, {"port", "value"}}},
                    {"trigger", {{"node", "fire"}, {"port", "value"}}},
                    {"paused", {{"node", "paused"}, {"port", "value"}}},
                    {"reset", {{"node", "reset"}, {"port", "value"}}}}}, {"outputs", {"value"}}});
}

inline void addGraphImageStage(nlohmann::json &document) {
    const nlohmann::json type{{"semantic", "pelican.render.color_signal@1"},
        {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
    document["nodes"].push_back({{"id", "intermediate"}, {"op", "render.image"},
        {"parameters", {{"type", type}, {"extent_scale", 1.0}, {"format", "R32G32B32A32_SFLOAT"},
                        {"usage", {"STORAGE", "SAMPLED"}}}}, {"outputs", {"resource"}}});
    auto &writer = document["nodes"][7];
    writer["parameters"]["shader"] = "shaders/graph_image_write";
    writer["parameters"]["ports"]["destination"] = {{"kind", "image"}, {"type", type}, {"access", "write"}};
    writer["inputs"]["destination"] = {{"node", "intermediate"}, {"port", "resource"}};
    document["nodes"].push_back({{"id", "image_probe"}, {"op", "render.compute"},
        {"parameters", {{"shader", "shaders/graph_image_read"}, {"dispatch", {{"groups", {1, 1, 1}}}},
          {"ports", {{"source", {{"kind", "image"}, {"type", type}, {"access", "read"}}},
                     {"destination", {{"element", "float"}, {"access", "write"}}}}}}},
        {"inputs", {{"source", {{"node", "probe"}, {"port", "destination"}}},
                    {"destination", {{"node", "result"}, {"port", "resource"}}}}},
        {"outputs", {"destination"}}});
    document["outputs"]["gpu"] = {{"node", "image_probe"}, {"port", "destination"}};
}
