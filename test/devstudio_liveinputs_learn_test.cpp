#include "liveinputslearn.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

using PelicanStudio::LiveInputsLearn;
using Json = nlohmann::json;

namespace {

Json heard(std::uint64_t sequence, const char *address = "/fader/1",
           const char *sender = "127.0.0.1:5000", const char *type = "f",
           double value = 0.25)
{
    return {{"sequence", sequence}, {"address", address}, {"sender", sender},
            {"arg_type", type}, {"value", value}};
}

} // namespace

TEST_CASE("OSC-3d learn only counts fresh, sorted, unduplicated messages", "[osc3d]")
{
    LiveInputsLearn learn;
    learn.start("a", "float", 100);
    CHECK(learn.state() == LiveInputsLearn::State::arming);
    learn.arm(10, 100);
    learn.receive(Json::array({heard(12, "/new", "s", "f", 0.5),
                               heard(10, "/before-learn", "s", "f", 0.75),
                               heard(11, "/new", "s", "f", 0.25)}), 200);
    REQUIRE(learn.candidates().size() == 1);
    CHECK(learn.candidates()[0].confident);
    CHECK(learn.candidates()[0].count == 2);
    CHECK(learn.candidates()[0].value == 0.5);
    learn.receive(Json::array({heard(10), heard(11, "/new", "s", "f", 0.25),
                               heard(12, "/new", "s", "f", 0.5)}), 201);
    CHECK(learn.candidates()[0].count == 2);
}

TEST_CASE("OSC-3d learn compatibility is the receiving conversion", "[osc3d]")
{
    LiveInputsLearn floating;
    floating.start("a", "float", 0, 0);
    floating.receive(Json::array({heard(1, "/f", "s", "f", 0.2),
                                  heard(2, "/i", "s", "i", 2)}), 1);
    CHECK(floating.candidates().size() == 2);
    CHECK(floating.candidates()[0].distinct_values == 1);
    CHECK_FALSE(floating.candidates()[0].confident);

    LiveInputsLearn integer;
    integer.start("a", "int", 0, 0);
    integer.receive(Json::array({heard(1, "/fraction", "s", "f", 2.49),
                                 heard(2, "/large", "s", "f", 1e30),
                                 heard(3, "/truth", "s", "T", 1)}), 1);
    REQUIRE(integer.candidates().size() == 1);
    CHECK(integer.candidates()[0].address == "/fraction");

    LiveInputsLearn boolean;
    boolean.start("a", "bool", 0, 0);
    boolean.receive(Json::array({heard(1, "/touch", "s", "T", 1)}), 1);
    REQUIRE(boolean.candidates().size() == 1);
    CHECK(boolean.candidates()[0].confident);
    boolean.receive(Json::array({heard(2, "/off", "s", "F", 0),
                                 heard(3, "/two", "s", "i", 2),
                                 heard(4, "/one", "s", "i", 1)}), 2);
    CHECK(boolean.candidates().size() == 3);
}

TEST_CASE("OSC-3d a heartbeat is not a candidate and senders stay separate", "[osc3d]")
{
    LiveInputsLearn learn;
    learn.start("a", "float", 0, 0);
    learn.receive(Json::array({heard(1)}), 10);
    REQUIRE(learn.candidates().size() == 1);
    CHECK(learn.candidates()[0].count == 1);
    CHECK_FALSE(learn.candidates()[0].confident);
    learn.receive(Json::array({heard(1), heard(2), heard(3)}), 20);
    CHECK(learn.candidates().empty());
    learn.receive(Json::array({heard(3), heard(4, "/fader/1", "127.0.0.1:5000", "f", 0.75),
                               heard(5, "/fader/1", "other", "f", 0.25)}), 30);
    REQUIRE(learn.candidates().size() == 2);
    const auto original = std::find_if(learn.candidates().begin(), learn.candidates().end(),
                                       [](const auto &candidate) { return candidate.sender == "127.0.0.1:5000"; });
    REQUIRE(original != learn.candidates().end());
    CHECK(original->confident);
    CHECK(learn.candidates()[0].address == learn.candidates()[1].address);
    CHECK(learn.candidates()[0].sender != learn.candidates()[1].sender);
}

TEST_CASE("OSC-3d candidates rank by distinct values then recency", "[osc3d]")
{
    LiveInputsLearn learn;
    learn.start("a", "float", 0, 0);
    learn.receive(Json::array({heard(1, "/two", "s", "f", 0.1),
                               heard(2, "/two", "s", "f", 0.2),
                               heard(3, "/recent", "s", "f", 0.3),
                               heard(4, "/two", "s", "f", 0.4)}), 10);
    REQUIRE(learn.candidates().size() == 2);
    CHECK(learn.candidates()[0].address == "/two");
    learn.receive(Json::array({heard(4, "/two", "s", "f", 0.4),
                               heard(5, "/recent", "s", "f", 0.6)}), 20);
    CHECK(learn.candidates()[0].address == "/two");
    learn.receive(Json::array({heard(5, "/recent", "s", "f", 0.6),
                               heard(6, "/recent", "s", "f", 0.9)}), 30);
    CHECK(learn.candidates()[0].address == "/recent");
}

TEST_CASE("OSC-3d learn timeout, cancel, ring boundary and reset", "[osc3d]")
{
    LiveInputsLearn learn;
    learn.start("a", "float", 0, 0);
    Json burst = Json::array();
    for (std::uint64_t sequence = 1; sequence <= 64; ++sequence)
        burst.push_back(heard(sequence, "/fader/1", "s", "f", sequence / 100.0));
    learn.receive(burst, 1);
    CHECK_FALSE(learn.gapWarning());
    learn.cancel();
    CHECK(learn.state() == LiveInputsLearn::State::idle);
    learn.start("a", "float", 0, 0);
    learn.receive(Json::array({heard(2), heard(65, "/fader/1", "s", "f", 0.5)}), 1);
    CHECK(learn.gapWarning());
    learn.receive(Json::array({heard(1)}), 2);
    CHECK(learn.state() == LiveInputsLearn::State::idle);
    learn.start("a", "float", 0, 0);
    learn.receive(Json::array(), LiveInputsLearn::timeout_ms);
    CHECK(learn.state() == LiveInputsLearn::State::timed_out);
}
