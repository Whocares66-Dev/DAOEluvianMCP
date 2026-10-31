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

Ghilana's own code (src/ghilana, #128) is a suite of its own: built with the
ghilana preset, 64-bit, so from a shell build.ps1 set up for it, and tested by
gh_tests without its cases over a real install. Each worker copies src/core,
src/ghilana, tests and cmake, and builds against the tree's own extern/DAOLib,
which it only reads:

    .\\tools\\build.ps1 -Preset ghilana; python tools\\mutants.py --suite ghilana

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
# What a worker's copy holds, besides its suite's trees.
FILES = (
    "CMakeLists.txt",
    "CMakePresets.json",
    "vcpkg.json",
    "vcpkg-configuration.json",
)


@dataclass(frozen=True)
class Suite:
    preset: str
    trees: tuple[str, ...]  # the folders a worker copies
    tests: str  # the test program, under the worker's build/<preset>
    args: tuple[str, ...] = ()  # what the test program is run with
    target: str = ""  # what a build builds; empty for all
    configure: tuple[str, ...] = ()  # more for its configure
    work: str = ""  # its workers' folder under build/mutants

    @property
    def packages(self) -> Path:
        # Its preset's packages, installed once by the tree's own build.
        return REPO / "build" / self.preset / "vcpkg_installed"


