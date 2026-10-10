# Eluvian MCP

A test bench inside **Dragon Age: Origins** for AI agents and scripts: a [DAOLib](https://github.com/Whocares66-Dev/DAOLib) plugin that serves the running game over [MCP](https://modelcontextprotocol.io) and plain HTTP on `127.0.0.1`. What [devbench](https://github.com/alandtse/devbench) is for Skyrim.

An agent asks the game what it wants to know (the creatures around, their health and effects, the quests, what the interface has up) and gives it orders by id, then waits for the event it expects instead of sleeping and reading logs. It can stage a test's case, walk the party across the world, loot, level up, talk, and play a whole campaign with no screen at all. Other DAOLib plugins add tools and events of their own through a small C interface, [`src/api/EluvianAPI.h`](src/api/EluvianAPI.h) (MIT).

It is tested on the GOG build of the game, version 1.05.13263.0 (DAOLib's address library maps EA's and Steam's too), and answers on `127.0.0.1` only with no authentication: a development bench, not something to ship to players.

## Docs

- [dev/PLAYBOOK.md](dev/PLAYBOOK.md): how an agent drives a test, with the calls for each job.
- [dev/DESIGN.md](dev/DESIGN.md): what it does and why, every tool and event, and the C interface for other plugins.
- [dev/TESTING.md](dev/TESTING.md): how it is checked in the game, and what was seen there.

## Connecting

An agent connects through the bridge, `tools\eluvian_bridge.py`, a stdio MCP server that outlives the game. It forwards to the game while it runs, and adds `game_launch`, `game_quit`, `game_status`, `game_saves` and the claim that tells agents sharing one game whose it is. This repository's `.mcp.json` starts it; from another:

```sh
claude mcp add eluvian -- python C:\project\DAOEluvianMCP\tools\eluvian_bridge.py
```

A script calls one tool from the shell and reads the answer as JSON:

```powershell
python tools\eluvian_bridge.py --call game_launch '{"owner":"me","load":{"character":"Lyna","kind":"manual","number":1}}'
python tools\eluvian_bridge.py --call creatures '{"side":"hostile"}'
```

Or straight to the running game, on port 8930 or the next free one (`eluvian.json` beside the logs says which):

```sh
curl http://127.0.0.1:8930/api/health
curl -X POST http://127.0.0.1:8930/api/tool/creatures -d '{"side":"hostile"}'
```

## What is here

- `src/core`: the bench without the game (HTTP, MCP, the tool registry, the event bus, and the tools that decide over other tools' answers), tested.
- `src/game`: the tools that read and drive the game, through DAOLib.
- `src/plugin`, `src/api`: the plugin itself, and the C interface other plugins include.
- `tools/`: building, deploying, the bridge, and `smoke.py`, every tool once in the game.
- `dev/`: the docs above.

## Building

Visual Studio 2022 with the C++ workload, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set, and Python 3. Everything is 32-bit, as the game is.

```powershell
git submodule update --init --recursive   # DAOLib, and its address library
.\tools\build.ps1 -Preset debug -Test     # the plugin, DAOLib's loader, the tests
.\tools\build.ps1 -Preset release -Test   # what the game plays
.\tools\deploy.ps1                        # the release build into the game (with the game closed)
.\tools\deploy.ps1 -Remove                # take it out again
```

Logs are written to `Documents\BioWare\Dragon Age\daolib\`; `eluvian.log` has every tool call with its arguments and answer.

## Working on Eluvian

[CLAUDE.md](CLAUDE.md) has the project's rules: the one architectural rule, threads, the checks before a commit and before a merge, and how a change is seen in play.

## License

GPLv3 ([LICENSE](LICENSE)), as DAOLib. `src/api/EluvianAPI.h` alone is MIT, so a plugin under any licence can include it.

## Thanks

- [alandtse](https://github.com/alandtse) for [devbench](https://github.com/alandtse/devbench), the bench for Skyrim that Eluvian follows: its tools, its scenarios, and its rule that a tool is named for its job.
- [DAOLib](https://github.com/Whocares66-Dev/DAOLib) and its [address library](https://github.com/Whocares66-Dev/DAOAddressLibrary), which give it the game.
- The libraries it builds on: [nlohmann/json](https://github.com/nlohmann/json), [spdlog](https://github.com/gabime/spdlog), [Microsoft Detours](https://github.com/microsoft/Detours) and [Catch2](https://github.com/catchorg/Catch2).
