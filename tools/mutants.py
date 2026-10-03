"""Break src/core on purpose, one way at a time, and see the tests fail.

DAOTurnBased's tools/mutants.py (2026-10-02), brought here for issue #6.
The rule is SkyrimFollowerTactics' (its dev/PRINCIPLES.md, "Check that the
tests would catch a bug"): a green suite can pass while testing nothing, and
breaking the code on purpose shows what is never checked. `-Preset core-cov
-Coverage` says which lines the tests run; this says which promises they
hold, and the core now decides more on its own (travel, loot, scenario),
where a test that asserts too little would let a walk or a looting go wrong
in play. Each mutant below is one promise of src/core (dev/DESIGN.md, the
tools' descriptions, the comments that say why) broken by a small edit. The
tool applies it, builds the core preset, runs el_tests, and puts the file
back. A mutant is caught if a test fails (or the run hangs past the
timeout), and survives if every test passes. A survivor is a promise no
test holds: it gets the test that catches it, or, if the edit breaks no
promise, it goes, with a line saying why.

The runner is DAOTurnBased's: only the test binary and the list of mutants
are this tree's. The mutants run side by side (the user there, 2026-10-02:
"use as many cores as available, but keep the threads low priority so they
don't interfere w/ others"). Each worker has a copy of the core and its
tests under build\\mutants\\wN, with a build of its own, and breaks that
copy: the working tree is read and never written, so the core can be edited
while they run and a run that is stopped leaves nothing broken. The copies
are kept between runs and brought up to the tree file by file, so that only
what changed is built again. Every core is used, a mutant to each, and
everything runs below normal priority, as the build does.

So the tests run with every core busy, and twenty copies of them at once:
a test that needs to be scheduled within milliseconds, or a file name every
copy shares, fails there with nothing broken, and counts a mutant caught
that is not. The first run here had three such (2026-10-03: a 20 ms frame
timeout, 20 and 30 ms scenario budgets, the event log's fixed temporary
names); the tests now leave tenths of a second and name their files their
own. A mutant caught that should not be is checked alone: `-j 1` and its
name.

Run from a shell where tools\\build.ps1 has imported the developer
environment, after one `.\\tools\\build.ps1 -Preset core` (the workers use its
vcpkg packages), as one PowerShell line:

    .\\tools\\build.ps1 -Preset core; python tools\\mutants.py
    python tools\\mutants.py               every mutant
    python tools\\mutants.py travel loot   those whose name contains "travel" or "loot"
    python tools\\mutants.py -j 4          four at a time (by default one a core)

Exits non-zero if any mutant survives or fails to compile, and before
building anything if a mutant's text is not in its file exactly once.
"""

from __future__ import annotations

import argparse
import os
import queue
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORK = REPO / "build" / "mutants"
# What a worker's copy holds: the core preset builds from these and nothing else.
FILES = (
    "CMakeLists.txt",
    "CMakePresets.json",
    "vcpkg.json",
    "vcpkg-configuration.json",
)
TREES = ("src/core", "tests")
# The core preset's packages (Catch2, nlohmann-json), installed once by the
# tree's own build.
PACKAGES = REPO / "build" / "core" / "vcpkg_installed"
# el_tests takes 17 s (2026-10-03); a mutant that turns a cut wait into the
# whole one it was asked for (a scenario's minute) takes a little over one.
# Past three, a run has hung, and counts as caught: one that sends every
# listener tool to a frame no test pumps does.
TIMEOUT_S = 180
# Below normal, as tools\\build.ps1 runs the build: the machine stays usable.
LOW = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)


@dataclass(frozen=True)
class Mutant:
    name: str
    file: str
    old: str
    new: str


TRAVEL = "src/core/Travel.cpp"
LOOT = "src/core/Loot.cpp"
SCENARIO = "src/core/Scenario.cpp"
BUS = "src/core/EventBus.cpp"
BENCH = "src/core/Bench.cpp"
QUEUE = "src/core/FrameQueue.cpp"
QUEUE_H = "src/core/FrameQueue.h"
HTTP = "src/core/Http.cpp"
MCP = "src/core/Mcp.cpp"
REST = "src/core/Rest.cpp"
ORIGIN = "src/core/Origin.cpp"
LOGTAIL = "src/core/LogTail.cpp"
EVENTLOG = "src/core/EventLog.cpp"
ERF = "src/core/Erf.cpp"
ARGS = "src/core/Args.cpp"
REGISTRY = "src/core/ToolRegistry.cpp"
LEVELUP = "src/core/LevelUp.cpp"
CONVERSATION = "src/core/Conversation.cpp"

