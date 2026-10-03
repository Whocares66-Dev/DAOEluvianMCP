#include "core/EventLog.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace eluvian;

namespace
{

// A name of this run's own: the mutants run many copies of the tests at
// once, and with one name for all they opened each other's files
// (tools/mutants.py, 2026-10-03).
std::filesystem::path TempFile(const char *name)
{
    return std::filesystem::temp_directory_path() / (std::to_string(std::random_device{}()) + "-" + name);
}

std::vector<Json> Lines(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<Json> lines;
    std::string line;
    while (std::getline(in, line))
    {
        lines.push_back(Json::parse(line));
    }
    return lines;
}

} // namespace

TEST_CASE("every event the bus emits is a line in the file, in the bus's order", "[event-log]")
{
    const auto path = TempFile("eluvian-test-events.jsonl");
    {
        EventLog log(path);
        REQUIRE(log.Open());
        EventBus bus(2); // a buffer of two: the file keeps what the buffer forgets
        bus.SetRecorder([&log](const Event &event) { log.Write(event); });
        bus.Emit("eluvian.creature.died", Json{{"id", "0x3bf"}});
        bus.Emit("turnbased.turn.ended", Json{{"round", 1}});
        bus.Emit("eluvian.mode.changed", Json{{"mode", "explore"}});
    }
    const auto lines = Lines(path);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0]["seq"] == 1);
    CHECK(lines[0]["topic"] == "eluvian.creature.died");
    CHECK(lines[0]["payload"]["id"] == "0x3bf");
    CHECK(lines[2]["topic"] == "eluvian.mode.changed");
    std::filesystem::remove(path);
}

TEST_CASE("an event is in the file as it is written, before the file closes", "[event-log]")
{
    const auto path = TempFile("eluvian-test-events-through.jsonl");
    {
        EventLog log(path);
        EventBus bus;
        bus.SetRecorder([&log](const Event &event) { log.Write(event); });
        bus.Emit("eluvian.creature.died", Json{{"id", "0x3bf"}});
        // Read while the game would still be running: a crash now keeps it.
        const auto lines = Lines(path);
        REQUIRE(lines.size() == 1);
        CHECK(lines[0]["topic"] == "eluvian.creature.died");
    }
    std::filesystem::remove(path);
}

TEST_CASE("a file at its most says so once and keeps nothing after", "[event-log]")
{
    const auto path = TempFile("eluvian-test-events-full.jsonl");
    {
        EventLog log(path, 200);
        EventBus bus;
        bus.SetRecorder([&log](const Event &event) { log.Write(event); });
        for (int i = 0; i < 10; ++i)
        {
            bus.Emit("mod.tick", Json{{"i", i}, {"padding", std::string(40, 'x')}});
        }
    }
    const auto lines = Lines(path);
    REQUIRE(lines.size() >= 2);
    CHECK(lines.back()["topic"] == "eluvian.events.full");
    CHECK(lines.size() < 10);
    std::filesystem::remove(path);
}

TEST_CASE("each game writes its file anew", "[event-log]")
{
    const auto path = TempFile("eluvian-test-events-anew.jsonl");
    for (int run = 0; run < 2; ++run)
    {
        EventLog log(path);
        EventBus bus;
        bus.SetRecorder([&log](const Event &event) { log.Write(event); });
        bus.Emit("run", Json{{"run", run}});
    }
    const auto lines = Lines(path);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0]["payload"]["run"] == 1);
    std::filesystem::remove(path);
}
