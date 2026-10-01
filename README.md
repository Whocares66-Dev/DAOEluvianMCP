# Eluvian MCP

A test bench inside **Dragon Age: Origins** (GOG Ultimate Edition, `DAOrigins.exe` 1.05.13263.0) for AI agents and scripts: a [DAOLib](https://github.com/Whocares66-Dev/DAOLib) plugin that serves the running game over [MCP](https://modelcontextprotocol.io) and plain HTTP on `127.0.0.1`, in the spirit of Skyrim's devbench.

An agent asks the game what it wants to know (the creatures, their health, their effects, whose turn it is) and gives it orders by id, then waits for the event it expects instead of sleeping and reading logs. Other DAOLib plugins add their own tools and events through a small C interface (`src/api/EluvianAPI.h`, MIT).

`dev/DESIGN.md` has the design: why, the tools, the events, and what comes next. `dev/PLAYBOOK.md` is how an agent drives a test through it.

## Tools

| Tool | What |
|---|---|
| `health` | Is the game alive and in what mode, answered even while it loads or hangs; who started it (`owner`) |
| `inspect` | `kind`: `game` (mode, pause, area, hero, party, a message box if one is up), `creatures` (positions, sides and groups, health, rooms and whether the party has seen them, what each is doing), `creature` (one in full, with its effects and abilities), `geometry` (distance, line of sight, the path walked), `placeables` (doors, chests, levers: state and lock), `dialogue` (the line spoken, the replies offered), `popup` (the game's message box), `window` (where the game's window is on the desktop), `cursor` (what the game says is under the real mouse), `data` (the data tree the interface is bound to, by path), and kinds plugins add |
| `order` | The player's orders by id: attack, move, ability, use; through the engine's own click functions or explore mode's click |
| `travel` | Walk a creature to a target or point, opening what is shut on the way; stops and says why when a fight, a conversation or a pause takes the journey from it |
| `dialogue` | Answer the conversation on the screen: choose a reply, continue, skip a cutscene |
| `camera` | Read it; face a target, follow a creature, turn: so a screenshot shows what the test means |
| `screen` | For the real mouse: a pixel to click a creature or object at (on its body, with nothing in front), a world point's pixel, what lies under a pixel; each as the game's pixels, a fraction of the picture and a desktop pixel |
| `script` | Any of the game's script commands by name, with its answer |
| `stage` | A test's set-up: conditions, health, cooldowns, a death, a spawn, a creature's group (a fight the party is not in), and mods' own actions |
| `screenshot` | The game as a picture, interface and all: its window drawn from inside the game (nothing over it gets in the way), or the game's own, as its screenshot key takes it |
| `game` | Pause, unpause; the saves with their area, name and time played; load one; save |
| `events`, `wait_for` | The event bus from a cursor; block until an event happens; `describe` lists the topics and what they hold |
| `scenario` | Steps in one call: tools, waits for events, pauses, checks; a transcript back |
| `log` | DAOLib's logs from a cursor; `eluvian.log` holds every tool call with its arguments, time and refusal |
| `game_saves`, `game_status`, `game_launch`, `game_quit` | The bridge's own, outside the game: the saves on disk, is it running and whose it is, start it (into a save, under an owner's name), close it |

## Building

As DAOLib: Visual Studio 2022 with the C++ workload, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set, Python 3, and for formatting and linting LLVM 22+ and PSScriptAnalyzer.

```powershell
git submodule update --init                     # extern\DAOLib
.\tools\build.ps1 -Preset debug -Test           # the plugin, DAOLib's loader, the tests
.\tools\deploy.ps1                              # into the game's bin_ship (game closed)
.\tools\deploy.ps1 -Remove                      # the plugin out again
```

## Connecting

Through the bridge, for an agent: `tools\eluvian_bridge.py` is a stdio MCP server the agent's client keeps across game restarts. It forwards to the game while it runs, lists the game's tools while it does not, and adds `game_status`, `game_launch` (never a second game) and `game_quit`. This repository's `.mcp.json` starts it; from another:

```sh
claude mcp add eluvian -- python C:\project\DAOEluvianMCP\tools\eluvian_bridge.py
```

Without an MCP client, a script or an agent calls one bridge tool from the command line and reads its answer as JSON; `game_launch`, `game_quit`, `game_saves` and `game_status` answer while no game runs:

```powershell
python tools\eluvian_bridge.py --call game_status
python tools\eluvian_bridge.py --call game_launch '{"owner": "my-name", "load": {"character": "Lyna", "kind": "manual", "number": 1}}'
```

`owner` is the name the caller goes by: `health` gives it back, so that agents sharing one game know whose a running one is (none means it was started by hand, the player's own). A launch answers `launched` false when a game already runs: read that before any other call.

Directly, for a quick look or a script: start the game; `eluvian.log` in `Documents\BioWare\Dragon Age\daolib\` says where it serves, and `eluvian.json` beside it has the port (8930, or the next free one; `ELUVIAN_PORT` chooses another). A direct MCP connection ends with the game.

```sh
claude mcp add --transport http eluvian-direct http://127.0.0.1:8930/mcp
curl http://127.0.0.1:8930/api/health
curl -X POST http://127.0.0.1:8930/api/tool/inspect -d '{"kind":"creatures","side":"hostile"}'
```

It answers on `127.0.0.1` only, has no authentication, and drives the game: a development bench, not something to ship to players.

## Licence

GPLv3 (`LICENSE`), as DAOLib. `src/api/EluvianAPI.h` alone is MIT, so a plugin under any licence can include it.
