# Eluvian MCP

A test bench inside **Dragon Age: Origins** (GOG Ultimate Edition, `DAOrigins.exe` 1.05.13263.0) for AI agents and scripts: a [DAOLib](https://github.com/Whocares66-Dev/DAOLib) plugin that serves the running game over [MCP](https://modelcontextprotocol.io) and plain HTTP on `127.0.0.1`, in the spirit of Skyrim's devbench.

An agent asks the game what it wants to know (the creatures, their health, their effects, whose turn it is) and gives it orders by id, then waits for the event it expects instead of sleeping and reading logs. Other DAOLib plugins add their own tools and events through a small C interface (`src/api/EluvianAPI.h`, MIT).

`dev/DESIGN.md` has the design: why, the tools, the events, and what comes next. `dev/PLAYBOOK.md` is how an agent drives a test through it.

## Tools

| Tool | What |
|---|---|
| `health` | Is the game alive and in what mode, answered even while it loads or hangs; who started it (`owner`) |
| `state` | The game itself: mode, pause, area, hero, party, a message box if one is up |
| `creatures`, `creature` | Every creature in the area (positions, sides and groups, health, rooms and whether the party has seen them, what each is doing); one in full, with its effects and abilities |
| `placeables`, `exits`, `triggers`, `surroundings` | Doors, chests, levers (state and lock); the ways out of the area and where each leads; the area's triggers; one look around, a line a thing |
| `geometry` | Distance, line of sight and the path walked between a creature and a target or point |
| `character`, `abilities`, `equipment`, `inventory`, `item` | The sheet (race, class, level, attributes, combat numbers, what it wears); every talent, spell and skill known; items with their type, material, stats and properties |
| `journal`, `plot` | The quests under way, their objectives and text; a quest's or story's flags and which are set |
| `worldmap` | The world map's or the Fade's pins, where the party is, whether a map is up; with `all`, every map's pins from the game's files |
| `gui`, `controls`, `window`, `cursor`, `data` | What the interface has up (screens, the message box and what its buttons send, the loot panel); the key bindings; the game window on the desktop; what is under the real mouse; the data tree the interface is bound to |
| `resources` | The game's templates and other resources by name, from its archives |
| `order` | The player's orders by id: attack, move, ability, use; through the engine's own click functions or explore mode's click |
| `control` | Make a party member the creature the player controls, as a click on its portrait |
| `pointer` | Hold the game's cursor over a creature, a point or a pixel without a click, for what shows only under the cursor; the real mouse is not moved |
| `travel` | Walk a creature to a target or point, opening what is shut on the way, or the party to another area by an exit, or across the world map or the Fade's to a pin (`to`); stops and says why when a fight, a conversation, a message box or a pause takes the journey from it |
| `loot` | Take what a container holds in one call: walk there, open it, take all as the loot panel's button does; what was taken and what is left, or why it did not open |
| `levelup` | Spend a creature's level-up points as the game's level-up screen does, through its Auto Level Up: what was raised and gained |
| `store` | Buy and sell at a merchant's Store screen as its own buttons do; `gui` reads the stock, prices and money |
| `party` | The party picker after a recruit and at camp: who is chosen; choose a follower in the picker's scene (found by the name under the cursor), unchoose one, clear, accept |
| `equip` | Put an item of the pack on, or take one off, as the inventory screen's paperdoll does, so the game's rules hold |
| `gift` | Give a follower a gift from the pack, as the inventory's Give does: the game's own gift event, approval before and after |
| `tactics` | A follower's tactics set as the tactics screen sets them, by name among what the engine offers; its choices; presets; `look` reads its slots |
| `board` | The quest board while it is up: its entries, accept one, close it, as its screen does |
| `dialogue` | Read or answer the conversation on the screen: choose a reply, continue, skip a cutscene; or run on to the next choice in one call, with the lines spoken on the way |
| `camera` | Read it; face a target, follow a creature, turn: so a screenshot shows what the test means |
| `screen` | For the real mouse: a pixel to click a creature or object at (on its body, with nothing in front), a world point's pixel, what lies under a pixel; each as the game's pixels, a fraction of the picture and a desktop pixel |
| `script` | Any of the game's script commands by name, with its answer |
| `stage_spawn`, `stage_group`, `stage_effect`, `stage_health`, `stage_cooldown`, `stage_kill`, `stage_equip`, `stage_reset`, `stage_cutscene` | A test's set-up, a tool each: a creature made, a creature's group (a fight the party is not in), conditions on and off, health, a cooldown, a death, an item made and worn, the party back to full, a cutscene played |
| `screenshot` | The game as a picture, interface and all: its window drawn from inside the game (nothing over it gets in the way), or the game's own, as its screenshot key takes it |
| `saves` | The saves with their area, name and time played; load one; force a load; save a new one |
| `pause`, `press` | Pause or let the game run; press a button of the game's message box or of the loot panel |
| `input`, `movie` | One of the game's own messages, as a button or key sends it; a shown screen's own script: call, set, get, its clips |
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
.\tools\build.ps1 -Preset release -Test         # what the game plays: optimised, its pdb kept (#76)
.\tools\deploy.ps1                              # the release build into the game's bin_ship (game closed)
.\tools\deploy.ps1 -Remove                      # the plugin out again
.\tools\build.ps1 -Preset core; python tools\mutants.py   # src/core broken on purpose: each mutant must be caught
```

The deploy puts in `eluvian.dll` and the address library's files (`daolib\addresses\*.bin`, one per build of the game, which the loader needs), and DAOLib's loader (`version.dll`) only where the game has none; `-Loader` replaces it. The loader and every plugin under it must be built on one plugin API of DAOLib's: where they are not, Eluvian stays off, `eluvian.log` says which API the loader has, and `game_launch` and `game_status` answer with that line as `why`.

`tools\mutants.py` checks the tests, not the code: each of its mutants breaks one promise of `src/core` with a small edit (a comparison flipped, a check dropped), builds the core in a copy of its own under `build\mutants`, and runs the tests, which must fail. A mutant that survives is a promise no test holds. It runs one mutant a core, below normal priority; `-j N` runs fewer, and names choose some (`python tools\mutants.py travel loot`).

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
curl -X POST http://127.0.0.1:8930/api/tool/creatures -d '{"side":"hostile"}'
```

It answers on `127.0.0.1` only, has no authentication, and drives the game: a development bench, not something to ship to players.

## Licence

GPLv3 (`LICENSE`), as DAOLib. `src/api/EluvianAPI.h` alone is MIT, so a plugin under any licence can include it.
