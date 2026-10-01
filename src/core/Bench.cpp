#include "core/Bench.h"

#include "core/Args.h"
#include "core/LogTail.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <system_error>
#include <utility>

namespace eluvian
{

namespace
{

constexpr std::int64_t kNoLimit = std::numeric_limits<std::int64_t>::max();

} // namespace

Bench::Bench(BenchOptions options) : options_(std::move(options)), events_(options_.eventCapacity, options_.frame)
{
}

ToolResult Bench::Invoke(std::string_view tool, const Json &args)
{
    auto call = tools_.Resolve(tool, args);
    if (!call)
    {
        throw UnknownTool("no tool \"" + std::string(tool) + "\"; tools/list (or GET /api/tools) lists them");
    }
    if (call->thread == Thread::Listener)
    {
        return call->handler(args);
    }
    // The handler and arguments go to the frame thread behind shared
    // pointers: copying them there must not throw on the way.
    auto handler = std::make_shared<const ToolHandler>(std::move(call->handler));
    auto arguments = std::make_shared<const Json>(args);
    return frames_.RunAndWait([handler, arguments] { return (*handler)(*arguments); }, options_.frameTimeout);
}

void Bench::Close()
{
    events_.Close();
    frames_.Close();
}

void AddEventTools(Bench &bench)
{
    static const Json kFilterProperties = {
        {"topic",
         {{"description", "A topic (\"eluvian.command.ended\"), a prefix (\"mymod.*\"), \"*\", or an array of "
                          "them. None: every topic."}}},
        {"match",
         {{"type", "object"},
          {"description", "Payload fields the event must have, equal; objects match as subsets. Creatures are ids "
                          "like \"0x7d9\": {\"id\": \"0x7d9\"}."}}},
    };

    Json eventsSchema = {{"type", "object"}, {"properties", kFilterProperties}};
    eventsSchema["properties"]["since"] = {
        {"type", "integer"},
        {"minimum", 0},
        {"description", "The cursor to read from: `next` from the last read, or `cursor` from an answer. 0, the "
                        "default: the oldest event still held."}};
    eventsSchema["properties"]["limit"] = {
        {"type", "integer"}, {"minimum", 1}, {"maximum", 1000}, {"description", "At most this many; 100 by default."}};
    bench.Tools().Add(
        ToolSpec{"events",
                 "The bench's events since a cursor: what happened in the game (eluvian.*) and what mods emit (their "
                 "own prefix), in "
                 "order, each with its sequence number, frame and time. Returns `next`, the cursor to read from "
                 "next time, and `missed`, how many were too old to keep.",
                 eventsSchema, Thread::Listener, true},
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "events");
            const auto filter = EventFilter::FromArgs(args.Raw());
            const auto since = static_cast<std::uint64_t>(args.Integer("since", 0, 0, kNoLimit));
            const auto limit = static_cast<std::size_t>(args.Integer("limit", 100, 1, 1000));
            const auto page = bench.Events().Since(since, filter, limit);
            Json events = Json::array();
            for (const auto &event : page.events)
            {
                events.push_back(event.ToJson());
            }
            return Json{{"events", std::move(events)},
                        {"next", page.next},
                        {"missed", page.missed},
                        {"truncated", page.truncated}};
        });

    Json waitSchema = {{"type", "object"}, {"properties", kFilterProperties}};
    waitSchema["properties"]["since"] = {
        {"type", "integer"},
        {"minimum", 0},
        {"description", "Wait for an event from this cursor on: `cursor` from the answer of the call that set it "
                        "off, so an event that came before the wait is not missed. Default: from now."}};
    waitSchema["properties"]["timeoutMs"] = {{"type", "integer"},
                                             {"minimum", 0},
                                             {"maximum", 300000},
                                             {"description", "How long to wait; 30000 by default."}};
    bench.Tools().Add(
        ToolSpec{"wait_for",
                 "Wait until an event matching `topic` and `match` happens, and return it; instead of sleeping and "
                 "reading the log. `found` is false if the time ran out. Returns `next`, the cursor after it.",
                 waitSchema, Thread::Listener, true},
        [&bench](const Json &raw) -> ToolResult {
            const Args args(raw, "wait_for");
            const auto filter = EventFilter::FromArgs(args.Raw());
            const auto since = static_cast<std::uint64_t>(
                args.Integer("since", static_cast<std::int64_t>(bench.Events().Next()), 0, kNoLimit));
            const auto timeout = std::chrono::milliseconds(args.Integer("timeoutMs", 30000, 0, 300000));
            if (auto event = bench.Events().WaitFor(since, filter, timeout))
            {
                const auto next = event->seq + 1;
                return Json{{"found", true}, {"event", event->ToJson()}, {"next", next}};
            }
            return Json{{"found", false}, {"waitedMs", timeout.count()}, {"next", bench.Events().Next()}};
        });
}

