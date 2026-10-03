# Eluvian MCP

A test bench inside **Dragon Age: Origins** (GOG Ultimate Edition, `DAOrigins.exe` 1.05.13263.0) for AI agents and scripts: a [DAOLib](https://github.com/Whocares66-Dev/DAOLib) plugin that serves the running game over [MCP](https://modelcontextprotocol.io) and plain HTTP on `127.0.0.1`, in the spirit of Skyrim's devbench.

An agent asks the game what it wants to know (the creatures, their health, their effects, whose turn it is) and gives it orders by id, then waits for the event it expects instead of sleeping and reading logs. Other DAOLib plugins add their own tools and events through a small C interface (`src/api/EluvianAPI.h`, MIT).

`dev/DESIGN.md` has the design: why, the tools, the events, and what comes next. `dev/PLAYBOOK.md` is how an agent drives a test through it.

## Tools

| Tool | What |
|---|---|
| `health` | Is the game alive and in what mode, answered even while it loads or hangs; who started it (`owner`) |
| `inspect` | `kind`: `game` (mode, pause, area, hero, party, a message box if one is up), `creatures` (positions, sides and groups, health, rooms and whether the party has seen them, what each is doing), `creature` (one in full, with its effects and abilities), `character` (the sheet: race, class, level, attributes, combat numbers, what it wears), `abilities` (every talent, spell and skill it knows), `tactics` (its tactics slots, preset and behaviour), `journal` (the quests under way, their objectives and text), `plot` (a quest's or story's flags and which are set), `exits` (the ways out of the area and where each leads), `resources` (the game's templates and other resources by name, from its archives), `equipment`, `inventory` and `item` (items with their type, material, stats and properties), `geometry` (distance, line of sight, the path walked), `placeables` (doors, chests, levers: state and lock), `dialogue` (the line spoken, the replies offered), `gui` (what the interface has up: the screens shown, the full-screen one, the game's message box and what its buttons send, the loot panel and what it holds), `window` (where the game's window is on the desktop), `cursor` (what the game says is under the real mouse), `data` (the data tree the interface is bound to, by path), and kinds plugins add |
| `order` | The player's orders by id: attack, move, ability, use; through the engine's own click functions or explore mode's click |
| `pointer` | Hold the game's cursor over a creature, a point or a pixel without a click, for what shows only under the cursor; the real mouse is not moved |
| `travel` | Walk a creature to a target or point, opening what is shut on the way, or the party to another area by an exit; stops and says why when a fight, a conversation, a message box or a pause takes the journey from it |
| `loot` | Take what a container holds in one call: walk there, open it, take all as the loot panel's button does; what was taken and what is left, or why it did not open |
| `levelup` | Spend a creature's level-up points as the game's level-up screen does, through its Auto Level Up: what was raised and gained |
| `dialogue` | Answer the conversation on the screen: choose a reply, continue, skip a cutscene |
| `camera` | Read it; face a target, follow a creature, turn: so a screenshot shows what the test means |
| `screen` | For the real mouse: a pixel to click a creature or object at (on its body, with nothing in front), a world point's pixel, what lies under a pixel; each as the game's pixels, a fraction of the picture and a desktop pixel |
| `script` | Any of the game's script commands by name, with its answer |
| `stage` | A test's set-up: conditions, health, cooldowns, a death, a spawn, a creature's group (a fight the party is not in), and mods' own actions |
| `screenshot` | The game as a picture, interface and all: its window drawn from inside the game (nothing over it gets in the way), or the game's own, as its screenshot key takes it |
| `game` | Pause, unpause; the saves with their area, name and time played; load one; save; press a button of the game's message box or of the loot panel |
| `events`, `wait_for` | The event bus from a cursor; block until an event happens; `describe` lists the topics and what they hold |
| `scenario` | Steps in one call: tools, waits for events, pauses, checks; a transcript back |
| `log` | DAOLib's logs from a cursor; `eluvian.log` holds every tool call with its arguments, time and refusal |
| `game_saves`, `game_status`, `game_launch`, `game_quit` | The bridge's own, outside the game: the saves on disk, is it running and whose it is (and why Eluvian does not serve, if it does not), start it (into a save, under an owner's name), close it |
| `game_events` | The bridge's own: the last run's events from the file the game keeps (`eluvian.events.jsonl`, every event as it happened), for after the game closed or crashed |
| `game_claim`, `game_release` | The bridge's own: say the game and its folder are in use, by whom, for what and until when; another's launch and Eluvian's deploy are refused meanwhile |

## Building

As DAOLib: Visual Studio 2022 with the C++ workload, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set, Python 3, and for formatting and linting LLVM 22+ and PSScriptAnalyzer.

```powershell
git submodule update --init --recursive         # extern\DAOLib, and its address library
.\tools\build.ps1 -Preset debug -Test           # the plugin, DAOLib's loader, the tests
.\tools\deploy.ps1                              # into the game's bin_ship (game closed)
.\tools\deploy.ps1 -Remove                      # the plugin out again
```

The deploy puts in `eluvian.dll` and the address library's files (`daolib\addresses\*.bin`, one per build of the game, which the loader needs), and DAOLib's loader (`version.dll`) only where the game has none; `-Loader` replaces it. The loader and every plugin under it must be built on one plugin API of DAOLib's: where they are not, Eluvian stays off, `eluvian.log` says which API the loader has, and `game_launch` and `game_status` answer with that line as `why`.

## The claim

Several agents share one game and its folder, and a test may leave a loader and plugins of its own in place between two runs. So whoever needs the game or the folder for more than one launch claims it first, and every agent's deploy and launch scripts honour the claim (the user's decision, 2026-10-02). It is one file beside the logs, `Documents\BioWare\Dragon Age\daolib\game.claim.json`:

```json
{"owner": "computer-use-agent", "note": "play test 23: the attack walk preview", "since": 1790000000.0, "until": 1790001800.0}
```

- A claim stands while `until` (Unix seconds) is later than now. While another's stands: launch no game, deploy nothing, change nothing in the game folder; tell the owner, or wait.
- To claim: write the file whole (a temporary file put in place), at most 240 minutes ahead; to renew, write it again. To release: delete it. Only the owner releases or renews; one past its time is nobody's.
- A file that cannot be read is no claim. The player at the keyboard is not held by it: a game started by hand has no owner in `health`, and is the player's.
- Through Eluvian's bridge: `game_claim {"owner", "note", "minutes"}`, `game_release {"owner"}`, and `game_status` shows the claim; `game_launch` and `tools\deploy.ps1` refuse while another's stands. From a shell: `python tools\eluvian_bridge.py --call game_claim '{"owner": "me", "note": "what for", "minutes": 30}'`.

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