# A mutant's `old` is the source's text as written, so a C++ escape is two
# characters here: "\\r\\n" is the source's "\r\n". An edit that leaves a
# local or a parameter unread keeps it read ("|| true", "(void)x"): /W4 /WX
# would refuse it, and a mutant that does not compile tests nothing.
MUTANTS = [
    # travel: a walk to a thing or a point, through doors, and across to
    # another area (Travel.cpp; #1, #3, #4).
    Mutant(
        "travel ignores the `within` it is given",
        TRAVEL,
        "if (distance <= within_)",
        "if (distance <= 1.5)",
    ),
    Mutant(
        "travel takes a walk ending short of a thing for one still to make",
        TRAVEL,
        "if (length < 0.5)",
        "if (length < 0.0)",
    ),
    Mutant(
        "travel makes a move that brings it no nearer again until the steps run out (#4)",
        TRAVEL,
        "if (lastWalk && length > *lastWalk - kNoNearer)",
        "if (false)",
    ),
    Mutant(
        "travel says a point a move brings no nearer is reached",
        TRAVEL,
        'return End(false, "the walk goes no nearer: the last move brought it no nearer");',
        'return End(true, "the walk goes no nearer: the last move brought it no nearer");',
    ),
    Mutant(
        "travel keeps the walk's length from before a door once it is open",
        TRAVEL,
        "lastWalk.reset(); // the way is another once it is open",
        "// the way is another once it is open",
    ),
    Mutant(
        "travel ends the walk at a door shut on the way",
        TRAVEL,
        'if (*result != "COMMAND_FAILED_PATH_ACTION_REQUIRED")',
        "if (true)",
    ),
    Mutant(
        "travel makes a move the game refused again",
        TRAVEL,
        'if (*result != "COMMAND_FAILED_PATH_ACTION_REQUIRED")',
        "if (false)",
    ),
    Mutant(
        "travel uses a door that did not open again",
        TRAVEL,
        "if (blocker == lastBlocker)",
        "if (false)",
    ),
    Mutant(
        "travel's maxDoors lets one door more through",
        TRAVEL,
        "if (doors_.size() >= maxDoors_)",
        "if (doors_.size() > maxDoors_)",
    ),
    Mutant(
        "travel has another creature use a door by click",
        TRAVEL,
        "byClick_ = who_ == controlled;",
        "byClick_ = true;",
    ),
    Mutant(
        "travel takes the party to another area with a creature not controlled",
        TRAVEL,
        "if (!byClick_)\n            {\n                throw",
        "if (false)\n            {\n                throw",
    ),
    Mutant(
        "travel takes an exit that is not active",
        TRAVEL,
        '(exit_.empty() && area == area_ && exit.value("active", true))',
        "(exit_.empty() && area == area_)",
    ),
    Mutant(
        "travel's crossing ends before the other area is in place",
        TRAVEL,
        "if (!area.empty() && area != fromArea_)",
        "if (!area.empty())",
    ),
    Mutant(
        "travel's crossing reads the game while an area loads",
        TRAVEL,
        'if (mode == "loading")\n            {\n                continue;',
        "if (false)\n            {\n                continue;",
    ),
    Mutant(
        "travel's crossing tells a fight before the party left as another mode",
        TRAVEL,
        'if (mode == "combat")\n            {\n                return End(false, "a fight began before the party left");',
        'if (false)\n            {\n                return End(false, "a fight began before the party left");',
    ),
    Mutant(
        "travel walks on into a fight",
        TRAVEL,
        'if (mode == "combat")\n        {\n            return "a fight began";',
        'if (false)\n        {\n            return "a fight began";',
    ),
    Mutant(
        "travel walks on through a conversation",
        TRAVEL,
        'if (mode == "dialog" || mode == "cutscene")\n        {\n            return "a "',
        'if (false)\n        {\n            return "a "',
    ),
    Mutant(
        "travel walks on while an area loads",
        TRAVEL,
        'if (mode == "loading")\n        {\n            return "an area is loading";',
        'if (false)\n        {\n            return "an area is loading";',
    ),
    Mutant(
        "travel walks in a paused game",
        TRAVEL,
        'if (health.value("paused", false))',
        "if (false)",
    ),
    Mutant(
        "travel takes a command another cleared for the end of the walker's",
        TRAVEL,
        'if (result == "COMMAND_FAILED_COMMAND_CLEARED")',
        "if (false)",
    ),
    Mutant(
        "travel looks for a command's end only after the order answered",
        TRAVEL,
        'auto since = given.value("cursor", bench_.Events().Next());',
        "(void)given;\n        auto since = bench_.Events().Next();",
    ),
    Mutant(
        "travel takes another creature's command ending for the walker's",
        TRAVEL,
        '{"match", {{"id", who_}, {"type", type}}}',
        '{"match", {{"type", type}}}',
    ),
    Mutant(
        "travel waits for a command's end that never comes to the end of the budget",
        TRAVEL,
        "if (auto reason = NotMine())",
        "if (auto reason = std::optional<std::string>())",
    ),
    Mutant(
        "travel stopped says the distance from before its last move (#3)",
        TRAVEL,
        "if (!arrived && !measured_)",
        "if (false)",
    ),
    Mutant(
        "travel says a stale distance where none can be measured",
        TRAVEL,
        'answer_.erase("distance");',
        "",
    ),
    Mutant(
        "travel goes to the search's furthest point again and again",
        TRAVEL,
        "&& !wentToReached_)",
        ")",
    ),
    Mutant(
        "travel moves to the target's own place, not the walk's end",
        TRAVEL,
        'const Json &to = waypoints.is_array() && !waypoints.empty() ? waypoints.back() : geometry.at("to");',
        'const Json &to = geometry.at("to");',
    ),
    Mutant(
        "travel uses an exit it did not reach",
        TRAVEL,
        'if (!leaving || !walked.value("arrived", false))',
        "if (!leaving)",
    ),
    # loot: walk, use, the panel, Take All (Loot.cpp; #1).
    Mutant(
        "loot does not take from a panel up already",
        LOOT,
        "if (!open.empty())\n        {\n            if (!target_.empty() && open != target_)",
        "if (false)\n        {\n            if (!target_.empty() && open != target_)",
    ),
    Mutant(
        "loot takes from another container's panel up",
        LOOT,
        "if (!target_.empty() && open != target_)",
        "if (false)",
    ),
    Mutant(
        "loot refuses the target's own panel up",
        LOOT,
        "if (!target_.empty() && open != target_)",
        "if (!target_.empty())",
    ),
    Mutant(
        "loot uses a container its walk did not reach",
        LOOT,
        'if (!walked.value("arrived", false))',
        "if (false)",
    ),
    Mutant(
        "loot gives the walk the whole default budget",
        LOOT,
        "std::max<long long>(1000, Left().count())",
        "std::max<long long>(1000, 60000)",
    ),
    Mutant(
        "loot looks for the use's events only after the order answered",
        LOOT,
        'auto since = given.value("cursor", bench_.Events().Next());',
        "auto since = bench_.Events().Next();",
    ),
    Mutant(
        "loot takes another creature's use for the looter's",
        LOOT,
        '{"match", {{"id", who}, {"type", "UseObject"}}}',
        '{"match", {{"type", "UseObject"}}}',
    ),
    Mutant(
        "loot does not take from the target's panel when it opens",
        LOOT,
        "if (open == target_)",
        "if (false)",
    ),
    Mutant(
        "loot waits out another container's panel opening",
        LOOT,
        'if (!open.empty())\n            {\n                return End(false, "the loot panel of "',
        'if (false)\n            {\n                return End(false, "the loot panel of "',
    ),
    Mutant(
        "loot waits out a message box the use brings",
        LOOT,
        'if (gui.contains("popup") && !gui.at("popup").is_null())',
        "if (false)",
    ),
    Mutant(
        "loot waits out a fight the use brings",
        LOOT,
        'if (mode == "combat")',
        "if (false)",
    ),
    Mutant(
        "loot waits out a use that starts nothing to the end of the budget",
        LOOT,
        "if (!started && Clock::now() - ordered > kNoUse)",
        "if (false)",
    ),
    Mutant(
        "loot gives a use no time to start",
        LOOT,
        "constexpr milliseconds kNoUse{3000};",
        "constexpr milliseconds kNoUse{0};",
    ),
    Mutant(
        "loot does not wait for the panel once the use has ended",
        LOOT,
        "if (endedAt && Clock::now() - *endedAt > kPanelGrace)",
        "if (endedAt)",
    ),
    Mutant(
        "loot says a panel left up closed",
        LOOT,
        'const bool closed = !pressed.contains("panel") || pressed.at("panel").is_null();',
        "const bool closed = true;",
    ),
    Mutant(
        "loot does not say the backpack is full",
        LOOT,
        'if (pressed.value("full", false))',
        "if (false)",
    ),
    # scenario: steps, waits from the right cursor, the budget, expectations
    # (Scenario.cpp).
    Mutant(
        "scenario runs more than 100 steps",
        SCENARIO,
        "steps.size() > 100",
        "steps.size() > 1000",
    ),
    Mutant(
        "scenario goes on past a failed step",
        SCENARIO,
        "if (timedOut || (!failure.empty() && !continueOnError))",
        "if (timedOut || (!failure.empty() && !continueOnError && false))",
    ),
    Mutant(
        "scenario stops at a failed step though told to go on",
        SCENARIO,
        "if (timedOut || (!failure.empty() && !continueOnError))",
        "if (timedOut || (!failure.empty() && (continueOnError || true)))",
    ),
    Mutant(
        "scenario goes on out of time when told to go on",
        SCENARIO,
        "if (timedOut || (!failure.empty() && !continueOnError))",
        "if (!failure.empty() && !continueOnError)",
    ),
    Mutant(
        "scenario says a run that timed out is ok",
        SCENARIO,
        '{"ok", ok && !timedOut}',
        '{"ok", ok}',
    ),
    Mutant(
        "scenario's wait after a tool step sees what came before the step",
        SCENARIO,
        "cursor = bench.Events().Next();\n                        answer = bench.Invoke(",
        "answer = bench.Invoke(",
    ),
    Mutant(
        "scenario's second wait finds the first one's event again",
        SCENARIO,
        "cursor = event->seq + 1; // a second wait looks past this one",
        "// a second wait looks past this one",
    ),
    Mutant(
        "scenario does not read a wait's `since`",
        SCENARIO,
        '"since", static_cast<std::int64_t>(cursor), 0,',
        '"sinceMs", static_cast<std::int64_t>(cursor), 0,',
    ),
    Mutant(
        "scenario does not cut a wait to the time left",
        SCENARIO,
        "const auto timeout = std::min(asked, left());",
        "const auto timeout = asked;",
    ),
    Mutant(
        "scenario does not say a wait cut by the budget timed out",
        SCENARIO,
        "else if (timeout < asked)",
        "else if (false)",
    ),
    Mutant(
        "scenario goes on after a tool step that ran past the budget",
        SCENARIO,
        "if (left().count() == 0)\n                        {\n                            // A call the game",
        "if (false)\n                        {\n                            // A call the game",
    ),
    Mutant(
        "scenario's label holds a failed step's answer",
        SCENARIO,
        "label->is_string() && failure.empty()",
        "label->is_string()",
    ),
    Mutant(
        "scenario's equals passes another value",
        SCENARIO,
        "(!actual || *actual != *it)",
        "(!actual)",
    ),
    Mutant(
        "scenario's atLeast fails at its bound",
        SCENARIO,
        'std::string_view(bound) == "atLeast" ? have < limit : have > limit',
        'std::string_view(bound) == "atLeast" ? have <= limit : have > limit',
    ),
    Mutant(
        "scenario's atMost fails at its bound",
        SCENARIO,
        'std::string_view(bound) == "atLeast" ? have < limit : have > limit',
        'std::string_view(bound) == "atLeast" ? have < limit : have >= limit',
    ),
    Mutant(
        "scenario's wait that times out does not ask why",
        SCENARIO,
        "if (whyNothing)",
        "if (false)",
    ),
    # The event bus: topics, match as a subset, cursors, missed, WaitFor
    # (EventBus.cpp).
    Mutant(
        "the bus's match compares the whole payload",
        BUS,
        "(match.empty() || Subset(match, event.payload))",
        "(match.empty() || (Subset(match, event.payload) && match == event.payload))",
    ),
    Mutant(
        "the bus's match takes a nested object only whole",
        BUS,
        "return it != have.end() && Subset(field.value(), *it);",
        "return it != have.end() && field.value() == *it;",
    ),
    Mutant(
        "the bus's match takes a field that is no object for an object",
        BUS,
        "if (!have.is_object())\n    {\n        return false;",
        "if (!have.is_object())\n    {\n        return true;",
    ),
    Mutant(
        'the bus\'s topic "*" matches nothing',
        BUS,
        'if (pattern == "*")',
        "if (false)",
    ),
    Mutant(
        "the bus's topic prefix matches without its dot",
        BUS,
        "pattern.size() - 1); // keeps the dot",
        "pattern.size() - 2); // keeps the dot",
    ),
    Mutant(
        "the bus says a read from 0 missed events",
        BUS,
        "cursor != 0 && cursor < events_.front().seq",
        "cursor < events_.front().seq",
    ),
    Mutant(
        "the bus counts one missed event too many",
        BUS,
        "page.missed = events_.front().seq - cursor;",
        "page.missed = events_.front().seq - cursor + 1;",
    ),
    Mutant(
        "the bus's limit lets one more through",
        BUS,
        "if (page.events.size() == limit)",
        "if (page.events.size() > limit)",
    ),
    Mutant(
        "the bus's truncated read skips what it did not return",
        BUS,
        "if (!page.truncated)\n    {\n        page.next = std::max(page.next, next_);",
        "if (true)\n    {\n        page.next = std::max(page.next, next_);",
    ),
    Mutant(
        "the bus's cursor does not pass the events read",
        BUS,
        "page.next = event.seq + 1;",
        "",
    ),
    Mutant(
        "the bus does not tell its recorder",
        BUS,
        "if (recorder_)",
        "if (false)",
    ),
    Mutant(
        "the bus's wait goes on when told to stop",
        BUS,
        "(keepWaiting != nullptr && !keepWaiting->load())",
        "(keepWaiting != nullptr && false)",
    ),
    Mutant(
        "the bus's wait goes on past Close",
        BUS,
        "if (closed_ || (keepWaiting",
        "if ((keepWaiting",
    ),
    Mutant(
        "the bus's wait misses the event at its cursor",
        BUS,
        "if (event.seq >= cursor && filter.Accepts(event))",
        "if (event.seq > cursor && filter.Accepts(event))",
    ),
    Mutant(
        "the bus lists every topic whatever the filter",
        BUS,
        "if (filter.AcceptsTopic(topic))",
        "if (filter.AcceptsTopic(topic) || true)",
    ),
    # The bench: Invoke, events, wait_for, log (Bench.cpp).
    Mutant(
        "the bench sends a listener tool to the frame",
        BENCH,
        "if (thread == Thread::Listener)",
        "if (false)",
    ),
    Mutant(
        "the bench does not tell of a failed call",
        BENCH,
        "record(error.what());\n        throw;",
        "(void)error;\n        throw;",
    ),
    Mutant(
        "events reads from now by default",
        BENCH,
        'const auto since = static_cast<std::uint64_t>(args.Integer("since", 0, 0, kNoLimit));\n'
        '            const auto limit = static_cast<std::size_t>(args.Integer("limit", 100, 1, 1000));',
        "const auto since = static_cast<std::uint64_t>(\n"
        '                args.Integer("since", static_cast<std::int64_t>(bench.Events().Next()), 0, kNoLimit));\n'
        '            const auto limit = static_cast<std::size_t>(args.Integer("limit", 100, 1, 1000));',
    ),
    Mutant(
        "events with describe reads events",
        BENCH,
        'if (args.Bool("describe", false))',
        "if (false)",
    ),
    Mutant(
        "wait_for waits from the oldest event by default",
        BENCH,
        'args.Integer("since", static_cast<std::int64_t>(bench.Events().Next()), 0, kNoLimit)',
        'args.Integer("since", 0, 0, kNoLimit)',
    ),
    Mutant(
        "wait_for's next is the event found itself",
        BENCH,
        "const auto next = event->seq + 1;",
        "const auto next = event->seq;",
    ),
    Mutant(
        "wait_for that found nothing says nothing of why",
        BENCH,
        "if (whyNothing)\n            {\n                if (Json why",
        "if (false)\n            {\n                if (Json why",
    ),
    Mutant(
        "log reads a file named by a path",
        BENCH,
        "return (c >= 'a' && c <= 'z') ||",
        "return true || (c >= 'a' && c <= 'z') ||",
    ),
    Mutant(
        "log does not read its grep",
        BENCH,
        'if (args.Has("grep"))',
        "if (false)",
    ),
    # The frame queue: deadlines, abandoned calls, closing (FrameQueue.cpp, .h).
    Mutant(
        "the frame runs a call its caller abandoned",
        QUEUE,
        "if (task->state.compare_exchange_strong(expected, State::Running))",
        "if (task->state.compare_exchange_strong(expected, State::Running) || true)",
    ),
    Mutant(
        "the queue takes a call abandoned already for one running",
        QUEUE,
        " || expected == State::Abandoned",
        "",
    ),
    Mutant(
        "the queue takes calls once closed",
        QUEUE,
        "if (closed_)\n    {\n        return false;",
        "if (false)\n    {\n        return false;",
    ),
    Mutant(
        "the queue does not know the frame thread",
        QUEUE,
        "frameThread_ = std::this_thread::get_id();",
        "",
    ),
    Mutant(
        "the queue makes a call on the frame thread wait for a frame",
        QUEUE_H,
        "if (OnFrameThread())",
        "if (false)",
    ),
    Mutant(
        "the queue gives up on a running call at its timeout",
        QUEUE_H,
        "if (Abandon(*task))",
        "if (true)",
    ),
    # HTTP: the head's limit, the request line, headers, keep-alive,
    # Content-Length and chunked bodies, the routes (Http.cpp).
    Mutant(
        "http waits for a head that does not end past its limit",
        HTTP,
        'return buffer_.size() > kMaxHead ? Fail(431, "the request\'s head is too long") : Status::NeedMore;',
        "return Status::NeedMore;",
    ),
    Mutant(
        "http speaks any version",
        HTTP,
        'if (version != "HTTP/1.1" && version != "HTTP/1.0")',
        "if (false)",
    ),
    Mutant(
        "http reads + in a path as a space",
        HTTP,
        "UrlDecode(target.substr(0, queryAt), false)",
        "UrlDecode(target.substr(0, queryAt), true)",
    ),
    Mutant(
        "http takes a header with no name",
        HTTP,
        "if (colon == std::string_view::npos || colon == 0)",
        "if (colon == std::string_view::npos)",
    ),
    Mutant(
        "http/1.1 keeps the connection though asked to close",
        HTTP,
        'connection.find("close") == std::string::npos',
        "true",
    ),
    Mutant(
        "http/1.0 closes though asked to keep alive",
        HTTP,
        ': connection.find("keep-alive") != std::string::npos;',
        ": false;",
    ),
    Mutant(
        "http says a length too long to read is not a number",
        HTTP,
        "if (error == std::errc::result_out_of_range)",
        "if (false)",
    ),
    Mutant(
        "http takes a Content-Length with more after it",
        HTTP,
        "if (error != std::errc{} || end != text.data() + text.size())",
        "if (error != std::errc{} || end == nullptr)",
    ),
    Mutant(
        "http waits for a body larger than the bench takes",
        HTTP,
        "if (declared > kMaxBody)",
        "if (false)",
    ),
    Mutant(
        "http reads a body again as the start of the next request",
        HTTP,
        "buffer_.erase(0, bodyStart + length);",
        "buffer_.erase(0, bodyStart);",
    ),
    Mutant(
        "http does not check a chunk ends where its size says",
        HTTP,
        'if (buffer_.compare(at + size, 2, "\\r\\n") != 0)',
        "if (false)",
    ),
    Mutant(
        "http writes keep-alive as close",
        HTTP,
        'out += keepAlive ? "Connection: keep-alive\\r\\n" : "Connection: close\\r\\n";',
        'out += keepAlive ? "Connection: close\\r\\n" : "Connection: keep-alive\\r\\n";',
    ),
    Mutant(
        "http decodes half an escape",
        HTTP,
        "if (high >= 0 && low >= 0)",
        "if (high >= 0)",
    ),
    Mutant(
        "http answers a page from elsewhere",
        HTTP,
        'if (!IsLocalOrigin(request.Header("origin")))',
        "if (false)",
    ),
    Mutant(
        "http reads GET /mcp as a message",
        HTTP,
        'if (request.method != "POST")',
        "if (false)",
    ),
    Mutant(
        "http answers a body that is not JSON as an invalid request, not a parse error",
        HTTP,
        "if (message.is_discarded())",
        "if (false)",
    ),
    # MCP's JSON-RPC: what is an error and what is a tool's result (Mcp.cpp).
    Mutant(
        "mcp takes a message that is not JSON-RPC 2.0",
        MCP,
        'if (!message.is_object() || message.value("jsonrpc", "") != "2.0")',
        "if (!message.is_object())",
    ),
    Mutant(
        "mcp calls a method that is no string",
        MCP,
        "if (!method->is_string())",
        "if (false)",
    ),
    Mutant(
        "mcp answers an unknown method as an invalid request",
        MCP,
        "return ErrorReply(*id, kMethodNotFound,",
        "return ErrorReply(*id, kInvalidRequest,",
    ),
    Mutant(
        "mcp's initialize never answers the version asked for",
        MCP,
        "known != kProtocolVersions.end() ? *known : kProtocolVersions.front()",
        "known != kProtocolVersions.end() ? kProtocolVersions.front() : kProtocolVersions.front()",
    ),
    Mutant(
        "mcp answers an unknown tool as the tool's failure, not a protocol error",
        MCP,
        "throw RpcError{kInvalidParams, e.what()};",
        "return TextResult(e.what(), true);",
    ),
    Mutant(
        "mcp says a tool's refusal is no error",
        MCP,
        "return TextResult(e.what(), true);\n    }\n    catch (const FrameTimeout",
        "return TextResult(e.what(), false);\n    }\n    catch (const FrameTimeout",
    ),
    Mutant(
        "mcp gives a bare answer as structured content",
        MCP,
        "if (answer.data.is_object())",
        "if (true)",
    ),
    Mutant(
        "mcp answers a batch of notifications with an empty list",
        MCP,
        "if (replies.empty())",
        "if (false)",
    ),
    Mutant(
        "mcp says a frame not running is no error",
        MCP,
        '"game closed. `health` says which.",\n                          true);',
        '"game closed. `health` says which.",\n                          false);',
    ),
    # REST: statuses, the query, the body (Rest.cpp).
    Mutant(
        "rest answers an unknown tool as a bad request",
        REST,
        "return Error(404, e.what());",
        "return Error(400, e.what());",
    ),
    Mutant(
        "rest answers a frame not running as a server error",
        REST,
        "return Error(504,",
        "return Error(500,",
    ),
    Mutant(
        "rest drops a tool's other content",
        REST,
        "if (!answer.content.empty())",
        "if (false)",
    ),
    Mutant(
        "rest does not read limit from the query",
        REST,
        'if (key == "since" || key == "limit")',
        'if (key == "since")',
    ),
    Mutant(
        "rest reads a number with more after it from the query",
        REST,
        "if (error != std::errc{} || end != value.data() + value.size())",
        "if (error != std::errc{} || end == nullptr)",
    ),
    Mutant(
        "rest parses a blank body as JSON",
        REST,
        'if (body.find_first_not_of(" \\t\\r\\n") != std::string_view::npos)',
        "if (true)",
    ),
    Mutant(
        "rest passes a body that is not JSON to the tool",
        REST,
        "if (args.is_discarded())",
        "if (false)",
    ),
    Mutant(
        "rest calls a tool with any method",
        REST,
        'if (method == "POST" && path.starts_with(kToolPrefix))',
        "if (path.starts_with(kToolPrefix))",
    ),
    # Origin: local clients only (Origin.cpp).
    Mutant(
        "origin refuses a client that sends none",
        ORIGIN,
        "if (origin.empty())\n    {\n        return true;",
        "if (origin.empty())\n    {\n        return false;",
    ),
    Mutant(
        "origin passes a port that is not a number",
        ORIGIN,
        "if (port.empty() || !std::ranges::all_of(port, [](char c) { return c >= '0' && c <= '9'; }))",
        "if (port.empty())",
    ),
    Mutant(
        "origin reads an IPv6 host's own colons as its port",
        ORIGIN,
        "host.starts_with('[') ? host.find(']') : 0",
        "host.starts_with('[') ? host.find('[') : 0",
    ),
    # The log tail's cursor (LogTail.cpp).
    Mutant(
        "the log tail reads a file begun again from the old cursor",
        LOGTAIL,
        "if (cursor > page.size)",
        "if (false)",
    ),
    Mutant(
        "the log tail keeps a line's carriage return",
        LOGTAIL,
        "if (line.ends_with('\\r'))",
        "if (false)",
    ),
    Mutant(
        "the log tail's line limit lets one more through",
        LOGTAIL,
        "if (page.lines.size() == maxLines)",
        "if (page.lines.size() > maxLines)",
    ),
    Mutant(
        "the log tail's cursor passes a line still being written",
        LOGTAIL,
        "page.next = cursor + at;",
        "page.next = cursor + text.size();",
    ),
    Mutant(
        "the log tail does not keep its byte limit",
        LOGTAIL,
        "const std::size_t want = available < maxBytes ? static_cast<std::size_t>(available) : maxBytes;",
        "(void)maxBytes;\n    const std::size_t want = static_cast<std::size_t>(available);",
    ),
    Mutant(
        "the log tail says a line still being written is a truncation",
        LOGTAIL,
        "page.truncated = page.next < page.size && want < available;",
        "page.truncated = page.next < page.size;",
    ),
    # The events' file (EventLog.cpp).
    Mutant(
        "the event log grows past its most",
        EVENTLOG,
        "if (written_ + line.size() > most_)",
        "if (false)",
    ),
    Mutant(
        "the event log says it is full at every event after",
        EVENTLOG,
        "if (!out_.is_open() || full_)",
        "if (!out_.is_open())",
    ),
    Mutant(
        "the event log keeps the last game's events",
        EVENTLOG,
        "std::ios::binary | std::ios::trunc",
        "std::ios::binary | std::ios::app",
    ),
    Mutant(
        "the event log keeps a line in a buffer, not the file",
        EVENTLOG,
        "out_.flush();",
        "",
    ),
    # The archives' indexes (Erf.cpp).
    Mutant(
        "erf reads V2.0's entries at V2.2's size",
        ERF,
        "constexpr Layout kV20{32, 72};",
        "constexpr Layout kV20{32, 76};",
    ),
    Mutant(
        "erf reads the index's length from the wrong count",
        ERF,
        "Word(head, 16)",
        "Word(head, 20)",
    ),
    Mutant(
        "erf does not read the last name",
        ERF,
        "const std::size_t count = Word(index, 16);",
        "const std::size_t count = Word(index, 16) - 1;",
    ),
    Mutant(
        "erf reads an index cut short past its end",
        ERF,
        "if (at + kNameBytes > index.size())",
        "if (at > index.size())",
    ),
    Mutant(
        "erf keeps a letter beyond ASCII as a byte",
        ERF,
        "text += unit < 0x80 ? static_cast<char>(unit) : '?';",
        "text += static_cast<char>(unit);",
    ),
    # Arguments (Args.cpp).
    Mutant(
        "args cuts an id past 32 bits to fit",
        ARGS,
        "if (n <= std::numeric_limits<std::uint32_t>::max())",
        "if (true)",
    ),
    Mutant(
        "args reads a negative id",
        ARGS,
        "if (n >= 0 && n <= std::int64_t{std::numeric_limits<std::uint32_t>::max()})",
        "if (n <= std::int64_t{std::numeric_limits<std::uint32_t>::max()})",
    ),
    Mutant(
        "args reads an id with more after it",
        ARGS,
        "if (error != std::errc{} || end != text.data() + text.size())",
        "if (error != std::errc{} || end == nullptr)",
    ),
    Mutant(
        "args refuses null for no arguments",
        ARGS,
        "args.is_null() ? Json::object() : args",
        "args",
    ),
    Mutant(
        "args takes a fraction for a whole number",
        ARGS,
        "if (n != std::floor(n) || n < static_cast<double>(min) || n > static_cast<double>(max))",
        "if (n < static_cast<double>(min) || n > static_cast<double>(max))",
    ),
    Mutant(
        "args takes an integer above its most",
        ARGS,
        "if (n != std::floor(n) || n < static_cast<double>(min) || n > static_cast<double>(max))",
        "if (n != std::floor(n) || n < static_cast<double>(min))",
    ),
    Mutant(
        "args reads an integer given as text",
        ARGS,
        "if (!value->is_number())\n    {\n        Fail(key, range);",
        "if (false)\n    {\n        Fail(key, range);",
    ),
    # The tool registry (ToolRegistry.cpp).
    Mutant(
        "the registry keeps a tool's kinds when it is added again",
        REGISTRY,
        "entry.kinds.clear();",
        "",
    ),
    Mutant(
        "the registry leaves the revision when a tool is added",
        REGISTRY,
        "++revision_;\n    // Built where it lives",
        "// Built where it lives",
    ),
    Mutant(
        "the registry adds a kind to a tool with a handler of its own",
        REGISTRY,
        "if (it->second.handler)",
        "if (false)",
    ),
    Mutant(
        "the registry looks up a kind that is no string",
        REGISTRY,
        "if (kindArg == args.end() || !kindArg->is_string())",
        "if (kindArg == args.end())",
    ),
    Mutant(
        "the registry runs a kind on its base tool's thread",
        REGISTRY,
        "return Call{entry.spec.name, kindName, kit->second.spec.thread, kit->second.handler};",
        "return Call{entry.spec.name, kindName, entry.spec.thread, kit->second.handler};",
    ),
    Mutant(
        "the registry requires a kind already required twice",
        REGISTRY,
        "if (std::find(required.begin(), required.end(), property) == required.end())",
        "if (true)",
    ),
    Mutant(
        "the registry leaves a kind out of the schema's enum",
        REGISTRY,
        "kinds.push_back(kindName);",
        "",
    ),
    # levelup (#5): the level-up screen's own messages, in order, for the
    # creature named in decimal; refused without auto or points; rounds
    # while points are left and fewer; what was raised and gained.
    Mutant(
        "levelup reads a creature's id in decimal, not hex",
        LEVELUP,
        "value, 16);",
        "value, 10);",
    ),
    Mutant(
        "levelup goes on without auto",
        LEVELUP,
        'if (!args.Bool("auto", false))',
        "if (false)",
    ),
    Mutant(
        "levelup levels a creature with no points",
        LEVELUP,
        'if (!first.value("canLevelUp", false))',
        "if (false)",
    ),
    Mutant(
        "levelup ends the screen with no CharGenEnd",
        LEVELUP,
        '!Send("GUILevelUp.CharGenEnd " + n + " 1 1")',
        "false",
    ),
    Mutant(
        "levelup counts no rounds",
        LEVELUP,
        "++rounds_;",
        "",
    ),
    Mutant(
        "levelup stops after one round with points left",
        LEVELUP,
        'if (!last.value("canLevelUp", false) || !spent)',
        'if (!last.value("canLevelUp", false) || spent)',
    ),
    Mutant(
        "levelup goes round again when the auto spent nothing",
        LEVELUP,
        "const bool spent = PointsLeft(*read) < PointsLeft(last);",
        "const bool spent = PointsLeft(*read) <= PointsLeft(last);",
    ),
    Mutant(
        "levelup says levelled when nothing was spent",
        LEVELUP,
        "if (PointsLeft(last) >= PointsLeft(first))",
        "if (PointsLeft(last) > PointsLeft(first))",
    ),
    Mutant(
        "levelup counts an attribute's whole base as raised",
        LEVELUP,
        "raised[name] = to - from;",
        "raised[name] = to;",
    ),
    Mutant(
        "levelup lists the abilities it had as gained",
        LEVELUP,
        'if (!had.contains(ability.value("id", 0)))',
        'if (had.contains(ability.value("id", 0)))',
    ),
    Mutant(
        "levelup waits for the screen the wrong way round",
        LEVELUP,
        "if (shown == up)",
        "if (shown != up)",
    ),
    # dialogue (#17): one step without `until`; with it, on through lines to
    # the next real choice, an ignored choice asked again, a reply with no
    # text gone through, the lines kept, an end said with its mode.
    Mutant(
        "dialogue without until runs on anyway",
        CONVERSATION,
        "if (!untilChoice_)",
        "if (false)",
    ),
    Mutant(
        "dialogue takes a continue's replies for ones answered",
        CONVERSATION,
        'if (action_ == "choose")',
        "if (true)",
    ),
    Mutant(
        "dialogue does not ask an ignored choice again",
        CONVERSATION,
        'if (Left().count() == 0 || tries == kMostTries || !Ask("dialogue.step", step_))',
        "if (Left().count() == 0 || tries == kMostTries)",
    ),
    Mutant(
        "dialogue goes through no reply with no text",
        CONVERSATION,
        'return Ask("dialogue.step", Json{{"action", "continue"}}).has_value();',
        "return true;",
    ),
    Mutant(
        "dialogue takes the game's continue for a real choice",
        CONVERSATION,
        'return replies.is_array() && !replies.empty() && !replies.at(0).value("text", std::string()).empty();',
        "return replies.is_array() && !replies.empty();",
    ),
    Mutant(
        "dialogue keeps no line spoken",
        CONVERSATION,
        "!text.empty())",
        "text.empty())",
    ),
    Mutant(
        "dialogue takes a mode change to dialog for the end",
        CONVERSATION,
        'if (to != "dialog")',
        'if (to == "dialog")',
    ),
    Mutant(
        "dialogue answers a refusal where the conversation ended",
        CONVERSATION,
        'if (read && !read->value("active", true))',
        "if (false)",
    ),
    Mutant(
        "dialogue cuts lines short without skipLines and not with it",
        CONVERSATION,
        "if (skipLines_ && replies.empty()",
        "if (!skipLines_ && replies.empty()",
    ),
    # #7: a call the game refuses mid-journey ends the journey with the
    # refusal, the steps kept, instead of going up as the call's error.
    Mutant(
        "travel throws a refusal mid-journey up",
        TRAVEL,
        "stopped_ = e.what();",
        "throw ToolError(e.what());",
    ),
    Mutant(
        "loot throws a refusal mid-way up",
        LOOT,
        "refused_ = e.what();",
        "throw ToolError(e.what());",
    ),
    # #8: a move cleared with nothing after it is given again, not waited out.
    Mutant(
        "travel waits out a cleared move that nothing follows",
        TRAVEL,
        "if (cleared)",
        "if (false)",
    ),
    Mutant(
        "travel gives no cleared move again",
        TRAVEL,
        "lastWalk.reset(); // not a move made: it does not say the walk goes no nearer\n                    continue;",
        'return End(false, "the walk ended " + *result);',
    ),

    # #21: a target the walk itself opened is taken from; a target in the
    # walk's way is its end, not a door.
    Mutant(
        "loot uses a target the walk already opened",
        LOOT,
        "PanelOf(*gui) == target_)",
        "PanelOf(*gui) == target_ && false)",
    ),
    Mutant(
        "travel opens its own target as a door in the way",
        TRAVEL,
        'blocker == where_.value("target", std::string())',
        'blocker == std::string("no such")',
    ),
]


