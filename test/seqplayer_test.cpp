#include "../src/core/playback/seqplayer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace Pelican {

namespace {

constexpr auto twoObjectSequence = R"jsonl(
{"schema": "pelican.transform_seq", "version": 1, "fps": 2.0, "objects": ["a", "b"]}
{"t": 0.0, "transforms": [{"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]}, {"pos": [1,0,0], "rot": [0,0,0,1], "scale": [1,1,1]}]}
{"t": 0.5, "hidden": [1], "transforms": [{"pos": [0,1,0], "rot": [0,0,0,1], "scale": [2,2,2]}, {"pos": [1,1,0], "rot": [0,0.7071068,0,0.7071068], "scale": [3,3,3]}]}
{"t": 1.0, "transforms": [{"pos": [0,2,0], "rot": [0,0,0,1], "scale": [1,1,1]}, {"pos": [1,2,0], "rot": [0,0,0,1], "scale": [1,1,1]}]}
)jsonl";

} // namespace

TEST_CASE("transform_seq parser reads header, frames, and hidden indexes", "[seqplayer]") {
    const auto sequence = TransformSequence::fromJsonLines(twoObjectSequence);

    REQUIRE(sequence.fps() == Catch::Approx(2.0));
    REQUIRE(sequence.objects().size() == 2);
    REQUIRE(sequence.objects()[0] == "a");
    REQUIRE(sequence.objects()[1] == "b");
    REQUIRE(sequence.frames().size() == 3);

    const auto &frame = sequence.frames()[1];
    REQUIRE(frame.time == Catch::Approx(0.5));
    REQUIRE(frame.isHidden(1));
    REQUIRE_FALSE(frame.isHidden(0));
    REQUIRE(frame.transforms[0].pos.y == Catch::Approx(1.0f));
    REQUIRE(frame.transforms[1].rotation.w == Catch::Approx(0.7071068f));
    REQUIRE(frame.transforms[1].rotation.y == Catch::Approx(0.7071068f));
}

TEST_CASE("transform_seq sampler floors, clamps, and loops", "[seqplayer]") {
    const auto sequence = TransformSequence::fromJsonLines(twoObjectSequence);

    REQUIRE(sequence.sampleIndex(0.0, false) == 0);
    REQUIRE(sequence.sampleIndex(0.49, false) == 0);
    REQUIRE(sequence.sampleIndex(0.5, false) == 1);
    REQUIRE(sequence.sampleIndex(50.0, false) == 2);
    REQUIRE(sequence.sampleIndex(1.5, true) == 0);
}

TEST_CASE("transform_seq parser rejects malformed files", "[seqplayer]") {
    REQUIRE_THROWS(TransformSequence::fromJsonLines(
        R"jsonl({"schema":"pelican.transform_seq","version":2,"fps":30,"objects":["a"]})jsonl"));
    REQUIRE_THROWS(TransformSequence::fromJsonLines(
        R"jsonl({"schema":"pelican.transform_seq","version":1,"fps":0,"objects":["a"]})jsonl"));
    REQUIRE_THROWS(TransformSequence::fromJsonLines(R"jsonl(
{"schema":"pelican.transform_seq","version":1,"fps":30,"objects":["a"]}
{"t":0,"hidden":[2],"transforms":[{"pos":[0,0,0],"rot":[0,0,0,1],"scale":[1,1,1]}]}
)jsonl"));
}

} // namespace Pelican
