# Eluvian MCP

A test bench inside **Dragon Age: Origins** for AI agents and scripts: a [DAOLib](https://github.com/Whocares66-Dev/DAOLib) plugin that serves the running game over [MCP](https://modelcontextprotocol.io) and plain HTTP on `127.0.0.1`. Inspired by Skyrim's [devbench](https://github.com/alandtse/devbench).

An agent asks the game what it wants to know (the creatures around, their health and effects, the quests, what the interface has up) and gives it orders by id, then waits for the event it expects instead of sleeping and reading logs. It can stage a test's case, walk the party across the world, loot, level up, talk, and play a whole campaign with no screen at all. Other DAOLib plugins add tools and events of their own through a small C interface, [`src/api/EluvianAPI.h`](src/api/EluvianAPI.h) (MIT).

It is tested on the GOG build of the game, version 1.05.13263.0 (DAOLib's address library maps EA's and Steam's too), and answers on `127.0.0.1` only with no authentication: a development bench, not something to ship to players.

## Docs

**[whocares66-dev.github.io/DAOEluvianMCP](https://whocares66-dev.github.io/DAOEluvianMCP/)**: every tool with its arguments, the events, and the C interface, generated from the code (`python tools/build_docs.py --serve` builds it locally).

- [dev/PLAYBOOK.md](dev/PLAYBOOK.md): how an agent drives a test, with the calls for each job.
- [dev/DESIGN.md](dev/DESIGN.md): what it does and why, every tool and event, and the C interface for other plugins.
- [dev/TESTING.md](dev/TESTING.md): how it is checked in the game, and what was seen there.

## Installing

Eluvian is a DAOLib plugin, so the game has [DAOLib](https://github.com/Whocares66-Dev/DAOLib/releases) first: its zip, extracted into the game's install folder, the one that holds `bin_ship`. An [Eluvian release](https://github.com/Whocares66-Dev/DAOEluvianMCP/releases) is extracted the same way, into the same folder, with the game closed. All of it lands in one folder, `bin_ship\daolib\plugins\eluvian\`: `eluvian.dll`, `eluvian.pdb` (which lets a crash log name Eluvian's own code), the bridge `eluvian_bridge.py`, and `VERSION.txt`, which names the DAOLib release it was built on, the oldest it works with. Deleting that folder takes Eluvian out. [CHANGELOG.md](CHANGELOG.md) says what each release brought.

Start the game as always. `Documents\BioWare\Dragon Age\daolib\loader.log` then says that `eluvian` was loaded, and `eluvian.log` beside it has the port Eluvian serves on and every tool call with its arguments and answer.

## Connecting

An agent connects through the bridge, `eluvian_bridge.py`, a stdio MCP server that outlives the game and needs only Python 3. It forwards to the game while it runs, and adds `game_launch`, `game_quit`, `game_status`, `game_saves` and the claim that tells agents sharing one game whose it is. Started from the game's folder it takes that game as its own:

```sh
claude mcp add eluvian -- python "C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins\bin_ship\daolib\plugins\eluvian\eluvian_bridge.py"
```

In a clone of this repository the same file is `tools\eluvian_bridge.py`, which `.mcp.json` starts; there it takes the GOG install's game unless `--exe` names another.

The game runs one at a time, except as a named instance: a second copy beside the first with its own saves, settings, logs and port, which a loader of DAOLib later than 0.1.0 makes of a copy started with a name. A bridge drives one copy; `game_instance {"name": "mytest"}` makes it that instance's, and `--instance mytest` starts it so. [dev/PLAYBOOK.md](dev/PLAYBOOK.md) has the calls.

A script calls one tool from the shell and reads the answer as JSON (here from a clone):

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
- `tools/`: building, deploying, packaging and releasing, the bridge, `smoke.py` (every tool once in the game) and `build_docs.py` (the docs site).
- `docs/`: the docs site's theme and the reference it is built from (`_data/eluvian.json`, what `eluvian_describe` prints).
- `dev/`: the docs above.

## Building

Only to work on Eluvian: to use it, install a release. Visual Studio 2022 with the C++ workload, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set, and Python 3. Everything is 32-bit, as the game is.

```powershell
git submodule update --init --recursive   # DAOLib, and its address library
.\tools\build.ps1 -Preset debug -Test     # the plugin, DAOLib's loader, the tests
.\tools\build.ps1 -Preset release -Test   # what the game plays
.\tools\deploy.ps1                        # the release build into the game (with the game closed)
.\tools\deploy.ps1 -Remove                # take it out again
.\tools\package.ps1                       # the release build as the zip a release carries
```

[dev/RELEASING.md](dev/RELEASING.md) is how a release is cut.

## Working on Eluvian

[CLAUDE.md](CLAUDE.md) has the project's rules: the one architectural rule, threads, the checks before a commit and before a merge, and how a change is seen in play.

## License

GPLv3 ([LICENSE](LICENSE)), as DAOLib. `src/api/EluvianAPI.h` alone is MIT, so a plugin under any license can include it.

## Thanks

- [alandtse](https://github.com/alandtse) for [devbench](https://github.com/alandtse/devbench)
- [DAOLib](https://github.com/Whocares66-Dev/DAOLib)
- [DAOAddressLibrary](https://github.com/Whocares66-Dev/DAOAddressLibrary)
- [nlohmann](https://github.com/nlohmann) for [JSON for Modern C++](https://github.com/nlohmann/json)
- [gabime](https://github.com/gabime) for [spdlog](https://github.com/gabime/spdlog)
- [Microsoft](https://github.com/microsoft) for [Detours](https://github.com/microsoft/Detours)
- [Catch2](https://github.com/catchorg/Catch2)
