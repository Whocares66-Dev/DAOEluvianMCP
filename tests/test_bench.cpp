#include "core/Bench.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace eluvian;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("a frame tool runs on the thread that drains the frames", "[bench]")
{
    Bench bench;
    std::atomic<bool> running{true};
    std::thread::id frameThread;
    std::thread frames([&] {
        frameThread = std::this_thread::get_id();
        while (running)
        {
            bench.Frames().Drain();
            std::this_thread::sleep_for(1ms);
        }
    });
    std::thread::id ranOn;
    bench.Tools().Add(ToolSpec{"record", "Record the thread."}, [&ranOn](const Json &) -> ToolResult {
        ranOn = std::this_thread::get_id();
        return true;
    });
    CHECK(bench.Invoke("record", Json::object()).data == true);
    running = false;
    frames.join();
    CHECK(ranOn == frameThread);
}

TEST_CASE("an unknown tool and a tool's refusal reach the caller", "[bench]")
{
    Bench bench;
    bench.Tools().Add(ToolSpec{"refuse", "Refuse.", {}, Thread::Listener},
                      [](const Json &) -> ToolResult { throw ToolError("no"); });
    CHECK_THROWS_AS(bench.Invoke("nothing", {}), UnknownTool);
    CHECK_THROWS_AS(bench.Invoke("refuse", {}), ToolError);
}

TEST_CASE("an argument a tool does not have is named in its answer, with the one meant", "[bench]")
{
    Bench bench;
    const Json schema = {{"type", "object"}, {"properties", {{"topic", Json::object()}, {"since", Json::object()}}}};
    bench.Tools().Add(ToolSpec{"read", "Read.", schema, Thread::Listener},
                      [](const Json &) -> ToolResult { return Json{{"events", Json::array()}}; });
    const auto answer = bench.Invoke("read", Json{{"since", 0}, {"topics", {"eluvian.gui.message"}}}).data;
    REQUIRE(answer.contains("ignored"));
    CHECK(answer.begin().key() == "ignored"); // said first, not after a long list (#66)
    CHECK(answer["events"].is_array());
    CHECK(answer["ignored"].size() == 1);
    CHECK(answer["ignored"]["topics"] == "not an argument of read; did you mean `topic`?");
    // Far from any it has: what it takes is said instead.
    const auto far = bench.Invoke("read", Json{{"cursor", 3}}).data;
    CHECK(far["ignored"]["cursor"] == "not an argument of read; it takes `topic`, `since`");
    // Nearer one than the others, but by more than a third of it: not meant.
    const auto stem = bench.Invoke("read", Json{{"topicsfilter", "x"}}).data;
    CHECK(stem["ignored"]["topicsfilter"] == "not an argument of read; it takes `topic`, `since`");
    CHECK_FALSE(bench.Invoke("read", Json{{"topic", "x"}}).data.contains("ignored"));
}

TEST_CASE("a refusal says what argument it ignored, the likely why", "[bench]")
{
    Bench bench;
    const Json schema = {{"type", "object"}, {"properties", {{"abilities", Json::object()}}}};
    bench.Tools().Add(ToolSpec{"spend", "Spend.", schema, Thread::Listener},
                      [](const Json &) -> ToolResult { throw ToolError("give `abilities`"); });
    CHECK_THROWS_WITH(bench.Invoke("spend", Json{{"abilites", {7}}}),
                      "give `abilities` (`abilites` is not an argument of spend; did you mean `abilities`?)");
    CHECK_THROWS_WITH(bench.Invoke("spend", Json{{"abilities", {7}}}), "give `abilities`");
}

TEST_CASE("a tool whose schema lists no arguments, and a kind's own, are not said ignored", "[bench]")
{
    Bench bench;
    bench.Tools().Add(ToolSpec{"open", "Open.", {}, Thread::Listener},
                      [](const Json &) -> ToolResult { return Json::object(); });
    CHECK_FALSE(bench.Invoke("open", Json{{"anything", 1}}).data.contains("ignored"));
    bench.Tools().Add(ToolSpec{"bare", "Bare.", {{"type", "object"}}, Thread::Listener},
                      [](const Json &) -> ToolResult { return Json::object(); });
    CHECK_FALSE(bench.Invoke("bare", Json{{"anything", 1}}).data.contains("ignored"));
    ToolSpec look{"look", "Look.", {{"type", "object"}, {"properties", {{"id", Json::object()}}}}, Thread::Listener};
    bench.Tools().Add(look, nullptr);
    bench.Tools().AddKind("look", KindSpec{"game", "The game.", Thread::Listener},
                          [](const Json &) -> ToolResult { return Json::object(); });
    CHECK_FALSE(bench.Invoke("look", Json{{"kind", "game"}, {"id", "0x82"}}).data.contains("ignored"));
    CHECK(bench.Invoke("look", Json{{"kind", "game"}, {"idd", "0x82"}}).data.contains("ignored"));
}