def unapplied(mutants: list[Mutant]) -> list[str]:
    """What is wrong with the list against the working tree, before anything
    is built: a mutant whose text is not in its file exactly once breaks
    nothing, or breaks the wrong place; two of one name are counted as one."""
    problems = []
    seen: set[str] = set()
    for mutant in MUTANTS:
        if mutant.name in seen:
            problems.append(f"two mutants are named {mutant.name!r}")
        seen.add(mutant.name)
    for mutant in mutants:
        path = REPO / mutant.file
        if not path.exists():
            problems.append(f"{mutant.name}: no {mutant.file}")
            continue
        times = path.read_text(encoding="utf-8").count(mutant.old)
        if times != 1:
            problems.append(
                f"{mutant.name}: its text is in {mutant.file} {times} times"
            )
        if mutant.old == mutant.new:
            problems.append(f"{mutant.name}: changes nothing")
    return problems


def run(
    command: list[str], cwd: Path, timeout: int | None = None
) -> subprocess.CompletedProcess[str]:
    # errors="replace": the compiler writes in the console's code page, and a
    # byte the locale's codec has no character for must not end the run.
    return subprocess.run(
        command,
        cwd=cwd,
        capture_output=True,
        text=True,
        errors="replace",
        timeout=timeout,
        check=False,
        creationflags=LOW,
    )


