#include "../src/core/os/inputsequence.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fstream>

using namespace Pelican;

TEST_CASE("pelican.input_seq v1 round trips every WP89 input event kind deterministically",
          "[input-sequence]") {
    InputStateCore input;
    input.queueButtonEvent(KeyCode::W, true);
    input.queueCursorMove(12.5f, -3.25f);
    input.queueAxisEvent(1.0f, -2.0f);
    input.queueEvent(InputEvent::scroll(0.5f, 4.0f));
    input.queueEvent(InputEvent::character(0x3042));
    input.beginFrame();

    InputSequence sequence{30.0};
    sequence.appendFrame(input.currentFrameInput().ordered_events);
    sequence.appendFrame(std::span<const InputEvent>{});

    const auto first = sequence.toJsonLines();
    const auto parsed = InputSequence::fromJsonLines(first);
    const auto second = parsed.toJsonLines();
    REQUIRE(second == first);
    REQUIRE(parsed.fps() == 30.0);
    REQUIRE(parsed.frames().size() == 2);
    REQUIRE(parsed.frames()[0].events.size() == 5);
    REQUIRE(parsed.frames()[0].events[0].event_seq == 0);
    REQUIRE(parsed.frames()[0].events[0].code == KeyCode::W);
    REQUIRE(parsed.frames()[0].events[4].codepoint == 0x3042);
    REQUIRE(parsed.frames()[1].events.empty());
}

TEST_CASE("input_seq rejects non-monotonic event_seq and missing frame markers", "[input-sequence]") {
    const std::string header = R"({"schema":"pelican.input_seq","version":1,"fps":60})";
    REQUIRE_THROWS_WITH(InputSequence::fromJsonLines(
                            header + "\n{\"event_seq\":0,\"type\":\"button\",\"code\":\"W\",\"pressed\":true}\n"),
                        "input_seq event appears before the first frame marker");

    const auto duplicate = header +
                           "\n{\"frame\":0}\n"
                           "{\"event_seq\":2,\"type\":\"button\",\"code\":\"W\",\"pressed\":true}\n"
                           "{\"event_seq\":2,\"type\":\"button\",\"code\":\"W\",\"pressed\":false}\n";
    REQUIRE_THROWS_WITH(InputSequence::fromJsonLines(duplicate),
                        "input_seq event_seq must be strictly increasing");
}

TEST_CASE("replay reconstructs held and released snapshots through InputState", "[input-sequence]") {
    const auto text =
        "{\"schema\":\"pelican.input_seq\",\"version\":1,\"fps\":60}\n"
        "{\"frame\":0}\n"
        "{\"event_seq\":0,\"type\":\"button\",\"code\":\"W\",\"pressed\":true}\n"
        "{\"frame\":1}\n"
        "{\"frame\":2}\n"
        "{\"event_seq\":1,\"type\":\"button\",\"code\":\"W\",\"pressed\":false}\n";

    const auto temp = std::filesystem::temp_directory_path() / "pelican_wp89_inputsequence_test.jsonl";
    {
        std::ofstream output{temp, std::ios::binary | std::ios::trunc};
        output << text;
    }

    InputSequenceRuntime runtime;
    InputState input;
    runtime.startReplay(temp);

    runtime.prepareFrame(input);
    input.beginFrame();
    REQUIRE(input.currentSnapshot().isKeyPushed(KeyCode::W));
    REQUIRE(input.currentSnapshot().getKey(KeyCode::W));

    runtime.prepareFrame(input);
    input.beginFrame();
    REQUIRE_FALSE(input.currentSnapshot().isKeyPushed(KeyCode::W));
    REQUIRE(input.currentSnapshot().getKey(KeyCode::W));

    runtime.prepareFrame(input);
    input.beginFrame();
    REQUIRE(input.currentSnapshot().isKeyReleased(KeyCode::W));
    REQUIRE_FALSE(input.currentSnapshot().getKey(KeyCode::W));
    REQUIRE(runtime.replayComplete());

    runtime.stopReplay();
    std::filesystem::remove(temp);
}
