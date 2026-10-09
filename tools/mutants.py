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
PARTY = "src/core/Party.cpp"
SCENARIO = "src/core/Scenario.cpp"
EXPECT = "src/core/Expect.cpp"
UNTIL = "src/core/Until.cpp"
READY = "src/core/Ready.cpp"
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
CRAFT = "src/core/Craft.cpp"
CONVERSATION = "src/core/Conversation.cpp"
PLACEABLES = "src/core/Placeables.cpp"
GFXLIST = "src/core/GfxList.cpp"
GIFT = "src/core/Gift.cpp"
REQUIREMENTS = "src/core/Requirements.cpp"

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
        "if (lastWalk_ && length > *lastWalk_ - kNoNearer)",
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
        "lastWalk_.reset(); // the way is another once it is open",
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
        "if (blocker == lastBlocker_)",
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
        '(exit_.empty() && area == area_ && exit.value("active", true) && !blocker && !tried_.contains(id)))',
        "(exit_.empty() && area == area_ && !blocker && !tried_.contains(id)))",
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
        'if (mode == "loading")\n        {\n            return std::nullopt;',
        "if (false)\n        {\n            return std::nullopt;",
    ),
    Mutant(
        "travel's crossing tells a fight before the party left as another mode",
        TRAVEL,
        'if (mode == "combat")\n        {\n            return End(false, "a fight began before the party left");',
        'if (false)\n        {\n            return End(false, "a fight began before the party left");',
    ),
    Mutant(
        "travel walks on into a fight",
        TRAVEL,
        "if (auto near = FightNear())",
        "if (auto near = std::optional<std::string>())",
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
        'if (mode == "loading")\n        {\n            if (auto why = WaitOutLoad())',
        "if (false)\n        {\n            if (auto why = WaitOutLoad())",
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
        "if (result == kCleared)",
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
        "travel walks up to its target in its own way again and again",
        TRAVEL,
        'if (distance > kNearOwnWay)\n        {\n            if (walk.contains("reached") && !walk.at("reached").is_null() && !wentToReached_)',
        'if (distance > kNearOwnWay)\n        {\n            if (walk.contains("reached") && !walk.at("reached").is_null())',
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
        '{"target", target_}, {"budgetMs", std::max<long long>(1000, Left().count())}',
        '{"target", target_}, {"budgetMs", std::max<long long>(1000, 60000)}',
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
        'if (!open.empty())\n        {\n            return "the loot panel of "',
        'if (false)\n        {\n            return "the loot panel of "',
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
        "if (timedOut_ || (failed && !continueOnError))",
        "if (timedOut_ || (failed && !continueOnError && !failed))",
    ),
    Mutant(
        "scenario stops at a failed step though told to go on",
        SCENARIO,
        "if (timedOut_ || (failed && !continueOnError))",
        "if (timedOut_ || (failed && (continueOnError || true)))",
    ),
    Mutant(
        "scenario goes on out of time when told to go on",
        SCENARIO,
        "if (timedOut_ || (failed && !continueOnError))",
        "if (failed && !continueOnError)",
    ),
    Mutant(
        "scenario says a run that timed out is ok",
        SCENARIO,
        '{"ok", ok && !timedOut_}',
        '{"ok", ok}',
    ),
    Mutant(
        "scenario's wait after a tool step sees what came before the step",
        SCENARIO,
        "cursor_ = bench_.Events().Next();\n        answer = bench_.Invoke(",
        "answer = bench_.Invoke(",
    ),
    Mutant(
        "scenario's second wait finds the first one's event again",
        SCENARIO,
        "cursor_ = event->seq + 1; // a second wait looks past this one",
        "// a second wait looks past this one",
    ),
    Mutant(
        "scenario does not read a wait's `since`",
        SCENARIO,
        '"since", static_cast<std::int64_t>(cursor_), 0,',
        '"sinceMs", static_cast<std::int64_t>(cursor_), 0,',
    ),
    Mutant(
        "scenario does not cut a wait to the time left",
        SCENARIO,
        "const auto timeout = std::min(asked, Left());",
        "const auto timeout = asked;",
    ),
    Mutant(
        "scenario does not say a wait cut by the budget timed out",
        SCENARIO,
        "if (timeout < asked)",
        "if (timeout < asked && timeout > asked)",
    ),
    Mutant(
        "scenario goes on after a tool step that ran past the budget",
        SCENARIO,
        "if (Left().count() == 0)\n        {\n            // A call the game",
        "if (false)\n        {\n            // A call the game",
    ),
    Mutant(
        "scenario's label holds a failed step's answer",
        SCENARIO,
        "label->is_string() && failure.empty()",
        "label->is_string()",
    ),
    Mutant(
        "scenario's equals passes another value",
        EXPECT,
        "(!actual || *actual != *it)",
        "(!actual)",
    ),
    Mutant(
        "scenario's atLeast fails at its bound",
        EXPECT,
        'std::string_view(bound) == "atLeast" ? have < limit : have > limit',
        'std::string_view(bound) == "atLeast" ? have <= limit : have > limit',
    ),
    Mutant(
        "scenario's atMost fails at its bound",
        EXPECT,
        'std::string_view(bound) == "atLeast" ? have < limit : have > limit',
        'std::string_view(bound) == "atLeast" ? have < limit : have >= limit',
    ),
    Mutant(
        "scenario's wait that times out does not ask why",
        SCENARIO,
        "if (whyNothing_)",
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
        "oldest && cursor != 0 && cursor < *oldest",
        "oldest && cursor < *oldest",
    ),
    Mutant(
        "the bus counts one missed event too many",
        BUS,
        "page.missed = *oldest - cursor;",
        "page.missed = *oldest - cursor + 1;",
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
        "if (!page.truncated)\n    {\n        page.next = std::max(page.next, next);",
        "if (true)\n    {\n        page.next = std::max(page.next, next);",
    ),
    Mutant(
        "the bus's cursor does not pass the events read",
        BUS,
        "page.next = event->seq + 1;",
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
        "if (event->seq >= cursor && filter.Accepts(*event))",
        "if (event->seq > cursor && filter.Accepts(*event))",
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
        "the bench does not tell of a refused call",
        BENCH,
        "record(error.what());\n        if (ignored.empty())",
        "(void)error;\n        if (ignored.empty())",
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
        "if (full_)\n    {\n        return;",
        "if (full_ && written_ == 0)\n    {\n        return;",
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
        "levelup levels a creature with no points",
        LEVELUP,
        "if (!offers_ && !canLevelUp_)",
        "if (false)",
    ),
    Mutant(
        "levelup counts no rounds",
        LEVELUP,
        "last = *read;\n            ++rounds_;",
        "last = *read;",
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
        "lastWalk_.reset(); // not a move made: it does not say the walk goes no nearer\n            return std::nullopt;",
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
    Mutant(
        "travel takes its cursor at the end, past its own events",
        TRAVEL,
        """(noticed writing the mutants, 2026-10-03, #13).
        answer_["cursor"] = bench_.Events().Next();""",
        "(noticed writing the mutants, 2026-10-03, #13).",
    ),
    Mutant(
        "travel gives up beside its target when the move fails there",
        TRAVEL,
        'if (after->value("distance", within_ + 1) <= within_)',
        "if (false)",
    ),
    # #15: an autosave's loading is waited out; a load into another area is
    # the journey's end.
    Mutant(
        "travel stops for an autosave as for an area loading",
        TRAVEL,
        "return NotMine(); // an autosave, done",
        'return std::string("an area is loading"); // an autosave, done',
    ),
    Mutant(
        "travel walks on after a load into another area",
        TRAVEL,
        "if (!startArea_.empty() && area != startArea_)",
        "if (false)",
    ),
    # #19: a pause that is a message box says what it asks.
    Mutant(
        "travel says only paused for a message box",
        TRAVEL,
        "if (const auto box = Box())",
        "if (const auto box = std::optional<std::string>())",
    ),
    Mutant(
        "dialogue does not say how to skip the cutscene a conversation ends into",
        CONVERSATION,
        'if (mode == "cutscene")',
        "if (false)",
    ),
    Mutant(
        "dialogue asks an ignored choice more than three times",
        CONVERSATION,
        "constexpr int kMostTries = 3;",
        "constexpr int kMostTries = 5;",
    ),
    # #29: an exit's use that fails is seen, and tried again from a step back.
    Mutant(
        "travel waits out an exit's use that failed",
        TRAVEL,
        'if (result.starts_with("COMMAND_FAILED") && result != kCleared)',
        "if (false)",
    ),
    Mutant(
        "travel tries a failed use again from where it failed",
        TRAVEL,
        "if (steppedBack || !StepBack(exit))",
        "if (steppedBack)",
    ),
    # #25: the party picker; a follower found where the name under the cursor
    # turns to theirs, in any case; the cursor given back; their slot
    # unchosen; Accept waited out; a click that chose nobody said.
    Mutant(
        "party takes a name read from before for a hit",
        PARTY,
        "const bool turned = name != last;",
        "const bool turned = !name.empty();",
    ),
    Mutant(
        "party names a follower only in their own case",
        PARTY,
        "if (turned && Lower(name) == Lower(name_))",
        "if (turned && name == name_)",
    ),
    Mutant(
        "party leaves the cursor held after a click",
        PARTY,
        "Release(); // the cursor the mouse's again once the click is sent",
        "// no release",
    ),
    Mutant(
        "party unchooses the slot after theirs",
        PARTY,
        'Send("GameModeController.UnselectPartyMember " + std::to_string(*slot));',
        'Send("GameModeController.UnselectPartyMember " + std::to_string(*slot + 1));',
    ),
    Mutant(
        "party answers Accept before the picker closes",
        PARTY,
        'while (mode == "partypicker" && Clock::now() < until)',
        "while (false)",
    ),
    Mutant(
        "party says nothing of a click that chose nobody",
        PARTY,
        "if (!SlotOf(Chosen()))",
        "if (false)",
    ),
    Mutant(
        "party answers Accept before the party holds everyone chosen",
        PARTY,
        "party.size() >= chosen)",
        "party.size() + chosen >= chosen)",
    ),
    # #27: an exit the walk cannot reach is used from where the walk stopped.
    Mutant(
        "travel gives up on an exit with no ground under it",
        TRAVEL,
        'why.starts_with("no walk there"))',
        "false)",
    ),
    # #23: who spoke a line goes with it.
    Mutant(
        "dialogue drops who spoke a line",
        CONVERSATION,
        '{"by", event.payload.value("by", Json())},',
        '{"by", Json()},',
    ),
    # #24: a world map pin by name in any case, tag or index; refused when not
    # active; the map waited for after its exit; an exit that opened no map
    # keeps its reason; an encounter is not arrival.
    Mutant(
        "travel to names a pin only in its own case",
        TRAVEL,
        'return Lower(pin.value("name", std::string())) == Lower(wanted) ||',
        'return pin.value("name", std::string()) == wanted ||',
    ),
    Mutant(
        "travel to clicks a pin that is not active",
        TRAVEL,
        'if (state != "active")',
        "if (false)",
    ),
    Mutant(
        "travel to does not wait for the map its exit opens",
        TRAVEL,
        'while (opening && map && !map->value("up", false) && Clock::now() < until)',
        "while (false)",
    ),
    Mutant(
        "travel to says the map is not up for an exit that did something else",
        TRAVEL,
        'if (opening && (!map || !map->value("up", false)))',
        "if (false)",
    ),
    Mutant(
        "travel to takes an encounter for the pin",
        TRAVEL,
        'if (area == pin.value("area", std::string()))',
        'if (area == pin.value("area", std::string()) || !area.empty())',
    ),
    Mutant(
        "travel to clicks from nowhere",
        TRAVEL,
        '"GameModeController.AreaTransition " + map.value("at", std::string()) + " " + std::to_string(index);',
        '"GameModeController.AreaTransition " + std::string("x") + " " + std::to_string(index);',
    ),
    Mutant(
        "travel to takes the last pin of a name, active or not",
        TRAVEL,
        'if (Names(candidate) && (pin == nullptr || (live && pin->value("state", std::string()) != "active")))',
        "if (Names(candidate) && live == live)",
    ),
    # #30: the map marked closed by travel before the pin's transition.
    Mutant(
        "travel to sends the transition with the map not marked closed by travel",
        TRAVEL,
        '{"set", "WorldMap.m_bClosedByTravel"}, {"value", true}});',
        '{"set", "WorldMap.m_bClosedByTravel"}, {"value", false}});',
    ),
    # #34: the Fade's own map is clicked as its pin does, with no mark.
    Mutant(
        "travel to marks the Fade's map as if it were the world's",
        TRAVEL,
        'if (map.value("map", std::string("world")) != "fade")',
        "if (true)",
    ),
    # #32: a text blocker is never a way out.
    Mutant(
        "travel takes a text blocker for the way to an area",
        TRAVEL,
        'exit.value("active", true) && !blocker && !tried_.contains(id)))',
        'exit.value("active", true) && !tried_.contains(id)))',
    ),
    Mutant(
        "travel waits out a command that ended unseen",
        TRAVEL,
        "if (!StillCommanded())",
        "if (false)",
    ),
    Mutant(
        "travel cuts short a move still under way",
        TRAVEL,
        "if (!StillCommanded())",
        "if (StillCommanded() || true)",
    ),
    # #33: a door the story keeps shut is said at once; a use that ends
    # unseen is a use made.
    Mutant(
        "travel uses a door the story keeps shut",
        TRAVEL,
        'walk.value(Json::json_pointer("/blocker/lock/pickLevel"), 0) >= kCannotPick)',
        'walk.value(Json::json_pointer("/blocker/lock/pickLevel"), 0) < 0)',
    ),
    Mutant(
        "travel forgives a use that ended unseen, and uses it again",
        TRAVEL,
        "return std::string(kUnseen);",
        "return std::string(kCleared);",
    ),
    # #40: a door whose key is required and not carried ends it at once.
    Mutant(
        "travel uses a door whose key the party does not carry",
        TRAVEL,
        "if (!blocker.empty() && keyMissing)",
        "if (blocker.empty() && keyMissing)",
    ),
    Mutant(
        "travel takes a carried key for a missing one",
        TRAVEL,
        '!walk.value(Json::json_pointer("/blocker/lock/keyHeld"), true);',
        "true;",
    ),
    # #45: only a locked door is shut for good.
    Mutant(
        "travel takes an unlocked door whose key is spent for shut for good",
        TRAVEL,
        "const bool keyMissing = locked && ",
        "const bool keyMissing = ",
    ),
    Mutant(
        "travel takes an unlocked story door for shut for good",
        TRAVEL,
        "if (!blocker.empty() && locked &&",
        "if (!blocker.empty() &&",
    ),
    # #44: a placeable's state named as its controller names it.
    Mutant(
        "a door's locked state is named shut",
        PLACEABLES,
        '"shut",      "locked", "open",',
        '"shut",      "shut", "open",',
    ),
    Mutant(
        "a static container's states are named as a container's",
        PLACEABLES,
        'kStatic = {"shut", "locked", "destroyed"}',
        'kStatic = {"shut", "locked", "open"}',
    ),
    # #46: the target in its own way is reached only near it.
    Mutant(
        "travel takes its target in its own way for reached from afar",
        TRAVEL,
        "if (distance > kNearOwnWay)",
        "if (distance > 1e9)",
    ),
    Mutant(
        "travel walks no nearer to its target in its own way",
        TRAVEL,
        "if (distance > kNearOwnWay)",
        "if (distance > 0)",
    ),
    # #47: a door locked with its key carried is used once more.
    Mutant(
        "travel gives up at a locked door after its key's first use",
        TRAVEL,
        "if (!keyTurned || keyTurned_ == blocker)",
        "if (keyTurned || keyTurned_ == blocker)",
    ),
    Mutant(
        "travel uses a door its key turned for ever",
        TRAVEL,
        "if (!keyTurned || keyTurned_ == blocker)",
        'if (!keyTurned || keyTurned_ == "never")',
    ),
    Mutant(
        "travel lists a door used twice twice",
        TRAVEL,
        'doors_.back().value("id", std::string()) != blocker',
        'doors_.back().value("id", std::string()) != "never"',
    ),
    Mutant(
        "levelup goes on with no way to level given",
        LEVELUP,
        "if ((auto_ ? 1 : 0) + (offers_ ? 1 : 0) + (chosen ? 1 : 0) != 1)",
        "if ((auto_ ? 1 : 0) + (offers_ ? 1 : 0) + (chosen ? 1 : 0) > 1)",
    ),
    Mutant(
        "levelup abandons the screen where it should keep what was spent",
        LEVELUP,
        '(complete ? "LEVELUP_EXIT_COMPLETE" : "LEVELUP_EXIT_ABORT")',
        '(complete ? "LEVELUP_EXIT_ABORT" : "LEVELUP_EXIT_ABORT")',
    ),
    Mutant(
        "levelup hides the screen with its exit point unset",
        LEVELUP,
        '{"set", "LevelUp.nExitPoint"}',
        '{"set", "LevelUp.nOtherPoint"}',
    ),
    Mutant(
        "levelup hides the screen though its exit point was not taken",
        LEVELUP,
        'if (!set->value("done", false))',
        'if (set->value("nothing", false))',
    ),
    # #5's choices: offers read with nothing spent; an ability not learnable
    # now refuses the lot; attributes spent as the screen's buttons do.
    Mutant(
        "levelup keeps what a look at the offers spent",
        LEVELUP,
        "Close(false); // abandoned: a look spends nothing",
        "Close(true); // abandoned: a look spends nothing",
    ),
    Mutant(
        "levelup spends an ability the screen does not offer now",
        LEVELUP,
        'if (state != "learnable")',
        "if (false)",
    ),
    Mutant(
        "levelup tells only the paperdoll of an attribute",
        LEVELUP,
        'if (!Send("GUICharGen.CharGenAssignAttribute " + spend) ||',
        "if (",
    ),
    Mutant(
        "levelup takes an attribute it does not know",
        LEVELUP,
        "if (AttributeIndex(name) == 0 ||",
        "if (",
    ),
    # #37: in combat mode a fight far off is walked on, one near stops it.
    Mutant(
        "travel stops for a fight far off",
        TRAVEL,
        "if (hostiles.empty())",
        "if (false)",
    ),
    Mutant(
        "travel walks into a fight at hand",
        TRAVEL,
        "if (hostiles.empty())",
        "if (true)",
    ),
    Mutant(
        "travel walks on in any fight though told to stop for every one",
        TRAVEL,
        "if (fightNear_ <= 0)",
        "if (false)",
    ),
    # #38: an exit named ahead is no door on the way.
    Mutant(
        "travel uses an exit ahead as a door",
        TRAVEL,
        'const bool exitAhead = !blocker.empty() && walk.at("blocker").contains("leadsTo") &&',
        'const bool exitAhead = false && walk.at("blocker").contains("leadsTo") &&',
    ),
    # #50: a blocker that is not interactive is not used.
    Mutant(
        "travel uses a thing in the way that is not interactive",
        TRAVEL,
        "if (!blocker.empty() && !exitAhead && !inertAhead)",
        "if (!blocker.empty() && !exitAhead)",
    ),
    # #53: where the walk finds none, short straight steps.
    Mutant(
        "travel stops dead where the walk finds none",
        TRAVEL,
        'walk.value("result", std::string()) == "none" && !straightTried_',
        'walk.value("result", std::string()) == "never" && !straightTried_',
    ),
    Mutant(
        "travel steps straight at an exit it cannot walk to",
        TRAVEL,
        "!straightTried_ && exit_.empty() && area_.empty())",
        "!straightTried_)",
    ),
    Mutant(
        "travel steps straight on though a step got no nearer",
        TRAVEL,
        "> before - kStraightGain",
        "> before + 1e9",
    ),
    Mutant(
        "travel stops stepping straight before the walk is found",
        TRAVEL,
        'step > 0 && read->value(Json::json_pointer("/walk/found"), false)',
        "step > 0",
    ),
    # #54: abilities taken as the screen's selection takes them.
    # levelup by name presses the ability's own icon (#58)
    Mutant(
        "levelup says nothing was spent after taking some",
        LEVELUP,
        'taken.empty() ? "; nothing was spent"',
        'taken.size() < 99 ? "; nothing was spent"',
    ),
    Mutant(
        "levelup counts a press that took no point as taken",
        LEVELUP,
        "if (*after < *before)",
        "if (*after <= *before)",
    ),
    Mutant(
        "levelup presses the first icon whatever the ability",
        LEVELUP,
        "icons_.find(ability)",
        "icons_.begin()",
    ),
    Mutant(
        "levelup plays the page again for each ability",
        LEVELUP,
        "if (shown_ == page.scene)",
        "if (false)",
    ),
    Mutant(
        "levelup shows the talents' page for a skill",
        LEVELUP,
        'if (page == "skills")',
        'if (page == "never")',
    ),
    Mutant(
        "levelup asks a spell's chains as a talent's",
        LEVELUP,
        'page == "spells" ? 2 : 1',
        'page == "never" ? 2 : 1',
    ),
    Mutant(
        "levelup reads one chain of a page",
        LEVELUP,
        "from = comma + 1;",
        "from = text.size();",
    ),
    Mutant(
        "levelup presses before the page shows its abilities",
        LEVELUP,
        "if (!icons->empty())",
        "if (icons->size() < 99)",
    ),
    Mutant(
        "levelup presses on another screen",
        LEVELUP,
        'args["screen"] = "LevelUp";',
        'args["screen"] = "Options";',
    ),
    Mutant(
        "levelup says a refused press had no points when it had",
        LEVELUP,
        "*before < 1 ?",
        "*before < -1 ?",
    ),
    Mutant(
        "levelup forgets what it took before a refusal",
        LEVELUP,
        "taken.push_back(ability);",
        "(void)ability;",
    ),
    Mutant(
        "levelup says no level-up is waiting for one that has",
        LEVELUP,
        "if (!canLevelUp_)",
        "if (canLevelUp_)",
    ),
    # #54: each kind's points counted by levelup itself.
    # what a call ignored is said (#61)
    Mutant(
        "an answer with an argument ignored loses its own fields",
        BENCH,
        "said[it.key()] = std::move(*it);",
        "(void)it;",
    ),
    Mutant(
        "a refusal keeps quiet of what it ignored",
        BENCH,
        "if (ignored.empty())",
        "if (ignored.size() < 99)",
    ),
    Mutant(
        "an argument the schema lists is said ignored",
        REGISTRY,
        "properties.contains(name) ||",
        "false ||",
    ),
    Mutant(
        "a kind's own property is said ignored",
        REGISTRY,
        "(!entry.handler && name == entry.spec.kindProperty)",
        "(false)",
    ),
    Mutant(
        "no argument meant is ever offered",
        REGISTRY,
        "best <= std::max<std::size_t>(2, name.size() / 3)",
        "best <= 0",
    ),
    Mutant(
        "any argument is offered as the one meant",
        REGISTRY,
        "best <= std::max<std::size_t>(2, name.size() / 3)",
        "best <= 99",
    ),
    Mutant(
        "a letter changed costs nothing in the distance",
        REGISTRY,
        "diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)",
        "diagonal + (a[i - 1] == b[j - 1] ? 0 : 0)",
    ),
    Mutant(
        "a schema with no arguments listed says every one ignored",
        REGISTRY,
        "if (!properties.is_object() || properties.empty())",
        "if (!properties.is_object())",
    ),
    # craft (#59): the crafting screen's own rules, its messages and the pack
    Mutant(
        "craft reads a maker's id in decimal, not hex",
        CRAFT,
        "value, 16);",
        "value, 10);",
    ),
    Mutant(
        "craft opens the screen for a maker without the skill",
        CRAFT,
        "if (!Held(kind_.ability))",
        "if (false)",
    ),
    Mutant(
        "craft crafts for whoever is controlled",
        CRAFT,
        "if (maker_ != controlled)",
        "if (false)",
    ),
    Mutant(
        "craft keeps the control it handed over",
        CRAFT,
        "if (!handBack_.empty())",
        "if (false)",
    ),
    Mutant(
        "craft crafts on a screen open for another",
        CRAFT,
        'selected->value("number", -1.0) != static_cast<double>(n_)',
        "false",
    ),
    Mutant(
        "craft crafts a recipe whose skill rank is missing",
        CRAFT,
        "if (!Held(now.skill))",
        "if (false)",
    ),
    Mutant(
        "craft crafts what the screen does not allow",
        CRAFT,
        "if (!now.canMake)",
        "if (false)",
    ),
    Mutant(
        "craft counts nothing it made",
        CRAFT,
        "made += *after - *before;",
        "made += 0;",
    ),
    Mutant(
        "craft takes one of several recipes a part of a name fits",
        CRAFT,
        "near.size() == 1",
        "!near.empty()",
    ),
    Mutant(
        "craft names no ingredient short",
        CRAFT,
        "ingredient.have < ingredient.need",
        "false",
    ),
    Mutant(
        "craft takes no recipe by its full name",
        CRAFT,
        "name == asked || product == asked",
        "false",
    ),
    Mutant(
        "craft reads a list's bare member names as full",
        CRAFT,
        "name.starts_with(prefix) ? name : prefix + name",
        "name",
    ),
    Mutant(
        "craft reads more paths at once than inspect takes",
        CRAFT,
        "kMostPaths = 64;",
        "kMostPaths = 640;",
    ),
    Mutant(
        "craft counts what is not the product",
        CRAFT,
        "== ResRef(productRef)",
        "!= ResRef(productRef)",
    ),
    Mutant(
        "craft compares a template with its .uti",
        CRAFT,
        'if (ref.ends_with(".uti"))',
        "if (false)",
    ),
    Mutant(
        "craft does not wait for the product",
        CRAFT,
        "if (*count > before)",
        "if (*count >= before)",
    ),
    Mutant(
        "craft lists every craft's recipes",
        CRAFT,
        "return BaseOf(recipe.skill) != kind_.ability;",
        "return recipe.skill < 0;",
    ),
    Mutant(
        "craft takes a craft's last rank for another's",
        CRAFT,
        "skill < kind.ability + 4",
        "skill < kind.ability + 3",
    ),
    Mutant(
        'craft keeps the "?" before a recipe\'s name',
        CRAFT,
        "if (recipe.name.starts_with('?'))",
        "if (false)",
    ),
    Mutant(
        "craft makes one more than asked",
        CRAFT,
        "while (made < count_)",
        "while (made <= count_)",
    ),
    Mutant(
        "craft gives the product no time to arrive",
        CRAFT,
        "kMadeWait{3000}",
        "kMadeWait{0}",
    ),
    Mutant(
        "craft leaves the screen up",
        CRAFT,
        'if (!Send("GUI.HideFullScreenGUI"))',
        "if (false)",
    ),
    Mutant(
        "craft goes on when the screen does not open",
        CRAFT,
        "if (!Screen(true))",
        "if (false)",
    ),
    Mutant(
        "craft makes two unasked",
        CRAFT,
        'Integer("count", 1, 1',
        'Integer("count", 2, 1',
    ),
    Mutant(
        "craft makes something when only asked to list",
        CRAFT,
        "if (recipe_.is_null())",
        "if (false)",
    ),
    # an item's requirements, as the screens split the engine's answer (#31)
    Mutant(
        "requirements read any mark but 0 as met",
        REQUIREMENTS,
        'pieces[i] == "1"',
        'pieces[i] != "0"',
    ),
    Mutant(
        "requirements read a pair at every piece",
        REQUIREMENTS,
        "i += 2)",
        "i += 1)",
    ),
    Mutant(
        "requirements lose a last piece with no ';' after it",
        REQUIREMENTS,
        "pieces.push_back(answer.substr(from));",
        "(void)from;",
    ),
    # levelup finds a skill in the skills page's own scroller (#63)
    Mutant(
        "levelup looks for a skill in the talents' scroller",
        LEVELUP,
        '3, "ScrollingSkills_mc.Scrollable_mc"',
        '3, "ScrollingAbilities_mc.Scrollable_mc"',
    ),
    Mutant(
        "levelup walks class containers only where the scroller has chains of its own",
        LEVELUP,
        "if (chains->empty())",
        "if (!chains->empty())",
    ),
    # levelup's specializations, as the screen's icons have them (#64)
    Mutant(
        "levelup lists no specializations in its offers",
        LEVELUP,
        'answer_["specializations"] = std::move(listed);',
        'answer_["noSpecializations"] = std::move(listed);',
    ),
    Mutant(
        "levelup reads a specialization had as not had",
        LEVELUP,
        'const bool had = selected->value("value", Json()) == true;',
        'const bool had = selected->value("value", Json()) == false;',
    ),
    Mutant(
        "levelup reads a grey specialization as open",
        LEVELUP,
        'const bool shut = grey->value("value", Json()) == true;',
        'const bool shut = grey->value("value", Json()) == false;',
    ),
    Mutant(
        "levelup calls a grey specialization locked though no point is left",
        LEVELUP,
        "else if (points < 1)",
        "else if (points < -1)",
    ),
    Mutant(
        "levelup presses a specialization had already",
        LEVELUP,
        'if (it->state == "has")',
        'if (it->state == "never")',
    ),
    Mutant(
        "levelup counts a specialization the screen took no point for",
        LEVELUP,
        "if (*left < *before)",
        "if (*left <= *before)",
    ),
    Mutant(
        "levelup takes no specialization by name",
        LEVELUP,
        "if (const auto taking = Specialization(ability))",
        "if (const auto taking = std::optional<bool>{}; ability < 0 && taking)",
    ),
    Mutant(
        "levelup goes on past a refused specialization",
        LEVELUP,
        "if (!*taking)",
        "if (!*taking && ability < 0)",
    ),
    Mutant(
        "levelup forgets a specialization taken before a refusal",
        LEVELUP,
        "taken.emplace_back(ability);",
        "(void)ability;",
    ),
    Mutant(
        "levelup takes only canLevelUp for a level-up waiting",
        LEVELUP,
        'first.value("canLevelUp", false) || PointsLeft(first) > 0;',
        'first.value("canLevelUp", false) || PointsLeft(first) > 99;',
    ),
    Mutant(
        "levelup says points are left when none are",
        LEVELUP,
        'PointsLeft(last) > 0 ? "levelled; points are left to spend"',
        'PointsLeft(last) >= 0 ? "levelled; points are left to spend"',
    ),
    # #55: a full pack's panel closed by loot itself.
    Mutant(
        "loot leaves a full pack's panel up",
        LOOT,
        'Json{{"action", "press"}, {"button", "Close"}}',
        'Json{{"action", "press"}, {"button", "Nothing"}}',
    ),
    Mutant(
        "loot says a full pack's panel stays up when it closed",
        LOOT,
        'if (answer_["closed"].get<bool>())',
        'if (!answer_["closed"].get<bool>())',
    ),
    # the party picker's status, and an unchoose or clear that changed nothing (#62)
    Mutant(
        "party changes the party in any picker status",
        PARTY,
        "status != kUse &&",
        "status == 99 &&",
    ),
    Mutant(
        "party refuses a look outside the choosing status",
        PARTY,
        'action_ != "look" && action_ != "accept"',
        'action_ != "never" && action_ != "accept"',
    ),
    Mutant(
        "party refuses where the status could not be read",
        PARTY,
        "status >= 0 && status != kUse",
        "status != kUse",
    ),
    Mutant(
        "party answers an unchoose the picker kept as done",
        PARTY,
        "if (SlotOf(Chosen()))",
        "if (false)",
    ),
    Mutant(
        "party says nothing of a clear that took no one out",
        PARTY,
        'if (Chosen() == answer_.at("before"))',
        "if (false)",
    ),
    Mutant(
        "party says a clear took no one out when it did",
        PARTY,
        'if (Chosen() == answer_.at("before"))',
        "if (true)",
    ),
    Mutant(
        "party names the picker's statuses wrongly",
        PARTY,
        '{"noUse", "readOnly", "use"}',
        '{"readOnly", "noUse", "use"}',
    ),
    # events from now; an answer's ignored said first (#66)
    Mutant(
        "events from the end reads from the start",
        BENCH,
        '{"events", Json::array()}, {"next", bench.Events().Next()}, {"missed", 0}, {"truncated", false}};',
        '{"events", Json::array()}, {"next", 0}, {"missed", 0}, {"truncated", false}};',
    ),
    Mutant(
        "an answer's ignored is said last, after the rest",
        BENCH,
        'Json said = {{"ignored", ignored}};',
        "Json said = Json::object();",
    ),
    # where the search finds none, the engine's own move first (#67)
    Mutant(
        "travel gives the engine no move of its own where the search finds none",
        TRAVEL,
        'if (EngineMove("the engine\'s own move, the search having found none"))',
        'if (!engineTried_ && EngineMove("the engine\'s own move, the search having found none"))',
    ),
    Mutant(
        "travel tries the engine's own move again and again",
        TRAVEL,
        '== "none" && !engineTried_ && exit_.empty()',
        '== "none" && exit_.empty()',
    ),
    # the engine's own move where the search's way ends at the target from afar (#67)
    Mutant(
        "travel gives up on a target named from afar without the engine's own move",
        TRAVEL,
        "if (EngineMove(\"the engine's own move, the search's way ending at the target\"))",
        "if (!engineTried_ && EngineMove(\"the engine's own move, the search's way ending at the target\"))",
    ),
    Mutant(
        "travel tries the engine's own move on a target named from afar again and again",
        TRAVEL,
        "if (!engineTried_)",
        "if (!engineTried_ || wentToReached_)",
    ),
    # the event file's line, written without copying the payload (#70)
    Mutant(
        "the event file leaves the payload out",
        EVENTLOG,
        'line += ",\\"payload\\":" + dump(event.payload);',
        'line += ",\\"payload\\":null";',
    ),
    Mutant(
        "the event file leaves the frame out",
        EVENTLOG,
        'line += ",\\"frame\\":" + dump(Json(event.frame));',
        'line += "";',
    ),
    # the run before's event file kept beside the new one (#74)
    Mutant(
        "the event file does not keep the run before's",
        EVENTLOG,
        "std::filesystem::rename(path, previous, error);",
        "(void)previous;",
    ),
    Mutant(
        "the event file keeps an empty run over the one before",
        EVENTLOG,
        "if (std::filesystem::file_size(path, error) > 0 && !error)",
        "if (std::filesystem::exists(path, error) && !error)",
    ),
    # #42: a pin not on the city's map is looked for on the other, swapped to once.
    Mutant(
        "travel never swaps a city's map for the other",
        TRAVEL,
        "if (!other.empty() && !swapped_)",
        "if (other.empty() && !swapped_)",
    ),
    Mutant(
        "travel swaps a city's maps back and forth",
        TRAVEL,
        "            swapped_ = true;\n",
        "            swapped_ = false;\n",
    ),
    # the screen's ability page, passed on (#71)
    Mutant(
        "levelup's offers leave the screen's ability page out",
        LEVELUP,
        'if (read->contains("abilityPage"))',
        'if (read->contains("abilityPages"))',
    ),
    # from c9a2cc5
    Mutant(
        "the event file's writer leaves half of each batch out",
        EVENTLOG,
        "            WriteNow(*event);\n        }",
        "            if (event->seq % 2 == 1)\n            {\n                WriteNow(*event);\n            }\n        }",
    ),
    Mutant(
        "the event file's writer stops with events still queued",
        EVENTLOG,
        "        if (queue_.empty())\n        {\n            return; // stopping, and everything given written",
        "        if (stopping_)\n        {\n            return; // stopping, and everything given written",
    ),
    Mutant(
        "the event file's Flush does not wait for the batch being written",
        EVENTLOG,
        "drained_.wait(lock, [this] { return queue_.empty() && !writing_; });",
        "drained_.wait(lock, [this] { return queue_.empty(); });",
    ),
    # wait_until and scenario's until step (#85)
    Mutant(
        "wait_until takes the first answer for the expectation met",
        UNTIL,
        "if (unmet.empty())",
        "if (unmet.empty() || tries > 0)",
    ),
    Mutant(
        "wait_until gives up at the first failed call",
        UNTIL,
        'answer["lastError"] = e.what();',
        'answer["lastError"] = e.what();\n            break;',
    ),
    Mutant(
        "wait_until waits a pointer that is not one out",
        UNTIL,
        '(void)expect::At(Json::object(), pointer, "wait_until");',
        "",
    ),
    Mutant(
        "wait_until counts no tries",
        UNTIL,
        'answer["tries"] = tries;',
        "",
    ),
    Mutant(
        "wait_until waits for itself",
        UNTIL,
        'if (tool == "wait_until" || tool == "scenario")',
        'if (tool == "scenario")',
    ),
    Mutant(
        "a scenario's until step passes though it did not hold",
        SCENARIO,
        'if (answer.value("held", false))',
        'if (answer.value("held", false) || !answer.value("held", false))',
    ),
    # wait_ready (#84)
    Mutant(
        "wait_ready takes a spoken line for a choice",
        READY,
        '!talk->value("replies", Json::array()).empty()',
        'talk->value("replies", Json::array()).empty()',
    ),
    Mutant(
        "wait_ready takes an interface with nothing up for input",
        READY,
        'if (gui->contains("popup") && !gui->at("popup").is_null())',
        'if (gui->contains("popup"))',
    ),
    Mutant(
        "wait_ready takes a cutscene for input",
        READY,
        'if (mode == "explore" || mode == "combat")',
        'if (mode == "explore" || mode == "combat" || mode == "cutscene")',
    ),
    Mutant(
        "wait_ready says nothing of what passed",
        READY,
        "for (const auto &event : bench.Events().Since(since, filter, 200).events)",
        "for (const auto &event : bench.Events().Since(since, filter, 0).events)",
    ),
    Mutant(
        "wait_ready counts what passed before the wait",
        READY,
        "const auto since = bench.Events().Next();",
        "const auto since = std::uint64_t{1};",
    ),
    Mutant(
        "wait_ready names no full screen",
        READY,
        'return "the " + gui->at("fullScreen").get<std::string>() + " screen";',
        'return std::string("a screen");',
    ),
    # a far way walked a leg at a time (#82)
    Mutant(
        "travel walks a far way's legs only once",
        TRAVEL,
        "if (end && (!wentToReached_ || (nearer && fresh)))",
        "if (end && (!wentToReached_ || (nearer && fresh && !wentToReached_)))",
    ),
    Mutant(
        "travel walks another leg that gets no nearer",
        TRAVEL,
        "const bool nearer = !legFrom_ || now < *legFrom_ - kLegGain;",
        "const bool nearer = !legFrom_ || now <= *legFrom_;",
    ),
    Mutant(
        "travel walks another leg to the same end",
        TRAVEL,
        "if (end && (!wentToReached_ || (nearer && fresh)))",
        "if (end && (!wentToReached_ || (nearer && (fresh || !fresh))))",
    ),
    Mutant(
        "travel counts no legs",
        TRAVEL,
        'answer_["legs"] = answer_.value("legs", 0) + 1;',
        "",
    ),
    # the pin of the area the party is in (#95)
    Mutant(
        "travel waits out its time at the pin of the area the party is in",
        TRAVEL,
        'if (!startArea_.empty() && pin->value("area", std::string()) == startArea_)',
        'if (!startArea_.empty() && pin->value("area", std::string()) == "" + name)',
    ),
    # another party member loots (#86)
    Mutant(
        "loot keeps control with the party member it handed it to",
        LOOT,
        "        if (const auto back = GiveBack())",
        "        if (const auto back = std::optional<Json>())",
    ),
    Mutant(
        "loot keeps control when the looting is refused",
        LOOT,
        "            GiveBack();\n            throw;",
        "            throw;",
    ),
    Mutant(
        "loot goes on when the party member will not take control",
        LOOT,
        '        if (!control.value("taken", false))',
        '        if (control.value("taken", false) && control.empty())',
    ),
    Mutant(
        "loot hands control back to the one controlled already",
        LOOT,
        "        if (before_.empty() || before_ == who_)",
        "        if (before_.empty())",
    ),
    # a walk ended short of a large object, the use's click walking the rest (#80)
    Mutant(
        "loot gives up on a large container its walk ended just short of",
        LOOT,
        '            if (distance > kUseReach || mode != "explore")',
        '            if (distance > 0 || mode != "explore")',
    ),
    Mutant(
        "loot uses a container its walk ended far short of",
        LOOT,
        '            if (distance > kUseReach || mode != "explore")',
        '            if (distance > kUseReach * 10 || mode != "explore")',
    ),
    Mutant(
        "loot uses a container though a fight stopped its walk",
        LOOT,
        '            if (distance > kUseReach || mode != "explore")',
        "            if (distance > kUseReach || mode.empty())",
    ),
    Mutant(
        "travel short of a large target does not say a use walks the rest",
        TRAVEL,
        '            answer_.value("distance", kUseReach + 1) <= kUseReach)',
        '            answer_.value("distance", kUseReach + 1) < 0)',
    ),
    # loot with a radius, by the placeable's action (#88)
    Mutant(
        "a placeable's loot action is named otherwise",
        PLACEABLES,
        '"dialog", "loot",',
        '"dialog", "use",',
    ),
    Mutant(
        "a placeable's action beyond the table is named",
        PLACEABLES,
        "    return static_cast<std::size_t>(action) < kActions.size()",
        "    return static_cast<std::size_t>(action) <= kActions.size()",
    ),
    Mutant(
        "a sweep loots what is not a container",
        LOOT,
        "                (action != kPlaceableActionLoot && action != kPlaceableActionUnlock))",
        "                (action == 0))",
    ),
    Mutant(
        "a sweep passes a locked container untried",
        LOOT,
        "                (action != kPlaceableActionLoot && action != kPlaceableActionUnlock))",
        "                (action != kPlaceableActionLoot))",
    ),
    Mutant(
        "a sweep loots what is not interactive",
        LOOT,
        '            if (!placeable.value("interactive", false) ||',
        '            if (!placeable.value("interactive", true) && false ||',
    ),
    Mutant(
        "a sweep goes on past a full backpack",
        LOOT,
        '                stopped = "the backpack is full";',
        "                looted += 0;",
    ),
    Mutant(
        "a sweep goes on past a fight",
        LOOT,
        "            if (stopped)\n            {\n                continue;\n            }",
        "            if (stopped && stopped->empty())\n            {\n                continue;\n            }",
    ),
    Mutant(
        "a sweep takes an herb's empty use for nothing there",
        LOOT,
        '                if (autoloot_ && result == "success")',
        '                if (autoloot_ && result == "never")',
    ),
    Mutant(
        "a sweep takes any container whose use opened nothing for picked",
        LOOT,
        '                if (autoloot_ && result == "success")',
        '                if (result == "success")',
    ),
    Mutant(
        "loot takes radius and target together",
        LOOT,
        "        if (radius_ && !target_.empty())",
        "        if (radius_ && target_.empty() && false)",
    ),
    Mutant(
        "a sweep lists around the one controlled, not the looter",
        LOOT,
        '            near["near"] = who_;',
        '            near["at"] = who_;',
    ),
    # a key-locked exit's second use (#96)
    Mutant(
        "travel waits out a key-locked exit its first use unlocked",
        TRAVEL,
        "        if (!succeeded || usedAgain || Clock::now() - *succeeded <= kUnlockGrace)",
        "        if (!succeeded || !usedAgain || Clock::now() - *succeeded <= kUnlockGrace)",
    ),
    Mutant(
        "travel uses an exit again and again",
        TRAVEL,
        "        usedAgain = true;\n        succeeded.reset();",
        "        succeeded.reset();",
    ),
    # the answer on the call's record, for the log (#98)
    Mutant(
        "a call's record does not carry its answer",
        BENCH,
        "        record({}, &result.data);",
        "        record({}, nullptr);",
    ),
    # a walk cut to upTo metres along the game's path (#90)
    Mutant(
        "travel's upTo cuts the walk nowhere",
        TRAVEL,
        "            if (upTo_ && !cut_ && CutWalk(geometry, walk))",
        "            if (upTo_ && cut_ && CutWalk(geometry, walk))",
    ),
    Mutant(
        "travel's upTo cuts from the goal's end",
        TRAVEL,
        "                const double share = length / leg;",
        "                const double share = 1.0 - (length / leg);",
    ),
    Mutant(
        "travel's upTo takes no length off at a turn",
        TRAVEL,
        "            length -= leg;\n            from = *next;",
        "            from = *next;",
    ),
    Mutant(
        "travel's upTo measures each leg from the start",
        TRAVEL,
        "            length -= leg;\n            from = *next;",
        "            length -= leg;",
    ),
    Mutant(
        "travel's upTo takes 0 metres",
        TRAVEL,
        "            if (*upTo_ <= 0)",
        "            if (*upTo_ < 0)",
    ),
    # a movie's list, comma-joined (#89)
    Mutant(
        "a movie's list keeps a blank before an element",
        GFXLIST,
        "        while (!element.empty() && element.front() == ' ')",
        "        while (!element.empty() && element.front() == 'x')",
    ),
    Mutant(
        "a movie's list keeps a blank after an element",
        GFXLIST,
        "        while (!element.empty() && element.back() == ' ')",
        "        while (!element.empty() && element.back() == 'x')",
    ),
    Mutant(
        "a movie's list takes a number with something after it",
        GFXLIST,
        "        if (error == std::errc() && end == element.data() + element.size() && !element.empty())",
        "        if (error == std::errc() && !element.empty())",
    ),
    Mutant(
        "a movie's list stops at its first comma",
        GFXLIST,
        "        joined = comma == std::string_view::npos ? std::string_view() : joined.substr(comma + 1);",
        "        joined = std::string_view();",
    ),
    # a crossing's load waited through; as near as the walk goes, near only (#99, #100)
    Mutant(
        "travel uses an exit again during its load",
        TRAVEL,
        '        if (answer_.value("mode", std::string()) == "loading")',
        '        if (answer_.value("mode", std::string()) == "dialog")',
    ),
    Mutant(
        "travel says arrived as near as the walk goes however far",
        TRAVEL,
        "        return End(distance <= std::max(within_, kUseReach), std::move(reason));",
        "        return End(distance >= 0, std::move(reason));",
    ),
    # a gift answered once the game has taken it (#81)
    Mutant(
        "a gift answers as it is signalled",
        GIFT,
        "    while (!taken)\n",
        "    while (!taken && false)\n",
    ),
    Mutant(
        "a gift is not taken when the item leaves the pack",
        GIFT,
        '            taken = !state.value("held", true) || state.value("approval", Json()) != before;',
        '            taken = state.value("approval", Json()) != before;',
    ),
    Mutant(
        "a gift is not taken when the approval moves",
        GIFT,
        '            taken = !state.value("held", true) || state.value("approval", Json()) != before;',
        '            taken = !state.value("held", true);',
    ),
    Mutant(
        "a gift waits past its budget",
        GIFT,
        "        if (taken || Clock::now() - started >= budget)",
        "        if (taken)",
    ),
    Mutant(
        "a gift says nothing of a gift not taken",
        GIFT,
        "    if (!taken)\n    {",
        "    if (taken && !taken)\n    {",
    ),
    # another exit to the same area where the first's use fails (#101)
    Mutant(
        "travel gives up at the first exit to an area whose use fails",
        TRAVEL,
        "        Json crossed = CrossAny();\n",
        "        Json crossed = Cross();\n",
    ),
    Mutant(
        "travel tries the same failed exit again",
        TRAVEL,
        '(exit_.empty() && area == area_ && exit.value("active", true) && !blocker && !tried_.contains(id)))',
        '(exit_.empty() && area == area_ && exit.value("active", true) && !blocker && tried_.size() < 99))',
    ),
    Mutant(
        "travel trades an exit named by id for another",
        TRAVEL,
        '        while (!area_.empty() && !crossed.value("arrived", false) &&',
        '        while (!crossed.value("arrived", false) &&',
    ),
    # a district's pin reached by its city (#91)
    Mutant(
        "travel looks for a pin by no city",
        TRAVEL,
        "        if (auto byCity = ByCity(map))\n",
        "        if (auto byCity = std::optional<Json>())\n",
    ),
    Mutant(
        "travel takes any active pin for the city's way in",
        TRAVEL,
        '                   holding->second.contains(pin.value("area", std::string()));',
        "                   !holding->second.empty();",
    ),
    Mutant(
        "travel stays in the city's entry area",
        TRAVEL,
        "        Cross();\n        return Map(true);",
        "        return Map(true);",
    ),
    # a room door is a door: its key held, its use only opening it (#104)
    Mutant(
        "travel refuses a door at the cannot-pick level whose key is held",
        TRAVEL,
        "        if (!blocker.empty() && locked && !keyHeld &&\n",
        "        if (!blocker.empty() && locked &&\n",
    ),
    Mutant(
        "travel takes any blocker with a leadsTo for an exit",
        TRAVEL,
        '            walk.at("blocker").value("action", kActionAreaTransition) == kActionAreaTransition;',
        '            walk.at("blocker").value("action", kActionAreaTransition) >= 0;',
    ),
    Mutant(
        "travel takes a blocker that says no action for a door",
        TRAVEL,
        '            walk.at("blocker").value("action", kActionAreaTransition) == kActionAreaTransition;',
        '            walk.at("blocker").value("action", 0) == kActionAreaTransition;',
    ),
    # a specialization listed by its icons and taken by name (#106)
    Mutant(
        "levelup lists the specializations by the screen's count, not its icons",
        LEVELUP,
        "        for (int n = 0; n < kMostSpecializations; ++n)",
        '        for (int n = 0; n < static_cast<int>(Number("LevelUp.nSpecializationsFound").value_or(0)); ++n)',
    ),
    Mutant(
        "levelup takes a specialization last, after the abilities",
        LEVELUP,
        "                chosen_.insert(chosen_.begin(), special.id);",
        "                chosen_.push_back(special.id);",
    ),
    Mutant(
        "levelup takes the specialization asked by name in its own case only",
        LEVELUP,
        "specialization_.is_string() && lower(special.name) == lower(specialization_.get<std::string>());",
        "specialization_.is_string() && special.name == specialization_.get<std::string>();",
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