def sync(worker: Path) -> None:
    """Brings a worker's copy up to the tree: a file is written only where it
    differs, so what did not change is not built again."""
    wanted: set[Path] = set()
    sources = [REPO / name for name in FILES if (REPO / name).exists()]
    for tree in TREES:
        sources += [f for f in (REPO / tree).rglob("*") if f.is_file()]
    for source in sources:
        target = worker / source.relative_to(REPO)
        wanted.add(target)
        data = source.read_bytes()
        if not target.exists() or target.read_bytes() != data:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    for tree in TREES:
        for stale in (worker / tree).rglob("*"):
            if stale.is_file() and stale not in wanted:
                stale.unlink()


def made_with() -> str:
    """The build program the tree's own core build was configured with."""
    cache = REPO / "build" / "core" / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_MAKE_PROGRAM:"):
                return line.partition("=")[2].strip()
    return ""


def tests_of(worker: Path) -> Path:
    return worker / "build" / "core" / "tests" / "el_tests.exe"


def prepare(worker: Path, jobs: int) -> str:
    """A worker's copy, configured and built as the tree is. Answers what went
    wrong, or nothing."""
    sync(worker)
    if not (worker / "build" / "core" / "build.ninja").exists():
        command = [
            "cmake",
            "--preset",
            "core",
            f"-DVCPKG_INSTALLED_DIR={PACKAGES.as_posix()}",
            "-DVCPKG_MANIFEST_INSTALL=OFF",
        ]
        # Ninja as the tree's own build found it: it need not be on the path
        # of the shell this runs in.
        if ninja := made_with():
            command.append(f"-DCMAKE_MAKE_PROGRAM={ninja}")
        configured = run(command, worker)
        if configured.returncode != 0:
            return (configured.stdout + configured.stderr)[-2000:]
    built = build(worker, jobs)
    if built.returncode != 0:
        return (built.stdout + built.stderr)[-2000:]
    return ""