TEST_CASE("events from the end answers the cursor now, to read from after acting", "[bench]")
{
    Bench bench;
    AddEventTools(bench);
    bench.Events().Emit("eluvian.old", Json::object());
    bench.Events().Emit("eluvian.old", Json::object());
    const auto mark = bench.Invoke("events", Json{{"since", "end"}}).data;
    CHECK(mark["events"].empty());
    CHECK(mark["next"] == bench.Events().Next());
    bench.Events().Emit("eluvian.new", Json::object());
    const auto after = bench.Invoke("events", Json{{"since", mark["next"]}}).data;
    REQUIRE(after["events"].size() == 1);
    CHECK(after["events"][0]["topic"] == "eluvian.new");
}

TEST_CASE("events returns the bus from a cursor, filtered", "[bench]")
{
    Bench bench;
    AddEventTools(bench);
    bench.Events().Emit("turnbased.turn.started", Json{{"id", "0x82"}});
    bench.Events().Emit("eluvian.mode.changed", Json{{"mode", "combat"}});
    bench.Events().Emit("turnbased.turn.ended", Json{{"id", "0x82"}});
    const auto answer = bench.Invoke("events", Json{{"topic", "turnbased.*"}, {"limit", 1}}).data;
    REQUIRE(answer["events"].size() == 1);
    CHECK(answer["events"][0]["topic"] == "turnbased.turn.started");
    CHECK(answer["truncated"] == true);
    const auto rest = bench.Invoke("events", Json{{"topic", "turnbased.*"}, {"since", answer["next"]}}).data;
    REQUIRE(rest["events"].size() == 1);
    CHECK(rest["events"][0]["topic"] == "turnbased.turn.ended");
    CHECK_THROWS_AS(bench.Invoke("events", Json{{"limit", 0}}), ToolError);
}

TEST_CASE("wait_for returns the event, or found false at its timeout", "[bench]")
{
    Bench bench;
    AddEventTools(bench);
    const auto cursor = bench.Events().Next();
    bench.Events().Emit("turnbased.fight.over", Json{{"reason", "no enemy left standing"}});
    auto answer = bench.Invoke("wait_for", Json{{"topic", "turnbased.fight.over"}, {"since", cursor}}).data;
    CHECK(answer["found"] == true);
    CHECK(answer["event"]["payload"]["reason"] == "no enemy left standing");
    CHECK(answer["next"] == cursor + 1);

    // From now, by default: the event before does not count.
    answer = bench.Invoke("wait_for", Json{{"topic", "turnbased.fight.over"}, {"timeoutMs", 10}}).data;
    CHECK(answer["found"] == false);
    CHECK(answer["waitedMs"] == 10);
}

namespace
{

class TempFolder
{
  public:
    TempFolder()
        : path_(std::filesystem::temp_directory_path() / ("eluvian_logs_" + std::to_string(std::random_device{}())))
    {
        std::filesystem::create_directories(path_);
    }
    // remove_all with an error_code throws only bad_alloc, which ends the
    // test run all the same (#77).
    ~TempFolder() // NOLINT(bugprone-exception-escape): above
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempFolder(const TempFolder &) = delete;
    TempFolder &operator=(const TempFolder &) = delete;