SUITES = {
    # src/core, its tests: el_tests.
    "core": Suite("core", ("src/core", "tests"), "tests/el_tests.exe"),
    # src/ghilana over src/core and DAOLib's core: gh_tests, its cases over a
    # real install left out (machine's, and an index of ten thousand archives
    # each).
    "ghilana": Suite(
        "ghilana",
        ("src/core", "src/ghilana", "tests", "cmake"),
        "tests/ghilana/gh_tests.exe",
        ("~[install]",),
        "gh_tests",
        (f"-DDAOLIB_DIR={(REPO / 'extern' / 'DAOLib').as_posix()}",),
        "ghilana",
    ),
}
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
    suite: str = "core"


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
ARGS = "src/core/Args.cpp"
REGISTRY = "src/core/ToolRegistry.cpp"
LEVELUP = "src/core/LevelUp.cpp"
CHARGEN = "src/core/CharGen.cpp"
CRAFT = "src/core/Craft.cpp"
CONVERSATION = "src/core/Conversation.cpp"
PLACEABLES = "src/core/Placeables.cpp"
GIFT = "src/core/Gift.cpp"
REQUIREMENTS = "src/core/Requirements.cpp"
PICTURE_SIZE = "src/core/PictureSize.cpp"
PEEK = "src/core/Peek.cpp"
SNAPSHOT = "src/core/Snapshot.cpp"
FRAME = "src/core/FrameCapture.cpp"
BEHIND = "src/core/Behind.cpp"
OPTIONS = "src/core/Options.cpp"
SIGHT = "src/core/Sight.cpp"
WARP = "src/core/Warp.cpp"
STDIO = "src/core/Stdio.cpp"

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
        "the bench runs a call without what its schema requires",
        BENCH,
        "if (const auto missing = tools_.Missing(tool, args); !missing.empty())",
        "if (const auto missing = tools_.Missing(tool, args); false)",
    ),
    Mutant(
        "the registry finds nothing missing",
        REGISTRY,
        "missing.push_back(name.get<std::string>());",
        "",
    ),
    Mutant(
        "the bench does not tell of a call to no tool",
        BENCH,
        "options_.onCall(CallRecord{tool, args, Thread::Listener, 0, said});",
        "(void)said;",
    ),
    Mutant(
        "the bench tells of a call without what it ignored",
        BENCH,
        "error, answer, &ignored}",
        "error, answer, nullptr}",
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
        "if (full_)\n    {\n        return;",
        "if (full_ && written_ == 0)\n    {\n        return;",
    ),
    Mutant(
        "the event log keeps the last game's events",
        EVENTLOG,
        "std::ios::binary | std::ios::trunc",
        "std::ios::binary | std::ios::app",
    ),
    # MCP over stdio (Stdio.cpp, #128): a message a line, each answered.
    Mutant(
        "stdio reads a line's carriage return as part of the message",
        STDIO,
        "(line.back() == '\\r' || line.back() == ' '",
        "(line.back() == ' '",
    ),
    Mutant(
        "stdio answers a blank line",
        STDIO,
        'if (line.find_first_not_of(" \\t") == std::string_view::npos)',
        "if (false)",
    ),
    Mutant(
        "stdio throws on a reply holding text that is not UTF-8",
        STDIO,
        "return value.dump(-1, ' ', false, Json::error_handler_t::replace);",
        "return value.dump(-1, ' ', false, Json::error_handler_t::strict);",
    ),
    Mutant(
        "mcp fails a tool's answer holding text that is not UTF-8",
        MCP,
        "answer.data.dump(-1, ' ', false, Json::error_handler_t::replace)",
        "answer.data.dump(-1, ' ', false, Json::error_handler_t::strict)",
    ),
    Mutant(
        "the event log keeps a line in a buffer, not the file",
        EVENTLOG,
        "out_.flush();",
        "",
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
        "the registry leaves the revision when a tool is added",
        REGISTRY,
        "const std::lock_guard lock(mutex_);\n    ++revision_;\n    const bool added",
        "const std::lock_guard lock(mutex_);\n    const bool added",
    ),
    Mutant(
        "the registry takes a tool without a handler",
        REGISTRY,
        "if (!handler)",
        "if (false)",
    ),
    # levelup (#5, #180): its steps by dao::levelup, each refusal said and
    # what was taken before it kept and ended; a specialization first, by
    # name in any case; the screen only shown, ended by its own Done.
    Mutant(
        "levelup ends a level-up with no screen up without levelup.finish",
        LEVELUP,
        'return Step("levelup.finish", Json{{"id", id_}}).has_value();',
        "return true;",
    ),
    Mutant(
        "levelup asks levelup.finish as well where the screen's Done ended it",
        LEVELUP,
        "        if (!screenUp_)\n        {\n            return Step(",
        "        if (!screenUp_ || show_.count() > 0)\n        {\n            return Step(",
    ),
    Mutant(
        "levelup leaves a level-up unended over abilities taken before a refusal",
        LEVELUP,
        "Settle(!taken.empty());",
        "Settle(false);",
    ),
    Mutant(
        "levelup leaves a level-up unended over attributes raised before a refusal",
        LEVELUP,
        "Settle(!taken.empty() || raised_);",
        "Settle(!taken.empty());",
    ),
    Mutant(
        "levelup closes the screen complete where nothing was taken",
        LEVELUP,
        "            screenUp_ = false;\n            Close(false);",
        "            screenUp_ = false;\n            Close(true);",
    ),
    Mutant(
        "levelup takes a step's refusal for a step taken",
        LEVELUP,
        'if (answer && !answer->value("taken", true))',
        'if (answer && !answer->value("taken", true) && args.contains("never"))',
    ),
    Mutant(
        "levelup takes a specialization as an ability",
        LEVELUP,
        '{"specialization", isSpecial}',
        '{"specialization", false}',
    ),
    Mutant(
        "levelup takes a specialization after the abilities named",
        LEVELUP,
        "chosen_.insert(chosen_.begin(), id);",
        "chosen_.push_back(id);",
    ),
    Mutant(
        "levelup matches a specialization's name in its own case",
        LEVELUP,
        'lower(special.value("name", std::string())) ==',
        'special.value("name", std::string()) ==',
    ),
    Mutant(
        "levelup says a locked specialization is locked as an ability is",
        LEVELUP,
        'refused_ += args.value("specialization", false)',
        "refused_ += false",
    ),
    Mutant(
        "levelup's auto stops after a round that spent",
        LEVELUP,
        'if (!last.value("canLevelUp", false) || !spent)',
        'if (!last.value("canLevelUp", false) || spent)',
    ),
    Mutant(
        "levelup's offers give the attributes' bases, not their totals",
        LEVELUP,
        'attributes[name] = value.value("total", 0.0);',
        'attributes[name] = value.value("base", 0.0);',
    ),
    Mutant(
        "levelup shows the screen with showMs 0",
        LEVELUP,
        "if (show_.count() == 0)",
        "if (show_.count() < 0)",
    ),
    Mutant(
        "levelup says nothing of a screen that would not open",
        LEVELUP,
        'answer_["screen"] = "not shown: " + refused_;',
        'answer_.erase("screen");',
    ),
    Mutant(
        "levelup's refusal lists every offer, not the learnable",
        LEVELUP,
        'answer_["offers"] = Learnable(*now);',
        'answer_["offers"] = *now;',
    ),
    Mutant(
        "levelup says nothing was spent where some was",
        LEVELUP,
        'return taken.empty() ? "; nothing was spent"',
        'return !taken.empty() ? "; nothing was spent"',
    ),
    Mutant(
        "levelup refuses a creature with only a specialization point (#64)",
        LEVELUP,
        '!first.value("canLevelUp", false) && PointsLeft(first) <= 0',
        '!first.value("canLevelUp", false)',
    ),
    Mutant(
        "levelup raises strength whatever attribute is named",
        LEVELUP,
        '{"attribute", AttributeIndex(name)}',
        '{"attribute", 1}',
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
    # party (#25, #182): the picker's steps by dao::partypicker, each refusal
    # said in words; a follower by name in any case; the chosen by slot; the
    # party read once it holds everyone chosen; a view-only picker refused.
    Mutant(
        "party lets a picker only to look at be changed",
        PARTY,
        "status >= 0 && status != kUse && action_",
        "status >= 0 && status == kUse && action_",
    ),
    Mutant(
        "party's clear says it took no one out when it took some",
        PARTY,
        'answer_.at("chosen") == answer_.at("before")',
        'answer_.at("chosen") != answer_.at("before")',
    ),
    Mutant(
        "party's look keeps the before",
        PARTY,
        '            answer_.erase("before");',
        '            answer_.erase("cursor");',
    ),
    Mutant(
        "party reads the picker before it is done showing",
        PARTY,
        "if (!entries.empty())",
        "if (!entries.is_null())",
    ),
    Mutant(
        "party counts everyone offered as chosen",
        PARTY,
        'if (entry.value("slot", -1) >= 0)',
        'if (entry.value("slot", -1) >= -1)',
    ),
    Mutant(
        "party leaves the chosen in the picker's order, not their slots'",
        PARTY,
        'std::ranges::sort(chosen, {}, [](const Json &member) { return member.value("slot", -1); });',
        'std::ranges::sort(chosen, {}, [](const Json &member) { return member.value("name", std::string()); });',
    ),
    Mutant(
        "party matches a follower's name in its own case",
        PARTY,
        'if (Lower(entry.value("name", std::string())) == Lower(name_) ||',
        'if (entry.value("name", std::string()) == name_ ||',
    ),
    Mutant(
        "party takes a refused step for one taken",
        PARTY,
        'if (took.value("taken", false))',
        'if (took.contains("taken"))',
    ),
    Mutant(
        "party says a full party as the picker's word",
        PARTY,
        'if (refusal == "party full")',
        'if (refusal == "party is full")',
    ),
    Mutant(
        "party says a choice of one chosen as not chosen",
        PARTY,
        'why = tool == "party.choose" ? "chosen already" : "not chosen";',
        'why = tool == "party.release" ? "chosen already" : "not chosen";',
    ),
    Mutant(
        "party reads the party as the picker closes",
        PARTY,
        'const std::size_t chosen = answer_.at("before").size();',
        "const std::size_t chosen = 0;",
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
        'if (state != "active" && !(fade && state == "completed"))',
        'if (fade && state == "never")',
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
    # #34, #42, #191: the Fade's map and a city's swap by DAOLib's worldmap.
    Mutant(
        "travel to takes a completed pin of the Fade's map only as active",
        TRAVEL,
        'if (state != "active" && !(fade && state == "completed"))',
        'if (state != "active" && !(fade && state == "active"))',
    ),
    Mutant(
        "travel to goes on past a swap the map refused",
        TRAVEL,
        'if (!swapped->value("taken", false))',
        'if (swapped->contains("never"))',
    ),
    Mutant(
        "travel to takes the last pin of a name, active or not",
        TRAVEL,
        'if (Names(candidate) && (pin == nullptr || (live && pin->value("state", std::string()) != "active")))',
        "if (Names(candidate) && live == live)",
    ),
    # #30, #184: the world's map clicked by DAOLib's worldmap::Travel, which
    # marks it closed by travel; its refusal said.
    Mutant(
        "travel to goes on past a pin the world map refused",
        TRAVEL,
        'if (!took->value("taken", false))',
        'if (took->contains("never"))',
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
    # #5's choices: offers read with nothing spent; an ability not learnable
    # now refuses the lot; attributes spent as the screen's buttons do.
    # The paperdoll's message is sent and not waited on (#126).
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
    # an argument a tool does not have is refused (#61, #147)
    Mutant(
        "an argument a tool does not have is let through",
        BENCH,
        "if (!ignored.empty())",
        "if (false)",
    ),
    Mutant(
        "an unknown argument's refusal does not say the one meant",
        BENCH,
        'said += " `" + name + "` is " + why.get<std::string>();',
        'said += " `" + name + "` is " + std::string(why.is_string() ? "" : "");',
    ),
    Mutant(
        "a missing argument is asked for before an unknown one is refused",
        BENCH,
        "        if (!ignored.empty())\n        {",
        "        if (!ignored.empty() && tools_.Missing(tool, args).empty())\n        {",
    ),
    Mutant(
        "an argument the schema lists is said ignored",
        REGISTRY,
        "if (properties.contains(name))",
        "if (false)",
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
    # craft (#59, #186): the steps by dao::crafting, as many made as asked and
    # each refusal said in words; a recipe by id, name or the one a part names.
    Mutant(
        "craft counts a refused make as made",
        CRAFT,
        'if (took.value("taken", false))',
        'if (took.contains("taken"))',
    ),
    Mutant(
        "craft makes one fewer than asked",
        CRAFT,
        "while (made < count_ && stopped.empty())",
        "while (made + 1 < count_ && stopped.empty())",
    ),
    Mutant(
        "craft says an ingredient short that is not",
        CRAFT,
        "if (have < need)",
        "if (have <= need)",
    ),
    Mutant(
        "craft says a missing rank as something else",
        CRAFT,
        'if (refusal == "no rank")',
        'if (refusal == "no ranks")',
    ),
    Mutant(
        "craft says short ingredients as something else",
        CRAFT,
        'if (refusal == "no ingredients")',
        'if (refusal == "no ingredient")',
    ),
    Mutant(
        "craft takes the next recipe for an id",
        CRAFT,
        'if (recipe.value("id", 0) == recipe_.get<int>())',
        'if (recipe.value("id", 0) == recipe_.get<int>() + 1)',
    ),
    Mutant(
        "craft takes the first of several a part of a name names",
        CRAFT,
        "if (near.size() == 1)",
        "if (near.size() >= 1)",
    ),
    Mutant(
        "craft says a stop after some made as nothing done",
        CRAFT,
        'return End(made > 0, (made > 0 ? std::to_string(made) + " made, then " : std::string()) + stopped);',
        'return End(false, (made > 0 ? std::to_string(made) + " made, then " : std::string()) + stopped);',
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
    # #55: a full pack's panel closed by loot itself.
    Mutant(
        "loot leaves a full pack's panel up",
        LOOT,
        'Ask("press", Json{{"button", "Close"}})',
        'Ask("press", Json{{"button", "Nothing"}})',
    ),
    Mutant(
        "loot says a full pack's panel stays up when it closed",
        LOOT,
        'if (answer_["closed"].get<bool>())',
        'if (!answer_["closed"].get<bool>())',
    ),
    # events from now (#66)
    Mutant(
        "events from the end reads from the start",
        BENCH,
        '{"events", Json::array()}, {"next", bench.Events().Next()}, {"missed", 0}, {"truncated", false}};',
        '{"events", Json::array()}, {"next", 0}, {"missed", 0}, {"truncated", false}};',
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
        "        if (!blocker.empty() && locked && (keyHeld || !keyHeld) &&\n",
    ),
    # #43: a move cut short part way, nearer, is asked again a few times.
    Mutant(
        "travel ends a walk a crowd cut short",
        TRAVEL,
        "cutShort_ < kCutShortRetries",
        "cutShort_ < 0",
    ),
    Mutant(
        "travel asks again a move that got no nearer",
        TRAVEL,
        "distance - kCutShortGain",
        "distance + kCutShortGain",
    ),
    # #41: a second use that succeeds and leads nowhere ends the crossing after a quiet spell.
    Mutant(
        "travel waits out its budget after a use that leads nowhere",
        TRAVEL,
        "                crossing.quietSince = crossing.succeeded;\n",
        "",
    ),
    Mutant(
        "travel gives up on a crossing at once after its second use",
        TRAVEL,
        "Clock::now() - *crossing.quietSince >= kQuietAfterUse",
        "Clock::now() - *crossing.quietSince >= milliseconds(0)",
    ),
    Mutant(
        "travel gives up on a crossing whose slow load is under way",
        TRAVEL,
        'crossing.loadSeen = crossing.loadSeen || answer_.value("mode", std::string()) == "loading";',
        "crossing.loadSeen = false;",
    ),
    # #72: what to do when the budget runs out.
    Mutant(
        "travel says nothing of its budget when its time runs out",
        TRAVEL,
        'starts_with("the time ran out"))',
        'starts_with("the time ran out!"))',
    ),
    Mutant(
        "travel tells of its budget on arrival too",
        TRAVEL,
        'if (!arrived && answer_["reason"].get<std::string>().starts_with("the time ran out"))',
        'if (arrived || answer_["reason"].get<std::string>().starts_with("the time ran out"))',
    ),
    # #56: a move that fails where it began is tried once more after a pause.
    Mutant(
        "travel ends at a first move that fails where it began",
        TRAVEL,
        'if (*result == "COMMAND_FAILED_INVALID_PATH" && !settled_ &&',
        'if (*result == "never" && !settled_ &&',
    ),
    Mutant(
        "travel tries a move that fails where it began for ever",
        TRAVEL,
        "                    settled_ = true;\n",
        "                    settled_ = false;\n",
    ),
    Mutant(
        "travel pauses again on a move that got nearer",
        TRAVEL,
        'after->value("distance", distance) > distance - kNoNearer)',
        'after->value("distance", distance) > -1)',
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
    # a tool's budget in its listing (#108)
    Mutant(
        "a tool's budget is not listed",
        REGISTRY,
        "        if (entry.spec.budget)\n",
        "        if (entry.spec.budget && entry.spec.listed && entry.spec.name.empty())\n",
    ),
    # warp: the party put in an area at a waypoint, both checked first (#157)
    Mutant(
        "warp refuses an area the archives lack",
        WARP,
        'if (!file.value("found", false))',
        'if (file.contains("nothing"))',
    ),
    Mutant(
        "warp names the areas with the one asked in their name",
        WARP,
        "name.find(asked) != std::string::npos",
        "name == asked",
    ),
    Mutant(
        "warp names the areas that begin as the one asked",
        WARP,
        "name.starts_with(prefix)",
        "name.ends_with(prefix)",
    ),
    Mutant(
        "warp finds a waypoint without case",
        WARP,
        'if (Lower(waypoint.value("tag", std::string())) == Lower(tag))',
        'if (waypoint.value("tag", std::string()) == tag)',
    ),
    Mutant(
        "warp refuses a waypoint the area lacks",
        WARP,
        "            if (waypoint == nullptr)\n            {\n                throw",
        '            if (waypoint == nullptr && args_.Has("never"))\n            {\n                throw',
    ),
    Mutant(
        "warp refuses a waypoint of an area whose file is not read",
        WARP,
        "if (!readable && (",
        'if (!readable && args_.Has("never") && (',
    ),
    Mutant(
        "warp jumps only from explore or combat",
        WARP,
        'if (mode != "explore" && mode != "combat")',
        'if (mode == "never")',
    ),
    Mutant(
        "warp says the game refused the jump",
        WARP,
        'if (sent.value("value", 1) == 0)',
        'if (sent.value("value", 1) == 2)',
    ),
    Mutant(
        "warp waits out the load",
        WARP,
        'health->value("mode", std::string()) != "loading"',
        'health->value("mode", std::string()) != "never"',
    ),
    Mutant(
        "warp waits for the area asked, not the one left",
        WARP,
        'state && Lower(state->value("area", Json::object()).value("tag", std::string())) == Lower(tag)',
        "state && !tag.empty()",
    ),
    Mutant(
        "warp says the hero stands away from the waypoint",
        WARP,
        "if (off > kAtWaypoint)",
        "if (off > kAtWaypoint * 100)",
    ),
    # the size screenshot writes a picture at (#156)
    Mutant(
        "a picture given its width keeps the frame's aspect",
        PICTURE_SIZE,
        "fit.height = Scaled(*askedWidth, height, width);",
        "fit.height = height;",
    ),
    Mutant(
        "a picture given its height keeps the frame's aspect",
        PICTURE_SIZE,
        "fit.width = Scaled(*askedHeight, width, height);",
        "fit.width = width;",
    ),
    Mutant(
        "a side the other gives is rounded to the nearest pixel",
        PICTURE_SIZE,
        "static_cast<int>(std::lround(exact))",
        "static_cast<int>(exact)",
    ),
    Mutant(
        "two sides a pixel off the frame's aspect are its aspect",
        PICTURE_SIZE,
        "- *askedHeight) <= 1)",
        "- *askedHeight) <= 0)",
    ),
    Mutant(
        "two sides of another aspect are refused without crop",
        PICTURE_SIZE,
        "    if (!crop)\n",
        "    if (crop && !crop)\n",
    ),
    Mutant(
        "a crop is the middle of the frame",
        PICTURE_SIZE,
        "fit.cropX = (width - fit.cropWidth) / 2;",
        "fit.cropX = 0;",
    ),
    Mutant(
        "a crop of a wider aspect keeps the frame's whole width",
        PICTURE_SIZE,
        "static_cast<long long>(width) * *askedHeight > static_cast<long long>(height) * *askedWidth",
        "static_cast<long long>(width) * *askedHeight < static_cast<long long>(height) * *askedWidth",
    ),
    Mutant(
        "a side under 16 pixels is refused",
        PICTURE_SIZE,
        "*side < kLeastSide ||",
        "*side < kLeastSide - 1 ||",
    ),
    # a field of a live object by a chain of offsets (#161)
    Mutant(
        "peek follows no pointer but reads every offset from the start",
        PEEK,
        "        at = *pointer;\n",
        "",
    ),
    Mutant(
        "peek reads the value at the chain's end without its last offset",
        PEEK,
        "const std::uint32_t address = offsets.empty() ? at : Moved(at, offsets.back());",
        "const std::uint32_t address = at;",
    ),
    Mutant(
        "peek follows the last offset as a pointer too",
        PEEK,
        "for (std::size_t i = 0; i + 1 < offsets.size(); ++i)",
        "for (std::size_t i = 0; i + 1 < offsets.size() + 1; ++i)",
    ),
    Mutant(
        "peek reads a short engine string from the heap",
        PEEK,
        "if (*capacity > kStringInline)",
        "if (*capacity >= kStringInline)",
    ),
    Mutant(
        "peek reads a length past the capacity as a string",
        PEEK,
        "if (*size > kStringMost || *size > *capacity)",
        "if (*size > kStringMost)",
    ),
    Mutant(
        "peek takes a negative offset forward",
        PEEK,
        "out.push_back(negative ? -value : value);",
        "out.push_back(value);",
    ),
    # snapshot_diff and the snapshot's keys (#167)
    Mutant(
        "snapshot keys count within a tag and template",
        SNAPSHOT,
        "seen[kind]++",
        "seen[kind]",
    ),
    Mutant(
        "snapshot_diff weighs a position against its tolerance",
        SNAPSHOT,
        "by > tolerance_.position + 1e-9",
        "by > 1e-9",
    ),
    Mutant(
        "snapshot_diff measures facing the short way round",
        SNAPSHOT,
        "by = std::min(by, 360.0 - by);",
        ";",
    ),
    Mutant(
        "snapshot_diff says the skew as b's time minus a's",
        SNAPSHOT,
        "skew_ = *y - *x;",
        "skew_ = *x - *y;",
    ),
    Mutant(
        "snapshot_diff takes the party as a set",
        SNAPSHOT,
        "std::ranges::sort(keys);",
        ";",
    ),
    Mutant(
        "snapshot_diff compares an effect's time left",
        SNAPSHOT,
        'Measure(effects[i], others[i], "remaining", at + "/remaining", tolerance_.time);',
        ";",
    ),
    Mutant(
        "snapshot_diff leaves a creature's name uncompared",
        SNAPSHOT,
        'kNotCompared = {"key", "id", "name"};',
        'kNotCompared = {"key", "id"};',
    ),
    Mutant(
        "snapshot_diff lists at most `most`",
        SNAPSHOT,
        "if (differences_.size() >= most_)",
        "if (most_ == 0)",
    ),
    Mutant(
        "snapshot_diff refuses a negative tolerance",
        SNAPSHOT,
        "item.value().get<double>() < 0",
        "item.value().get<double>() < -1",
    ),
    Mutant(
        "snapshot_diff compares the area, mode and pause",
        SNAPSHOT,
        "if (inA && !Same(a.at(field), b.at(field)))",
        "if (inA && !Same(a.at(field), a.at(field)))",
    ),
    # frame_capture's answer over a frame's draws (#160)
    Mutant(
        "frame_capture ids a shader by FNV-1a 64",
        FRAME,
        "hash *= 0x100000001b3ULL;",
        "hash += 0x100000001b3ULL;",
    ),
    Mutant(
        "frame_capture takes a render state with D3DRS_",
        FRAME,
        "wanted.erase(0, 6);",
        "wanted.erase(0, 5);",
    ),
    Mutant(
        "frame_capture defaults the render states",
        FRAME,
        "return DefaultRenderStates();\n    }\n    if (!given->is_array())",
        "return {};\n    }\n    if (!given->is_array())",
    ),
    Mutant(
        "frame_capture matches a texture in any case",
        FRAME,
        "return Upper(text).find(Upper(part)) != std::string::npos;",
        "return std::string(text).find(part) != std::string::npos;",
    ),
    Mutant(
        "frame_capture matches a shader by the start of its id",
        FRAME,
        "bound.id.starts_with(shader)",
        "bound.id == shader",
    ),
    Mutant(
        "frame_capture filters by shader",
        FRAME,
        "(filter.shader.empty() || Runs(draw, filter.shader))",
        "(filter.shader.empty() || Runs(draw, filter.texture))",
    ),
    Mutant(
        "frame_capture leaves a sampler's constant to textures",
        FRAME,
        'if (constant.set == "sampler")',
        'if (constant.set == "bool")',
    ),
    Mutant(
        "frame_capture lists only the constants asked",
        FRAME,
        "if (!wanted.empty() && std::ranges::find(wanted, constant.name) == wanted.end())",
        "if (!wanted.empty() && std::ranges::find(wanted, constant.name) != wanted.end())",
    ),
    Mutant(
        "frame_capture names an unnamed sampler by its register",
        FRAME,
        'std::format("s{}", sampled.sampler)',
        'std::format("s{}", 0)',
    ),
    Mutant(
        "frame_capture skips `skip` matching draws",
        FRAME,
        "if (matched <= filter.skip || list.size() >= filter.most)",
        "if (matched < filter.skip || list.size() >= filter.most)",
    ),
    Mutant(
        "frame_capture says truncated after a skip",
        FRAME,
        '{"truncated", matched > filter.skip + listed}',
        '{"truncated", matched > listed}',
    ),
    Mutant(
        "frame_capture trims a register to the constant's width",
        FRAME,
        "if (i % 4 < width)",
        "if (i % 4 <= width)",
    ),
    Mutant(
        "frame_capture takes a column-major matrix's rows as its width",
        FRAME,
        "constant.columnMajor ? constant.rows : constant.columns",
        "constant.columns",
    ),
    Mutant(
        "frame_capture has no primitive type 0",
        FRAME,
        "if (type >= 1 && type < kNames.size())",
        "if (type < kNames.size())",
    ),
    # a launched copy kept behind the window in front (#175)
    Mutant(
        "behind: a normal or restoring show does not activate",
        BEHIND,
        "        return kShowNoActivate;",
        "        return command;",
    ),
    Mutant(
        "behind: a minimising show does not activate",
        BEHIND,
        "        return kShowMinNoActive;",
        "        return command;",
    ),
    Mutant(
        "behind: a plain or maximising show does not activate",
        BEHIND,
        "        return kShowNa;",
        "        return command;",
    ),
    Mutant(
        "behind: SetWindowPos never activates",
        BEHIND,
        "return flags | kNoActivate | kNoZOrder;",
        "return flags | kNoZOrder;",
    ),
    Mutant(
        "behind: SetWindowPos never raises the window",
        BEHIND,
        "return flags | kNoActivate | kNoZOrder;",
        "return flags | kNoActivate;",
    ),
    # what the engine drew (#168)
    Mutant(
        "frame_capture filters by material",
        FRAME,
        "(filter.material.empty() || (!draw.material.empty() && Contains(draw.material, filter.material)))",
        "(filter.material.empty() || draw.material.empty() || Contains(draw.material, filter.material))",
    ),
    Mutant(
        "frame_capture filters by model",
        FRAME,
        "(filter.model.empty() || (!draw.model.empty() && Contains(draw.model, filter.model)))",
        "true",
    ),
    Mutant(
        "frame_capture gives a node only where there is one",
        FRAME,
        "if (draw.node != 0)",
        "if (true)",
    ),
    Mutant(
        "frame_capture counts the frame's materials",
        FRAME,
        "            materials.insert(draw.material);",
        "",
    ),
    # the camera's angles for a point to be seen (#117)
    Mutant(
        "the camera faces a point from the point's own side",
        SIGHT,
        "std::atan2(pivot[1] - to[1], pivot[0] - to[0]);",
        "std::atan2(to[1] - pivot[1], to[0] - pivot[0]);",
    ),
    Mutant(
        "the camera looks down at a point above it",
        SIGHT,
        "sight.pitch = (kPi / 2) + std::atan2(dz, flat);",
        "sight.pitch = (kPi / 2) - std::atan2(dz, flat);",
    ),
    Mutant(
        "the camera's pivot is taken for the point it looks at",
        SIGHT,
        "    if (std::hypot(flat, dz) < kNoDistance)\n",
        "    if (false)\n",
    ),
    Mutant(
        "the pivot's height is reckoned from above the camera",
        SIGHT,
        "return position[2] - (flat * std::cos(pitch) / sine);",
        "return position[2] + (flat * std::cos(pitch) / sine);",
    ),
    Mutant(
        "the pivot's height is reckoned from a camera straight above",
        SIGHT,
        "if (std::abs(sine) < 0.1F)",
        "if (std::abs(sine) < 0.0F)",
    ),
    Mutant(
        "the close view is said to keep any tilt",
        SIGHT,
        "return std::clamp(pitch, kPi / 6, kPi * 5 / 6);",
        "return std::clamp(pitch, 0.0F, kPi);",
    ),
    # The hero built on the creation screen (#153).
    # options (#190): the options screen's steps by dao::options, waited on
    # as they land; a shown tab or an open screen no refusal; a set read back.
    Mutant(
        "options takes a refused step for one taken",
        OPTIONS,
        'if (took.value("taken", false) || (already && refusal == "already so"))',
        'if (took.value("taken", false) || (already || refusal == "already so"))',
    ),
    Mutant(
        "options refuses a tab shown already",
        OPTIONS,
        'Step("options.tab", Json{{"tab", args_.String("tab")}}, true);',
        'Step("options.tab", Json{{"tab", args_.String("tab")}}, false);',
    ),
    Mutant(
        "options refuses to open a screen open already",
        OPTIONS,
        'if (!took.value("taken", false) && refusal != "already so")',
        'if (!took.value("taken", false))',
    ),
    Mutant(
        "options answers open before the screen is up",
        OPTIONS,
        '        if (state.value("open", false))\n            {\n                return Look',
        '        if (state.value("tab", Json()).is_null() || state.value("open", false))\n            {\n                return Look',
    ),
    Mutant(
        "options steps on a closed screen",
        OPTIONS,
        'if (!state.value("open", false))',
        'if (state.value("open", true) && false)',
    ),
    Mutant(
        "options sets a checkbox the other way",
        OPTIONS,
        "return asked.get<bool>() ? 1 : 0;",
        "return asked.get<bool>() ? 0 : 1;",
    ),
    Mutant(
        "options takes a choice's value only in its own case",
        OPTIONS,
        "if (Lower(values[i].get<std::string>()) == Lower(asked.get<std::string>()))",
        "if (values[i].get<std::string>() == asked.get<std::string>())",
    ),
    Mutant(
        "options finds no control by its label",
        OPTIONS,
        'Lower(entry.value("label", std::string())) == Lower(asked))',
        'entry.value("label", std::string()) == "")',
    ),
    Mutant(
        "options answers a set before the plugin took it",
        OPTIONS,
        "if (took || Clock::now() >= until)",
        "if (took || Clock::now() < until)",
    ),
    Mutant(
        "options says nothing of the menu up again after it closed",
        OPTIONS,
        "if (back.is_string() && !back.get<std::string>().empty())",
        "if (back.is_array())",
    ),
    Mutant(
        "options says the screen closed while a confirmation is up",
        OPTIONS,
        "if (popup.is_object())",
        "if (popup.is_array())",
    ),
    # chargen (#153, #189): each choice by dao::chargen from what the screen
    # offers then, refused where it greys it out; every point or none; the end.
    Mutant(
        "chargen takes no background by a word of its name",
        CHARGEN,
        'full.find(" " + name + " ")',
        'full.find(name + "  ")',
    ),
    Mutant(
        "chargen takes a choice the screen greys out",
        CHARGEN,
        'if (found == nullptr || !found->value("enabled", false))',
        "if (found == nullptr)",
    ),
    Mutant(
        "chargen takes wizard for no class",
        CHARGEN,
        'name = name == "wizard" ? "mage" : name;',
        'name = name == "mage" ? "wizard" : name;',
    ),
    Mutant(
        "chargen takes no voice by its sound set's last word",
        CHARGEN,
        "asked == set || asked == word ||",
        "asked == set ||",
    ),
    Mutant(
        "chargen offers a locked ability",
        CHARGEN,
        '!= "not yet"',
        '!= "never"',
    ),
    Mutant(
        "chargen spends some of the points",
        CHARGEN,
        "if (raising != attributePoints || std::cmp_not_equal(",
        "if (raising > attributePoints || std::cmp_not_equal(",
    ),
    Mutant(
        "chargen finishes with finish false",
        CHARGEN,
        'if (args_.Bool("finish", true))',
        'if (!args_.Has("offers"))',
    ),
    Mutant(
        "chargen ends at the first difficulty",
        CHARGEN,
        'asked["difficulty"] = *difficulty_;',
        'asked["difficulty"] = 0;',
    ),
    Mutant(
        "chargen reckons the origin's row wrong",
        CHARGEN,
        "(race_ * 1000) + (class_ * 100) + background_",
        "(race_ * 1000) + (class_ * 10) + background_",
    ),
    Mutant(
        "chargen says the exit the other way",
        CHARGEN,
        'spending ? "full" : "quick play"',
        'spending ? "quick play" : "full"',
    ),
    Mutant(
        "chargen says nothing was chosen where some was",
        CHARGEN,
        "return chose_.empty() ?",
        "return !chose_.empty() ?",
    ),
    # One measure for every angle (#143).
    Mutant(
        "the camera's facing is the way from it, not the way it looks",
        SIGHT,
        "return std::remainder(90.0F - (yaw * kDegrees), 360.0F);",
        "return std::remainder((yaw * kDegrees) - 90.0F, 360.0F);",
    ),
    Mutant(
        "a facing is said past -180 to 180",
        SIGHT,
        "return std::remainder(90.0F - (yaw * kDegrees), 360.0F);",
        "return 90.0F - (yaw * kDegrees);",
    ),
    Mutant(
        "a facing asked for turns the camera the other way",
        SIGHT,
        "return (90.0F - degrees) / kDegrees;",
        "return (degrees - 90.0F) / kDegrees;",
    ),
    Mutant(
        "pitchDegrees is read from straight above",
        SIGHT,
        "return (pitch * kDegrees) - 90.0F;",
        "return pitch * kDegrees;",
    ),
    Mutant(
        "a pitch asked for is taken from straight above",
        SIGHT,
        "return (degrees + 90.0F) / kDegrees;",
        "return degrees / kDegrees;",
    ),
    # Ghilana's own (src/ghilana, #128): the copy the game takes, a table as
    # the game merges it, what conflicts and query count, and the text and
    # files ghilana writes. `--suite ghilana`.
    # Blender's split vertices joined back into the chunk's own (#139).
    Mutant(
        "ghilana's export writes no vertex's place in its chunk",
        "src/ghilana/Gltf.cpp",
        "attributes[std::string(kOrderAttribute)] = out_.Floats(order, 1, kVertices);",
        "(void)order;",
        "ghilana",
    ),
    Mutant(
        "ghilana joins a mesh whose triangles are not the original's",
        "src/ghilana/GltfImport.cpp",
        "    if (had != now)\n",
        "    if (false)\n",
        "ghilana",
    ),
    Mutant(
        "ghilana takes a triangle from another corner for another triangle",
        "src/ghilana/GltfImport.cpp",
        "if (b < a && b <= c)",
        "if (false)",
        "ghilana",
    ),
    Mutant(
        "ghilana keeps Blender's tangents of an unedited shape",
        "src/ghilana/GltfImport.cpp",
        "const bool shaped = Shaped(data, original, v);",
        "const bool shaped = Shaped(data, original, v) && v > count;",
        "ghilana",
    ),
    Mutant(
        "ghilana keeps the original's place over an edit",
        "src/ghilana/GltfImport.cpp",
        "        return 1e-4F;",
        "        return 1.0F;",
        "ghilana",
    ),
    # The index's directories kept between processes (#146).
    Mutant(
        "ghilana keeps no directories between processes",
        "src/ghilana/GameFiles.cpp",
        "        options.cache = paths_.cache;\n",
        "",
        "ghilana",
    ),
    # A staged plugins folder (#145).
    Mutant(
        "ghilana reads the game's plugins though --plugins names others",
        "src/ghilana/GameFiles.cpp",
        "        options.plugins = paths_.plugins;\n",
        "",
        "ghilana",
    ),
    Mutant(
        "ghilana's environment does not say a plugins folder was given",
        "src/ghilana/ToolsFiles.cpp",
        "!where.plugins.empty()",
        "false",
        "ghilana",
    ),
    Mutant(
        "ghilana taken: the copy the game takes is one it has, else none",
        "src/ghilana/GameFiles.cpp",
        "return !copies.empty() && copies.front().unused == dao::core::install::Unused::No ? &copies.front() : nullptr;",
        "return !copies.empty() ? &copies.front() : nullptr;",
        "ghilana",
    ),
    Mutant(
        "ghilana cell: a number matches only an equal number",
        "src/ghilana/Tables.cpp",
        "return cell.get<double>() == wanted.get<double>();",
        "return true;",
        "ghilana",
    ),
    Mutant(
        "ghilana cell: text matches without case",
        "src/ghilana/Tables.cpp",
        "return Lower(TextOf(cell)) == Lower(TextOf(wanted));",
        "return TextOf(cell) == TextOf(wanted);",
        "ghilana",
    ),
    Mutant(
        "ghilana table: its files in the game's order",
        "src/ghilana/ToolsData.cpp",
        "files.Installed().SortFiles(taken);",
        "(void)taken;",
        "ghilana",
    ),
    Mutant(
        "ghilana table: a row after an unreadable file is unsure",
        "src/ghilana/ToolsData.cpp",
        "            unreadableBefore = true;\n",
        "",
        "ghilana",
    ),
    Mutant(
        "ghilana table: the copies a file's game copy shadows are said",
        "src/ghilana/ToolsData.cpp",
        "        for (std::size_t i = 1; i < copies.size(); ++i)\n        {\n            read.shadowed",
        "        for (std::size_t i = 2; i < copies.size(); ++i)\n        {\n            read.shadowed",
        "ghilana",
    ),
    Mutant(
        "ghilana table: the rows the game drops are left out unless asked",
        "src/ghilana/ToolsData.cpp",
        "if (!dropped && !row.taken)",
        "if (dropped && !row.taken)",
        "ghilana",
    ),
    # "the merge is given each part's source" went (#130): DAOLib's m2da::Merge takes the
    # tables alone since the readers moved there, each row's file named by the caller after,
    # so its text was in Tables.cpp no more and the suite stopped before building anything.
    Mutant(
        "ghilana conflicts: copies of one source are no conflict across sources",
        "src/ghilana/ToolsFiles.cpp",
        "if (count < 2 || (across && sources.size() < 2))",
        "if (count < 2 || (across && sources.empty()))",
        "ghilana",
    ),
    Mutant(
        "ghilana conflicts: a source loses where another's copy is taken",
        "src/ghilana/ToolsFiles.cpp",
        "taken->source != *source",
        "taken->source == *source",
        "ghilana",
    ),
    Mutant(
        "ghilana query: the game's copy of each resource unless all are asked",
        "src/ghilana/ToolsData.cpp",
        "const bool game = &copy == GameFiles::Taken(copies);",
        "const bool game = true;",
        "ghilana",
    ),
    Mutant(
        "ghilana query: a dotted path through a list reaches each element",
        "src/ghilana/ToolsData.cpp",
        "    if (node.is_array())\n",
        "    if (node.is_array() && path.empty())\n",
        "ghilana",
    ),
    Mutant(
        "ghilana query: a bare name is found in a list's elements",
        "src/ghilana/ToolsData.cpp",
        "it.value().is_array() && i < it.value().size()",
        "it.value().is_null() && i < it.value().size()",
        "ghilana",
    ),
    Mutant(
        "ghilana query: group_by counts each element's value",
        "src/ghilana/ToolsData.cpp",
        "for (const auto &key : GroupKeys(json, ask.groupBy))",
        "for (const auto &key : {GroupKeys(json, ask.groupBy).front()})",
        "ghilana",
    ),
    Mutant(
        "ghilana query: matched names only the elements of a list",
        "src/ghilana/ToolsData.cpp",
        "                if (found.listed)\n",
        "                if (found.listed || !found.listed)\n",
        "ghilana",
    ),
    Mutant(
        "ghilana query: a field through a list answers every element's value",
        "src/ghilana/ToolsData.cpp",
        "else if (found.size() == 1 && !found.front().listed)",
        "else if (!found.empty())",
        "ghilana",
    ),
    Mutant(
        "ghilana extract: never into the game's folder",
        "src/ghilana/ToolsFiles.cpp",
        "if (Inside(to, files.Where().game))",
        "if (false)",
        "ghilana",
    ),
    Mutant(
        "ghilana extract: all_copies keeps two copies of one source apart",
        "src/ghilana/ToolsFiles.cpp",
        "(copy.archive == kLoose ? relative.parent_path() : relative)",
        "relative.parent_path()",
        "ghilana",
    ),
    Mutant(
        "ghilana json: a GFF3 struct past the depth is its field count",
        "src/ghilana/FileJson.cpp",
        "if (depth > limits.depth)",
        "if (false)",
        "ghilana",
    ),
    Mutant(
        "ghilana json: a column is named by its hash where a name is known",
        "src/ghilana/FileJson.cpp",
        "    if (!name.empty())\n    {\n        return std::string(name);",
        "    if (false)\n    {\n        return std::string(name);",
        "ghilana",
    ),
    Mutant(
        "ghilana text: a pattern matches without case",
        "src/ghilana/Text.cpp",
        "(pattern[p] == '?' || LowerChar(pattern[p]) == LowerChar(text[t]))",
        "(pattern[p] == '?' || pattern[p] == text[t])",
        "ghilana",
    ),
    Mutant(
        "ghilana text: a path's backslashes are slashes",
        "src/ghilana/Text.cpp",
        "        if (c == '\\\\')",
        "        if (false)",
        "ghilana",
    ),
    Mutant(
        "ghilana text: a surrogate pair is one character",
        "src/ghilana/Text.cpp",
        "Append(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));",
        "Append(out, kReplacement);",
        "ghilana",
    ),
    Mutant(
        "ghilana names: #label is read as the label it writes",
        "src/ghilana/Names.cpp",
        "std::from_chars(name.data() + 1, end, label)",
        "std::from_chars(name.data(), end, label)",
        "ghilana",
    ),
    Mutant(
        "ghilana png: eight bits a channel",
        "src/ghilana/Png.cpp",
        "header.bit_depth = 8;",
        "header.bit_depth = 16;",
        "ghilana",
    ),
    # A model as glTF and back (#132): the axes turned and turned back, a
    # blend index made the model's joint, a joint's inverse bind matrix, a
    # node placed under its parent, a weight to no bone refused.
    Mutant(
        "ghilana gltf: the game's Z up is glTF's Y up",
        "src/ghilana/GltfMath.cpp",
        "return {x, z, -y};",
        "return {x, y, z};",
        "ghilana",
    ),
    Mutant(
        "ghilana gltf: glTF's Y up turned back to the game's Z up",
        "src/ghilana/GltfMath.cpp",
        "return {v[0], -v[2], v[1]};",
        "return {v[0], v[2], v[1]};",
        "ghilana",
    ),
    Mutant(
        "ghilana gltf: a node is placed under its parent",
        "src/ghilana/GltfMath.cpp",
        "? Times(world[static_cast<std::size_t>(node.parent)], local)",
        "? local",
        "ghilana",
    ),
    Mutant(
        "ghilana gltf: a blend index is the joint of its slot's bone",
        "src/ghilana/Gltf.cpp",
        "return static_cast<std::uint16_t>(of[static_cast<std::size_t>(bone)]);",
        "return static_cast<std::uint16_t>(bone);",
        "ghilana",
    ),
    Mutant(
        "ghilana gltf: a joint's inverse bind matrix starts at the mesh node",
        "src/ghilana/Gltf.cpp",
        "const Mat4 m = Times(Inverse(world_[static_cast<std::size_t>(joint)]), meshPlace);",
        "const Mat4 m = Inverse(world_[static_cast<std::size_t>(joint)]);",
        "ghilana",
    ),
    Mutant(
        "ghilana import: a weight to no bone of the model is refused",
        "src/ghilana/GltfImport.cpp",
        "        if (bone < 0)\n        {\n            std::string message = label;",
        "        if (bone < -1)\n        {\n            std::string message = label;",
        "ghilana",
    ),
    # New meshes, and a chunk over too many bones split (#136).
    Mutant(
        "ghilana import: a second mesh claiming a chunk is a new chunk",
        "src/ghilana/GltfImport.cpp",
        "if (chunk && !given[*chunk])",
        "if (chunk)",
        "ghilana",
    ),
    Mutant(
        "ghilana import: a new chunk counts in only its own bones",
        "src/ghilana/GltfImport.cpp",
        "std::vector<std::int32_t> bonesUsed = added ? std::vector<std::int32_t>{} : ref.bonesUsed;",
        "std::vector<std::int32_t> bonesUsed = ref.bonesUsed;",
        "ghilana",
    ),
    Mutant(
        "ghilana split: a piece takes as many bones as allowed",
        "src/ghilana/GltfImport.cpp",
        "return group.bones.size() + more <= most;",
        "return group.bones.size() + more < most;",
        "ghilana",
    ),
    Mutant(
        "ghilana split: a vertex's blend indices count in its piece's bones",
        "src/ghilana/GltfImport.cpp",
        "? slotOf(skin.bonesUsed.at(static_cast<std::size_t>(skin.slots[v][k]))) : 0;",
        "? skin.slots[v][k] : 0;",
        "ghilana",
    ),
    # New bones (#137).
    Mutant(
        "ghilana bones: a joint that is no bone of the model is a new bone",
        "src/ghilana/GltfImport.cpp",
        "if (made_.contains(j) || depth > 1000 || IsBone(name))",
        "if (true)",
        "ghilana",
    ),
    Mutant(
        "ghilana bones: a new bone goes under the nearest node the model has",
        "src/ghilana/GltfImport.cpp",
        "return {*known, like, daoWorlds_[static_cast<std::size_t>(*known)]};",
        "return {-1, like, daoWorlds_[static_cast<std::size_t>(*known)]};",
        "ghilana",
    ),
    Mutant(
        "ghilana bones: a new bone's index follows the model's bones",
        "src/ghilana/GltfImport.cpp",
        "static_cast<std::int32_t>(tree_.totalBones + newBones_.size())",
        "static_cast<std::int32_t>(newBones_.size())",
        "ghilana",
    ),
    Mutant(
        "ghilana bones: a new bone is placed relative to its parent",
        "src/ghilana/GltfImport.cpp",
        "const Mat4 local = Times(Inverse(anchor.world), gltfWorlds_.at(j));",
        "const Mat4 local = gltfWorlds_.at(j);",
        "ghilana",
    ),
    # A texture's round trip (#135).
    Mutant(
        "ghilana textures: a normal map is written in the game's layout",
        "src/ghilana/ToolsTextures.cpp",
        "if (asked.empty() && normal)",
        "if (asked.empty() && !normal)",
        "ghilana",
    ),
    Mutant(
        "ghilana textures: a side that is no power of two is refused, saying so",
        "src/ghilana/ToolsTextures.cpp",
        "if (!PowerOfTwo(image.width) || !PowerOfTwo(image.height))",
        "if (false)",
        "ghilana",
    ),
    # A head's morphs (#138).
    Mutant(
        "ghilana morphs: a morph names a model by one of its parts",
        "src/ghilana/ToolsModels.cpp",
        "return Lower(Utf8(p)) == model; }))",
        "return Lower(Utf8(p)) != model; }))",
        "ghilana",
    ),
    Mutant(
        "ghilana morphs: a vertex count a morph cannot follow is refused",
        "src/ghilana/ToolsModels.cpp",
        "given->values[0].size() != node.places.size())",
        "given->values[0].size() == 0)",
        "ghilana",
    ),
    # A creature of a mod's own (#141).
    Mutant(
        "ghilana tables: a new row's ID already in the merged table is refused",
        "src/ghilana/ToolsTemplates.cpp",
        "if (taken.contains(id) ||",
        "if (false ||",
        "ghilana",
    ),
    Mutant(
        "ghilana tables: a row is copied from the one asked for",
        "src/ghilana/ToolsTemplates.cpp",
        "return IdOf(row, *idColumn) == copy; });",
        "return IdOf(row, *idColumn) != copy; });",
        "ghilana",
    ),
    Mutant(
        "ghilana tables: the merged table is the name without _base",
        "src/ghilana/ToolsTemplates.cpp",
        "std::string prefix = Stem(table);",
        "std::string prefix = Stem(table).substr(0, Stem(table).find('_'));",
        "ghilana",
    ),
    Mutant(
        "ghilana tables: an item variation's ID is a byte",
        "src/ghilana/ToolsTemplates.cpp",
        "(id < 0 || id > 255)",
        "(id < 0 || id > 65535)",
        "ghilana",
    ),
    Mutant(
        "ghilana templates: a field in a struct is found by its path",
        "src/ghilana/ToolsTemplates.cpp",
        "in = in ? in->Child(label.substr(0, slash)) : std::nullopt;",
        "in = in;",
        "ghilana",
    ),
    # An id's text as the game shows it, of the tables in its order (#169).
    Mutant(
        "ghilana strings: an id's text is the first table's in the game's order",
        "src/ghilana/ToolsData.cpp",
        'item["text"] = Utf8(hits.front().text);',
        'item["text"] = Utf8(hits.back().text);',
        "ghilana",
    ),
    # A data table's cells by DAOLib's schema (#170).
    Mutant(
        "ghilana references: a table's string id column by the schema, whatever its name",
        "src/ghilana/ToolsData.cpp",
        "holding.holds == Holds::StrRef ? IsStringId(value)",
        "holding.holds == Holds::Plain ? IsStringId(value)",
        "ghilana",
    ),
    Mutant(
        "ghilana references: a schema's resource column names its one extension's file",
        "src/ghilana/ToolsData.cpp",
        "if (holding.holds == Holds::Resource)",
        "if (holding.holds == Holds::StrRef)",
        "ghilana",
    ),
    # A template's row fields followed to their rows (#171).
    Mutant(
        "ghilana references: a schema's row field is followed to its row",
        "src/ghilana/ToolsData.cpp",
        "if (holding.holds == Holds::Row && value.is_number_integer())",
        "if (holding.holds == Holds::Plain && value.is_number_integer())",
        "ghilana",
    ),
    Mutant(
        "ghilana references: a row field of -1 names no row",
        "src/ghilana/ToolsData.cpp",
        "if (id >= 0 && rows_.size() < limit_)",
        "if (rows_.size() < limit_)",
        "ghilana",
    ),
    Mutant(
        "ghilana references: a row is the one the game keeps of its ID",
        "src/ghilana/ToolsData.cpp",
        "return r.taken && r.id == id;",
        "return r.id != id;",
        "ghilana",
    ),
    Mutant(
        "ghilana references: a table with no Label names its rows by its first text column",
        "src/ghilana/ToolsData.cpp",
        "        if (merged.columns[c].type == dao::core::gff4::Type::String)",
        "        if (c > merged.columns.size())",
        "ghilana",
    ),
    # A mod packed for players (#140).
    Mutant(
        "ghilana mods: a talk table goes in the add-in's talktables",
        "src/ghilana/ToolsMods.cpp",
        'const bool talk = Extension(file.name) == "tlk";',
        'const bool talk = Extension(file.name) == "none";',
        "ghilana",
    ),
    Mutant(
        "ghilana mods: two files of one name are refused",
        "src/ghilana/ToolsMods.cpp",
        "if (!names.insert(given.name).second)",
        "if (false)",
        "ghilana",
    ),
    # A level, its sky, a cutscene and a stage (#163).
    Mutant(
        "ghilana level: a placed model where its room has it, not the level",
        "src/ghilana/ToolsLevels.cpp",
        '{"at", level::InLevel(room, placed, {0, 0, 0})},',
        '{"at", placed.at},',
        "ghilana",
    ),
    Mutant(
        "ghilana level: a placed model turned by its own turn alone",
        "src/ghilana/ToolsLevels.cpp",
        '{"turn", level::Then(placed.turn, room.turn)},',
        '{"turn", placed.turn},',
        "ghilana",
    ),
    Mutant(
        "ghilana level: a lamp where its layout has it, not the level",
        "src/ghilana/ToolsLevels.cpp",
        '{"at", level::InLevel(room, placed, lamp.at)},',
        '{"at", lamp.at},',
        "ghilana",
    ),
    Mutant(
        "ghilana level: a room's layout is listed as a placed model",
        "src/ghilana/ToolsLevels.cpp",
        "            if (IsLayout(placed))\n",
        "            if (IsLayout(placed) && placed.scale < 0)\n",
        "ghilana",
    ),
    Mutant(
        "ghilana cutscene: an action to the end gives 1e10 for its stop",
        "src/ghilana/ToolsLevels.cpp",
        '{"stop", a.stop >= 1e9F ? Json("end") : Json(a.stop)},',
        '{"stop", Json(a.stop)},',
        "ghilana",
    ),
    Mutant(
        "ghilana cutscene: a spoken line without its text",
        "src/ghilana/ToolsLevels.cpp",
        'line["text"] = Utf8(found->text);',
        "(void)found;",
        "ghilana",
    ),
    Mutant(
        "ghilana cutscene: a time sampled off the cutscene's frames",
        "src/ghilana/ToolsLevels.cpp",
        'asked.at = cutscene::FrameTime(static_cast<float>(args.Number("at")), cut.fps);',
        'asked.at = static_cast<float>(args.Number("at"));',
        "ghilana",
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


def sync(worker: Path, suite: Suite) -> None:
    """Brings a worker's copy up to the tree: a file is written only where it
    differs, so what did not change is not built again."""
    wanted: set[Path] = set()
    sources = [REPO / name for name in FILES if (REPO / name).exists()]
    for tree in suite.trees:
        sources += [f for f in (REPO / tree).rglob("*") if f.is_file()]
    for source in sources:
        target = worker / source.relative_to(REPO)
        wanted.add(target)
        data = source.read_bytes()
        if not target.exists() or target.read_bytes() != data:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    for tree in suite.trees:
        for stale in (worker / tree).rglob("*"):
            if stale.is_file() and stale not in wanted:
                stale.unlink()


def made_with(suite: Suite) -> str:
    """The build program the tree's own build of the suite's preset was configured with."""
    cache = REPO / "build" / suite.preset / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_MAKE_PROGRAM:"):
                return line.partition("=")[2].strip()
    return ""


def tests_of(worker: Path, suite: Suite) -> list[str]:
    return [str(worker / "build" / suite.preset / suite.tests), *suite.args]


def prepare(worker: Path, jobs: int, suite: Suite) -> str:
    """A worker's copy, configured and built as the tree is. Answers what went
    wrong, or nothing."""
    sync(worker, suite)
    if not (worker / "build" / suite.preset / "build.ninja").exists():
        command = [
            "cmake",
            "--preset",
            suite.preset,
            f"-DVCPKG_INSTALLED_DIR={suite.packages.as_posix()}",
            "-DVCPKG_MANIFEST_INSTALL=OFF",
            *suite.configure,
        ]
        # Ninja as the tree's own build found it: it need not be on the path
        # of the shell this runs in.
        if ninja := made_with(suite):
            command.append(f"-DCMAKE_MAKE_PROGRAM={ninja}")
        configured = run(command, worker)
        if configured.returncode != 0:
            return (configured.stdout + configured.stderr)[-2000:]
    built = build(worker, jobs, suite)
    if built.returncode != 0:
        return (built.stdout + built.stderr)[-2000:]
    return ""


def build(worker: Path, jobs: int, suite: Suite) -> subprocess.CompletedProcess[str]:
    command = ["cmake", "--build", "--preset", suite.preset, "--parallel", str(jobs)]
    if suite.target:
        command += ["--target", suite.target]
    return run(command, worker)


def test(mutant: Mutant, worker: Path, suite: Suite) -> str:
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
        if build(worker, 1, suite).returncode != 0:
            return "did not compile"
        try:
            result = run(tests_of(worker, suite), worker, timeout=TIMEOUT_S)
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
    parser.add_argument(
        "--suite",
        choices=sorted(SUITES),
        default="core",
        help="core (src/core, the default) or ghilana (src/ghilana, built 64-bit)",
    )
    args = parser.parse_args()
    suite = SUITES[args.suite]
    chosen = [
        m
        for m in MUTANTS
        if m.suite == args.suite
        and (not args.names or any(w in m.name for w in args.names))
    ]
    if not chosen:
        print("no mutant's name contains any of those")
        return 1
    if wrong := unapplied(chosen):
        print("\n".join(wrong))
        return 1
    if not suite.packages.exists():
        print(
            f"no {suite.packages}: build the {suite.preset} preset once "
            f"(tools\\build.ps1 -Preset {suite.preset})"
        )
        return 1
    count = max(1, min(args.jobs, len(chosen)))
    workers = [WORK / suite.work / f"w{n}" for n in range(count)]
    started = time.monotonic()

    # The copies, built side by side: each with its share of the cores.
    share = max(1, (os.cpu_count() or 2) // count)
    problems: dict[Path, str] = {}

    def ready(worker: Path) -> None:
        problems[worker] = prepare(worker, share, suite)

    threads = [threading.Thread(target=ready, args=(w,)) for w in workers]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    for worker, problem in problems.items():
        if problem:
            print(f"{worker.name} does not configure or build:\n{problem}")
            return 1
    if run(tests_of(workers[0], suite), workers[0]).returncode != 0:
        print(f"the unmutated {args.suite} does not pass its tests; fix that first")
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
                verdict = test(mutant, worker, suite)
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