namespace
{

// A log's name as an agent gives it: "loader", or "loader.log". Only a
// plain file name, so nothing outside the folder can be read.
std::optional<std::string> LogFileName(std::string name)
{
    if (name.empty() ||
        !std::ranges::all_of(name,
                             [](char c) {
                                 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                        c == '_' || c == '-' || c == '.';
                             }) ||
        name.find("..") != std::string::npos)
    {
        return std::nullopt;
    }
    if (!name.ends_with(".log"))
    {
        name += ".log";
    }
    return name;
}

Json ListLogs(const std::filesystem::path &directory)
{
    Json files = Json::array();
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error))
    {
        if (entry.is_regular_file(error) && entry.path().extension() == ".log")
        {
            files.push_back({{"file", entry.path().filename().string()}, {"size", entry.file_size(error)}});
        }
    }
    std::sort(files.begin(), files.end(), [](const Json &a, const Json &b) { return a["file"] < b["file"]; });
    return files;
}

} // namespace

void AddLogTool(Bench &bench, std::filesystem::path directory)
{
    const Json schema = {
        {"type", "object"},
        {"properties",
         {{"file",
           {{"type", "string"},
            {"description", "The log, by name: \"loader\", \"eluvian\", a mod's, any in the folder. Omit "
                            "to list them."}}},
          {"since",
           {{"description", "The byte cursor to read from: `next` from the last read. \"end\" reads nothing and "
                            "returns the end as `next`, a mark to read from after acting. Default: the start."}}},
          {"grep", {{"type", "string"}, {"description", "Keep only lines this regular expression finds a match in."}}},
          {"limit",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 5000},
            {"description", "At most this many lines; 300 by default."}}}}}};
    bench.Tools().Add(
        ToolSpec{"log",
                 "Read a DAOLib log (Documents\\BioWare\\Dragon Age\\daolib\\) from a cursor: the lines written since "
                 "`since`, and `next` to read from next time. `restarted` says the file began again (the game "
                 "restarted).",
                 schema, Thread::Listener, true},
        [directory = std::move(directory)](const Json &raw) -> ToolResult {
            const Args args(raw, "log");
            if (!args.Has("file"))
            {
                return Json{{"directory", directory.string()}, {"files", ListLogs(directory)}};
            }
            const auto name = LogFileName(args.String("file"));
            if (!name)
            {
                throw ToolError("log: `file` is a log's name, like \"loader\"; omit it to list them");
            }
            const auto path = directory / *name;
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error))
            {
                throw ToolError("log: no " + *name + " in " + directory.string() + "; omit `file` to list the logs");
            }
            if (args.Has("since") && args.Raw().at("since") == "end")
            {
                const auto size = std::filesystem::file_size(path, error);
                return Json{{"file", *name}, {"lines", Json::array()}, {"next", size}, {"size", size}};
            }
            const auto since = static_cast<std::uint64_t>(args.Integer("since", 0, 0, kNoLimit));
            std::optional<std::regex> grep;
            if (args.Has("grep"))
            {
                try
                {
                    grep.emplace(args.String("grep"), std::regex::ECMAScript | std::regex::optimize);
                }
                catch (const std::regex_error &e)
                {
                    throw ToolError(std::string("log: `grep` is not a regular expression: ") + e.what());
                }
            }
            const auto limit = static_cast<std::size_t>(args.Integer("limit", 300, 1, 5000));
            const auto page = ReadLog(path, since, grep, limit, 4u << 20);
            return Json{{"file", *name},     {"lines", page.lines},         {"next", page.next},
                        {"size", page.size}, {"restarted", page.restarted}, {"truncated", page.truncated}};
        });
}

} // namespace eluvian
