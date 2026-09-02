#include "../src/core/userpublic/details/event/registerer.hpp"
#include "../src/core/userpublic/gamesystem.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

struct Wp56ProbeEvent {
    int value = 0;

    template <class T> void ref(T &ar) {
        ar.prop("value", value);
    }
};

struct Wp56ReentrantEvent {
    int value = 0;

    template <class T> void ref(T &ar) {
        ar.prop("value", value);
    }
};

std::vector<std::string> event_records;

struct Wp56EventOrderProbeSystem {
    void onEvent(const Wp56ProbeEvent &event, Pelican::GameContext &ctx) {
        event_records.push_back("probe:" + std::to_string(event.value));
        if (event.value == 2) {
            ctx.emit(Wp56ReentrantEvent{20});
        }
    }

    void onEvent(const Wp56ReentrantEvent &event, Pelican::GameContext &) {
        event_records.push_back("reentrant:" + std::to_string(event.value));
    }
};

} // namespace

PELICAN_REGISTER_EVENT(Wp56ProbeEvent);
PELICAN_REGISTER_EVENT(Wp56ReentrantEvent);
PELICAN_REGISTER_SYSTEM(Wp56EventOrderProbeSystem, 50);

TEST_CASE("Event bus dispatches at frame boundary in emit order", "[event-layer]") {
    Pelican::internal::clearPendingEvents();
    event_records.clear();

    Pelican::GameContext ctx;
    ctx.emit(Wp56ProbeEvent{1});
    ctx.emit(Wp56ProbeEvent{2});

    REQUIRE(event_records.empty());

    Pelican::internal::freezePendingEventsForFrame();
    Pelican::internal::dispatchFrozenEvents(ctx);
    REQUIRE(event_records == std::vector<std::string>{"probe:1", "probe:2"});

    Pelican::internal::freezePendingEventsForFrame();
    Pelican::internal::dispatchFrozenEvents(ctx);
    REQUIRE(event_records == std::vector<std::string>{"probe:1", "probe:2", "reentrant:20"});

    Pelican::internal::clearPendingEvents();
}

TEST_CASE("Event bus freeze is separate from delivery", "[event-layer][frame-phase]") {
    Pelican::internal::clearPendingEvents();
    event_records.clear();

    Pelican::GameContext ctx;
    ctx.emit(Wp56ProbeEvent{1});
    Pelican::internal::freezePendingEventsForFrame();
    ctx.emit(Wp56ProbeEvent{3});
    Pelican::internal::dispatchFrozenEvents(ctx);
    REQUIRE(event_records == std::vector<std::string>{"probe:1"});

    Pelican::internal::freezePendingEventsForFrame();
    Pelican::internal::dispatchFrozenEvents(ctx);
    REQUIRE(event_records == std::vector<std::string>{"probe:1", "probe:3"});

    Pelican::internal::clearPendingEvents();
}

TEST_CASE("WP380 event outbox is invisible until its preallocated noexcept flush",
          "[WP380][transaction][event-outbox]") {
    auto &events = Pelican::internal::getEventRegisterer();
    Pelican::internal::clearPendingEvents();

    {
        Pelican::internal::PreparedEventOutbox discarded{events, 1};
        auto reserved = discarded.prepare(Wp56ProbeEvent{380});
        {
            Pelican::internal::EventOutboxScope scope{discarded};
            events.emit(Wp56ProbeEvent{381});
            REQUIRE(events.pendingEventCount() == 0);
            REQUIRE(discarded.size() == 1);
        }
        discarded.stageReserved(std::move(reserved));
        REQUIRE(events.pendingEventCount() == 0);
    }
    REQUIRE(events.pendingEventCount() == 0);

    {
        Pelican::internal::PreparedEventOutbox committed{events, 1};
        auto reserved = committed.prepare(Wp56ProbeEvent{382});
        {
            Pelican::internal::EventOutboxScope scope{committed};
            events.emit(Wp56ProbeEvent{383});
        }
        committed.stageReserved(std::move(reserved));
        committed.flush();
    }
    const auto snapshot = events.isolationSnapshot();
    REQUIRE(snapshot.pending.size() == 2);
    REQUIRE(snapshot.pending[0].name == "Wp56ProbeEvent");
    REQUIRE(snapshot.pending[1].name == "Wp56ProbeEvent");
    Pelican::internal::clearPendingEvents();
}