def build(worker: Path, jobs: int) -> subprocess.CompletedProcess[str]:
    return run(
        ["cmake", "--build", "--preset", "core", "--parallel", str(jobs)], worker
    )


def test(mutant: Mutant, worker: Path) -> str:
    """One mutant, in a worker's copy, from the copy as it was made: an edit to
    the tree while mutants run is not taken up half way (it was, read from the
    tree each time: a run whose mutants "did not compile" by the dozen,
    DAOTurnBased, 2026-10-02)."""
    original = (worker / mutant.file).read_text(encoding="utf-8")
    if original.count(mutant.old) != 1:
        return f"not applied: the text is in {mutant.file} {original.count(mutant.old)} times"
    path = worker / mutant.file
    try:
        path.write_text(original.replace(mutant.old, mutant.new), encoding="utf-8")
        # One file changed: one compiler.
        if build(worker, 1).returncode != 0:
            return "did not compile"
        try:
            result = run([str(tests_of(worker))], worker, timeout=TIMEOUT_S)
        except subprocess.TimeoutExpired:
            return "caught (the tests hung)"
        return "caught" if result.returncode != 0 else "SURVIVED"
    finally:
        path.write_text(original, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "names", nargs="*", help="only mutants whose name contains one of these"
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=os.cpu_count() or 2,
        help="how many mutants at a time (by default one a core)",
    )
    args = parser.parse_args()
    chosen = [
        m for m in MUTANTS if not args.names or any(w in m.name for w in args.names)
    ]
    if not chosen:
        print("no mutant's name contains any of those")
        return 1
    if wrong := unapplied(chosen):
        print("\n".join(wrong))
        return 1
    if not PACKAGES.exists():
        print(
            f"no {PACKAGES}: build the core preset once (tools\\build.ps1 -Preset core)"
        )
        return 1
    count = max(1, min(args.jobs, len(chosen)))
    workers = [WORK / f"w{n}" for n in range(count)]
    started = time.monotonic()

    # The copies, built side by side: each with its share of the cores.
    share = max(1, (os.cpu_count() or 2) // count)
    problems: dict[Path, str] = {}

    def ready(worker: Path) -> None:
        problems[worker] = prepare(worker, share)

    threads = [threading.Thread(target=ready, args=(w,)) for w in workers]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    for worker, problem in problems.items():
        if problem:
            print(f"{worker.name} does not configure or build:\n{problem}")
            return 1
    if run([str(tests_of(workers[0]))], workers[0]).returncode != 0:
        print("the unmutated core does not pass its tests; fix that first")
        return 1
    print(
        f"{count} at a time, in {WORK} ({time.monotonic() - started:.0f} s to make ready)",
        flush=True,
    )

    todo: queue.Queue[Mutant] = queue.Queue()
    for mutant in chosen:
        todo.put(mutant)
    verdicts: dict[str, str] = {}
    said = threading.Lock()

    def work(worker: Path) -> None:
        while True:
            try:
                mutant = todo.get_nowait()
            except queue.Empty:
                return
            start = time.monotonic()
            # Whatever goes wrong is said, and counted as not caught.
            try:
                verdict = test(mutant, worker)
            except Exception as error:  # noqa: BLE001
                verdict = f"failed: {error}"
            with said:
                verdicts[mutant.name] = verdict
                print(
                    f"{verdict:<32} {mutant.name} ({time.monotonic() - start:.0f} s)",
                    flush=True,
                )

    threads = [threading.Thread(target=work, args=(w,)) for w in workers]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    bad = [
        name for name, verdict in verdicts.items() if not verdict.startswith("caught")
    ]
    hung = [name for name, verdict in verdicts.items() if "hung" in verdict]
    for name in hung:
        print(f"hung, and counted as caught: {name}")
    print(
        f"{len(chosen) - len(bad)} of {len(chosen)} caught in {time.monotonic() - started:.0f} s"
    )
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
