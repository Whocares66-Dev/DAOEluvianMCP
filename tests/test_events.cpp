#include "core/EventBus.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace eluvian;
using namespace std::chrono_literals;

TEST_CASE("events are numbered from 1 and stamped with the frame", "[events]")
{
    std::uint64_t frame = 41;
    EventBus bus(16, [&frame] { return ++frame; });
    CHECK(bus.Next() == 1);
    CHECK(bus.Emit("a", Json::object()) == 1);
    CHECK(bus.Emit("b", Json::object()) == 2);
    const auto page = bus.Since(0, {}, 10);
    REQUIRE(page.events.size() == 2);
    CHECK(page.events[0].frame == 42);
    CHECK(page.events[1].topic == "b");
    CHECK(page.next == 3);
    CHECK(page.missed == 0);
}

TEST_CASE("a read from a cursor returns what came after and where to go on", "[events]")
{
    EventBus bus;
    for (int i = 0; i < 5; ++i)
    {
        bus.Emit("tick", Json{{"i", i}});
    }
    auto page = bus.Since(4, {}, 10);
    REQUIRE(page.events.size() == 2);
    CHECK(page.events[0].payload["i"] == 3);
    CHECK(page.next == 6);

    page = bus.Since(page.next, {}, 10);
    CHECK(page.events.empty());
    CHECK(page.next == 6);
}

TEST_CASE("a limit truncates and the cursor resumes at the first not returned", "[events]")
{
    EventBus bus;
    for (int i = 0; i < 5; ++i)
    {
        bus.Emit(i % 2 == 0 ? "even" : "odd", Json{{"i", i}});
    }
    EventFilter even;
    even.topics = {"even"};
    auto page = bus.Since(0, even, 2);
    REQUIRE(page.events.size() == 2);
    CHECK(page.truncated);
    CHECK(page.next == 5); // the third even event, seq 5, is next
    page = bus.Since(page.next, even, 2);
    REQUIRE(page.events.size() == 1);
    CHECK(page.events[0].payload["i"] == 4);
    CHECK_FALSE(page.truncated);
}

TEST_CASE("the buffer drops the oldest, and a stale cursor learns how many it missed", "[events]")
{
    EventBus bus(3);
    for (int i = 0; i < 5; ++i)
    {
        bus.Emit("tick", Json{{"i", i}});
    }
    const auto page = bus.Since(1, {}, 10);
    CHECK(page.missed == 2);
    REQUIRE(page.events.size() == 3);
    CHECK(page.events[0].seq == 3);
}

TEST_CASE("topics match by name, by prefix and by star", "[events]")
{
    Event turn{1, 0, 0, "turnbased.turn.ended", Json::object()};
    EventFilter filter;
    CHECK(filter.Accepts(turn));
    filter.topics = {"turnbased.*"};
    CHECK(filter.Accepts(turn));
    filter.topics = {"turnbased.turn.ended"};
    CHECK(filter.Accepts(turn));
    filter.topics = {"turnbased.turn"};
    CHECK_FALSE(filter.Accepts(turn));
    filter.topics = {"turnbasedx.*"};
    CHECK_FALSE(filter.Accepts(turn));
    filter.topics = {"turnbased.*"}; // a prefix ends at its dot: another mod's name that begins the same is not it
    CHECK_FALSE(filter.Accepts(Event{2, 0, 0, "turnbasedx.turn.ended", Json::object()}));
    filter.topics = {"eluvian.*", "*"};
    CHECK(filter.Accepts(turn));
}

TEST_CASE("match compares payload fields, objects as subsets", "[events]")
{
    Event died{1, 0, 0, "eluvian.creature.died",
               Json{{"creature", {{"id", "0x7d9"}, {"name", "Howe Archer"}}}, {"round", 4}}};
    EventFilter filter;
    filter.match = Json{{"creature", {{"id", "0x7d9"}}}};
    CHECK(filter.Accepts(died));
    filter.match = Json{{"creature", {{"id", "0x7d1"}}}};
    CHECK_FALSE(filter.Accepts(died));
    filter.match = Json{{"round", 4}};
    CHECK(filter.Accepts(died));
    filter.match = Json{{"round", "4"}};
    CHECK_FALSE(filter.Accepts(died));
    filter.match = Json{{"missing", 1}};
    CHECK_FALSE(filter.Accepts(died));
    filter.match = Json{{"creature", {{"id", "0x7d9"}}}}; // an object does not match a field that is no object
    CHECK_FALSE(filter.Accepts(Event{2, 0, 0, "eluvian.creature.died", Json{{"creature", "0x7d9"}}}));
}

TEST_CASE("filters read from a tool's arguments, and say what is wrong", "[events]")
{
    auto filter = EventFilter::FromArgs(Json{{"topic", Json::array({"a", "b.*"})}, {"match", {{"id", "0x1"}}}});
    CHECK(filter.topics.size() == 2);
    CHECK(filter.match["id"] == "0x1");
    CHECK_THROWS_AS(EventFilter::FromArgs(Json{{"topic", 3}}), ToolError);
    CHECK_THROWS_AS(EventFilter::FromArgs(Json{{"topic", Json::array({"a", 1})}}), ToolError);
    CHECK_THROWS_AS(EventFilter::FromArgs(Json{{"match", "id"}}), ToolError);
    CHECK(EventFilter::FromArgs(Json{{"topic", nullptr}}).topics.empty());
}

TEST_CASE("WaitFor finds an event that came before the wait, from the cursor", "[events]")
{
    EventBus bus;
    const auto cursor = bus.Next();
    bus.Emit("turnbased.turn.ended", Json{{"id", "0x82"}});
    EventFilter filter;
    filter.topics = {"turnbased.turn.ended"};
    const auto event = bus.WaitFor(cursor, filter, 0ms);
    REQUIRE(event);
    CHECK(event->payload["id"] == "0x82");
}

TEST_CASE("WaitFor wakes for an event emitted while it waits", "[events]")
{
    EventBus bus;
    const auto cursor = bus.Next();
    std::thread emitter([&bus] {
        std::this_thread::sleep_for(20ms);
        bus.Emit("noise", Json::object());
        std::this_thread::sleep_for(20ms);
        bus.Emit("turnbased.fight.over", Json{{"reason", "no enemy left standing"}});
    });
    EventFilter filter;
    filter.topics = {"turnbased.fight.*"};
    const auto event = bus.WaitFor(cursor, filter, 5s);
    emitter.join();
    REQUIRE(event);
    CHECK(event->topic == "turnbased.fight.over");
}

TEST_CASE("WaitFor gives up at its timeout, on Close, and when told to stop", "[events]")
{
    EventBus bus;
    EventFilter never;
    never.topics = {"never"};
    CHECK_FALSE(bus.WaitFor(bus.Next(), never, 30ms));

    std::atomic<bool> keepWaiting{false};
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(bus.WaitFor(bus.Next(), never, 5s, &keepWaiting));
    CHECK(std::chrono::steady_clock::now() - start < 1s);

    std::thread closer([&bus] {
        std::this_thread::sleep_for(20ms);
        bus.Close();
    });
    const auto closing = std::chrono::steady_clock::now();
    CHECK_FALSE(bus.WaitFor(bus.Next(), never, 30s));
    CHECK(std::chrono::steady_clock::now() - closing < 10s); // woken by the Close, not the timeout
    closer.join();
}
