# Eluvian MCP -- project context

A test bench inside Dragon Age: Origins for agents: `eluvian.dll`, a DAOLib plugin serving the running game over MCP (`POST /mcp`) and plain HTTP (`/api/`) on 127.0.0.1. An agent asks the game questions and gives it orders instead of driving it by screenshots and reading logs after. Skyrim's devbench (`C:\modding\devbench`) is the model. `dev/DESIGN.md` is what it does and why, the tools, the events, the C ABI, and what DAOLib gives it. `dev/TESTING.md` is how to check it in the game and what was seen there. `dev/PLAYBOOK.md` is for the agent driving a test through it: the rules, a turn, the mod's names. `README.md` is how to build, deploy and connect.

Markdown here is not hard-wrapped: one line per paragraph or list item.

## The target and the siblings

DAOLib's `CLAUDE.md` has the game, the one exe build every address is from, the toolchain and where logs go. Everything is 32-bit x86; `CMakeLists.txt` refuses a 64-bit configure.

- `extern/DAOLib` is a git submodule (DAOLib's own repository), built with this one as DAOTurnBased builds it; without the submodule CMake falls back to the sibling `..\DAOLib`, and `-DDAOLIB_DIR=../DAOLib` builds against the sibling's working tree meanwhile. A change DAOLib needs is asked of the DAOLib session (lib-agent) or made in DAOLib's own repository with its rules, then the submodule moved to it.
- `C:\project\DAOTurnBased` is the turn-based mod, the first thing Eluvian tests. It registers its own tools and events through `src/api/EluvianAPI.h`; its session is turn-based-agent.

## Layout

```
src/core/     the bench without the game: HTTP parsing and routes, MCP's JSON-RPC, REST, the tool
              registry, the event bus, the queue to the frame thread, arguments, logs from a cursor.
              No <Windows.h>, no DAOLib, no dao:: type. Tested.
src/server/   the socket loop under core/Http.h: Winsock, 127.0.0.1, a thread per connection. Tested
              over loopback.
src/game/     the tools that read and drive the game, through DAOLib (inspect, script, order, game,
              health), and the engine's events on the bus. Verified by playing.
src/plugin/   DAOLib_Load, the frame pump, the host side of the C ABI (Eluvian_GetInterface)
src/api/      EluvianAPI.h: the C ABI other plugins include. MIT, unlike the rest (GPLv3).
tests/        Catch2 over src/core; tests/server over src/server
tools/        build.ps1, deploy.ps1, console.ps1, check-powershell.ps1 (DAOTurnBased's, for this tree);
              eluvian_bridge.py, the stdio MCP server an agent keeps across game restarts (.mcp.json),
              and test_bridge.py, run by ctest
dev/          the design
extern/DAOLib the submodule
```

**Eluvian is general, not any one mod's.** It serves the game and the bench; a mod's state, actions and events come from the mod through `src/api/EluvianAPI.h`, under its prefix, documented by the mod. A tool, event or doc line only one mod would want is that mod's to register, and no agent-facing text here names a mod (the turn-based mod was the first user; history in comments is fine). `dev/DESIGN.md`, Principles.

**The one architectural rule: `src/core` never includes `<Windows.h>`, links DAOLib or names a `dao::` type.** What can be decided without the game belongs there, with a test.

**Threads.** The listener's threads never touch game memory. A tool runs on the frame thread (`dao::OnFrame`, the scripts' thread, paused or not) unless declared listener-thread; `core/FrameQueue.h` takes it there and brings the answer back, and abandons a call no frame takes in time. Listener tools (`health`, `events`, `wait_for`, `log`) answer while frames stop. Script commands are not called while an area loads (lib-agent, 2026-09-30).

**Why no HTTP library.** cpp-httplib, the obvious one, refuses 32-bit Windows in vcpkg (2026-09-30); `core/Http.h` is the little HTTP the bench needs, tested.

**The game's speed belongs to one plugin.** Each plugin's copy of DAOLib hooks the frame's timer on its first `dao::clock::SetSpeed`, so two setting a speed multiply, and `Speed()` and `Held()` read back only the caller's own (DAOLib `ed55ab8`). In a turn-based test the mod owns it (`ai_turn_speed`); Eluvian neither sets nor reports it.

**Tool names reach a model**: letters, digits, `_` and `-` only (a plugin's: `turnbased_state`). Inspect kinds and event topics may use dots (`turnbased.state`, `turnbased.turn.ended`).

## Building

```powershell
.\tools\build.ps1 -Preset core -Test            # src/core and its tests only: the fast loop
.\tools\build.ps1 -Preset debug -Test           # eluvian.dll, DAOLib's loader, all tests
.\tools\build.ps1 -Preset debug -Format -Lint   # formatters, then clang-tidy, ruff and PSScriptAnalyzer
.\tools\deploy.ps1                              # into the game (closed); -Remove takes it out
```

## Before every commit

```powershell
.\tools\build.ps1 -Preset debug -Test -Format -Lint
.\tools\build.ps1 -Preset debug-asan -Test
.\tools\build.ps1 -Preset debug-analyze
```

Last green 2026-10-01, against DAOLib `79da8ee`: 103 tests, clang-tidy, ruff, PSScriptAnalyzer, ASan and /analyze clean; `-Preset core-cov -Coverage` shows 97% of `src/core`'s regions and 98% of its lines run by the tests. Seen in the game the same day, reads only (`dev/TESTING.md`): orders and a fight not yet.

## Writing files from the shell

The Bash tool turns `\\` into `\` and `\r`, `\n`, `\t` into control characters inside a heredoc (DAOTurnBased's note; it happened here too, 2026-09-30), and Python's text mode writes CRLF on Windows. Write sources through the Edit or Write tools, or from a Python file that writes bytes with LF.

## Style

As DAOLib's: clang-format's stock Microsoft style, lint for correctness only, `/W4 /WX`. Comments say why and what was read, with dates, not what the code does. A file's head says what it is for and how it fits. A tool's description is written for an agent reading it cold: what it answers, what to pass, what to do next.