    void Write(const std::string &name, const std::string &text) const
    {
        std::ofstream(path_ / name, std::ios::binary | std::ios::app) << text;
    }
    [[nodiscard]] const std::filesystem::path &Path() const
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("log lists the logs, reads one from a cursor, and marks the end", "[bench]")
{
    const TempFolder folder;
    folder.Write("turnbased.log", "round 1\nAedan: attack\n");
    folder.Write("loader.log", "loaded\n");
    folder.Write("notes.txt", "not a log\n");
    Bench bench;
    AddLogTool(bench, folder.Path());

    auto answer = bench.Invoke("log", Json::object()).data;
    REQUIRE(answer["files"].size() == 2);
    CHECK(answer["files"][0]["file"] == "loader.log");

    answer = bench.Invoke("log", Json{{"file", "turnbased"}, {"grep", "attack"}}).data;
    CHECK(answer["lines"] == Json::array({"Aedan: attack"}));

    const auto mark = bench.Invoke("log", Json{{"file", "turnbased.log"}, {"since", "end"}}).data;
    CHECK(mark["lines"].empty());
    folder.Write("turnbased.log", "round 2\n");
    answer = bench.Invoke("log", Json{{"file", "turnbased"}, {"since", mark["next"]}}).data;
    CHECK(answer["lines"] == Json::array({"round 2"}));
}

TEST_CASE("log reads nothing outside its folder, and says how to find a log", "[bench]")
{
    const TempFolder folder;
    Bench bench;
    AddLogTool(bench, folder.Path());
    CHECK_THROWS_WITH(bench.Invoke("log", Json{{"file", "../secret"}}), ContainsSubstring("omit it to list them"));
    CHECK_THROWS_WITH(bench.Invoke("log", Json{{"file", "C:\\x"}}), ContainsSubstring("a log's name"));
    CHECK_THROWS_WITH(bench.Invoke("log", Json{{"file", "missing"}}), ContainsSubstring("no missing.log"));
    folder.Write("turnbased.log", "x\n");
    CHECK_THROWS_WITH(bench.Invoke("log", Json{{"file", "turnbased"}, {"grep", "("}}),
                      ContainsSubstring("not a regular expression"));
}

TEST_CASE("events describes the topics there are, filtered like a read", "[bench]")
{
    Bench bench;
    AddEventTools(bench);
    CHECK(bench.Topics().Describe(
        TopicSpec{"eluvian.creature.died", "A creature died.", Json{{"id", "the creature"}}, "eluvian"}));
    CHECK(bench.Topics().Describe(TopicSpec{"mymod.thing_happened", "A thing.", Json::object(), "mymod"}));
    CHECK_FALSE(bench.Topics().Describe(TopicSpec{"mymod.thing_happened", "A thing, again.", Json::object(), "mymod"}));

    auto answer = bench.Invoke("events", Json{{"describe", true}}).data;
    REQUIRE(answer["topics"].size() == 2);
    CHECK(answer["topics"][0]["topic"] == "eluvian.creature.died");
    CHECK(answer["topics"][0]["fields"]["id"] == "the creature");
    CHECK(answer["topics"][1]["description"] == "A thing, again.");

    answer = bench.Invoke("events", Json{{"describe", true}, {"topic", "mymod.*"}}).data;
    REQUIRE(answer["topics"].size() == 1);
    CHECK(answer["topics"][0]["from"] == "mymod");
}

TEST_CASE("a wait that times out carries what the host knows of why", "[bench]")
{
    Bench bench;
    bool paused = true;
    AddEventTools(bench, [&paused] { return paused ? Json{{"paused", true}} : Json::object(); });
    auto answer = bench.Invoke("wait_for", Json{{"topic", "nothing.comes"}, {"timeoutMs", 10}}).data;
    CHECK(answer["found"] == false);
    CHECK(answer["paused"] == true);
    paused = false;
    answer = bench.Invoke("wait_for", Json{{"topic", "nothing.comes"}, {"timeoutMs", 10}}).data;
    CHECK(answer["found"] == false);
    CHECK_FALSE(answer.contains("paused"));
}

TEST_CASE("every call is told of as it ends, with how long and how", "[bench]")
{
    struct Seen
    {
        std::string tool;
        Json args;
        Thread thread;
        std::string error;
        Json answer;
    };
    std::vector<Seen> seen;
    BenchOptions options;
    options.onCall = [&seen](const CallRecord &call) {
        CHECK(call.ms >= 0);
        seen.push_back(Seen{std::string(call.tool), call.args, call.thread, std::string(call.error),
                            call.answer != nullptr ? *call.answer : Json()});
    };
    Bench bench(options);
    bench.Tools().Add(ToolSpec{"echo", "Echo.", {}, Thread::Listener},
                      [](const Json &args) -> ToolResult { return args; });
    bench.Tools().Add(ToolSpec{"refuse", "Refuse.", {}, Thread::Listener},
                      [](const Json &) -> ToolResult { throw ToolError("refuse: not now"); });
    bench.Tools().Add(ToolSpec{"break", "Break.", {}, Thread::Listener},
                      [](const Json &) -> ToolResult { throw std::runtime_error("broke"); });

    bench.Invoke("echo", Json{{"id", "0x7d9"}});
    CHECK_THROWS_AS(bench.Invoke("refuse", Json::object()), ToolError);
    CHECK_THROWS_AS(bench.Invoke("nothing", Json::object()), UnknownTool); // no tool, so no call to tell of
    // A failure that is not a refusal is told of too, by its own path.
    CHECK_THROWS_AS(bench.Invoke("break", Json::object()), std::runtime_error);

    REQUIRE(seen.size() == 3);
    CHECK(seen[0].tool == "echo");
    CHECK(seen[0].args["id"] == "0x7d9");
    CHECK(seen[0].thread == Thread::Listener);
    CHECK(seen[0].error.empty());
    CHECK(seen[0].answer["id"] == "0x7d9"); // what it answered, for the log to say
    CHECK(seen[1].error == "refuse: not now");
    CHECK(seen[1].answer.is_null());
    CHECK(seen[2].error == "broke");
}
