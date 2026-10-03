# Eluvian MCP: design

Eluvian is a DAOLib plugin, `eluvian.dll`, that serves the running game on 127.0.0.1: to agents over MCP, and to scripts over plain HTTP. An agent asks the game questions and gives it orders, instead of reading screenshots and logs after the fact. Skyrim's devbench (`C:\modding\devbench`) is the model: a tool registry reflected into both protocols, handlers that run on the game's thread and return a value, an event bus to wait on, and a C ABI through which other plugins add their own tools.

## Why

The first agent play test of the turn-based mode (`DAOTurnBased\dev\PLAYTEST-2026-09-30.md`) drove the game by computer use. One fight took 34 minutes for about 25 orders. Where the time went, from the tester (computer-use-agent, 2026-09-30):

- Finding what to click. The camera re-centres at every turn, so each order took a fresh screenshot, a zoom, a hover for the name label, and then the click: three to five calls per order.
- Waiting blind. Nothing said a turn had ended, so the tester waited a guessed 4 to 12 s and then read the log; too short, and the click landed in someone else's turn.
- Clicks that did nothing. Eight orders on unwalkable spots queued no command and showed nothing; each cost a screenshot and a log read to discover.
- Windows. A CRT dialog from another session's test run sat over the game for ten minutes, and one attack took 15 calls to route round it. Focus returned to Claude between calls.
- Inference. Two "Howe Archer"s were told apart only in the log (`0x7d1`, `0x7d9`); whose turn it was, the movement left, enemy health, a creature's effects, cooldowns and deaths all came from log diffs or zoomed screenshots.

The log is reactive: it says what the mod thought to write, and a question it did not anticipate goes unanswered. The session's worst bug, a pin counted twice, needed "which effects are on the dog, from whom, for how long" at one moment, and no line said it (turn-based-agent, 2026-09-30).

## Principles

From devbench and the agentic-renderdoc model it follows, with what the play test adds.

- **A call returns the answer.** A read answers the question; an order returns what the game did with it, a refusal with its reason included. Handlers run on the frame thread (`dao::OnFrame`, the scripts' thread, which runs while paused too) and the listener waits for their value.
- **Ids everywhere.** Every creature in every answer carries its object id (`"0x7d9"`, as the logs write it) and its name. Every tool that takes a creature takes the id.
- **Wait on events, not on time.** `wait_for` blocks until the event happens and returns it. A guessed sleep is the failure the tester described.
- **Few tools, each named for its job.** One tool with an `action` or `kind` enum rather than a verb per operation, but a new top-level tool when a job does not match an existing tool's name: devbench's A/B test found a cold agent never looks inside a tool whose summary reads as something else.
- **Failure says what to do.** An invalid id says so, and how to list the valid ones.
- **General, not one mod's.** Eluvian serves the game: its creatures, orders, effects, events, logs, and the bench around them (protocols, the bus, scenarios). A mod's own state, actions and events come from the mod, through the C ABI, under its own prefix, documented by the mod; test data (saves, arenas) belongs to the mod tested. The test for a new tool or event: would a test of some other mod, or of the game alone, want it? If only one mod would, it is that mod's to register. The turn-based mod was the first to use Eluvian, and much here was found through it, but nothing in Eluvian's code or its agent-facing text names it.
- **Loopback only.** No auth; a dev bench deployed on purpose. Never bound to anything but 127.0.0.1.

## Architecture

```
  agent ── MCP  POST /mcp ─────┐                          ┌── turnbased.dll, other plugins:
  script ─ REST /api/* ────────┤  listener threads        │   RegisterTool, EmitEvent (C ABI)
                               ▼                          │
                        ToolRegistry ◀────────────────────┘
                               │  a tool's handler, posted to
                               ▼
                         FrameQueue ── drained by dao::OnFrame on the scripts' thread
                                          (engine reads, script commands, orders)

  EventBus: one ring buffer, a sequence number per event; engine events from DAOLib,
            the mods' through EmitEvent; `events` reads from a cursor, `wait_for` blocks
```

| Layer | What | Rule |
|---|---|---|
| `src/core` | MCP's JSON-RPC, the tool registry, the event bus, the frame queue, argument reading, the scenario runner | No `<Windows.h>`, no DAOLib, no `dao::` type. Tested whole. |
| `src/server` | The HTTP listener (cpp-httplib): `/mcp` and `/api/*` over core | Tested over loopback, without the game. |
| `src/game` | The tools themselves, through DAOLib | Verified by playing. |
| `src/plugin` | `DAOLib_Load`, the frame pump, the interface other plugins call | |
| `src/api` | `EluvianAPI.h`, the C ABI for other plugins: MIT, header-only | Plain C types across the boundary. |

**Threads.** A tool is declared frame-thread (the default) or listener-thread. Frame tools are posted to the frame queue with a deadline; a call that is not reached in time (an area loading, the game hung) returns an error naming what `health` says, and is abandoned rather than run late. Listener tools answer without the game: `health`, `events`, `wait_for`, `log`. A plugin's handler is called on the frame thread too, so turnbased need not marshal; it takes its own mutex as its hooks do.

**Plugin load order.** The loader loads plugins sorted by name, so a plugin cannot count on `eluvian.dll` being loaded when its own `DAOLib_Load` runs. It registers from its first frame callback instead, when every plugin is loaded.

## Tools

In build order: what the testers said would have settled the most first (turn-based-agent's ranking: effects, then positions, distance and line of sight, then the event stream, then state).

| Tool | Thread | What |
|---|---|---|
| `health` | listener | Alive or not without waiting on the frame: frames so far, milliseconds since the last, tasks pending, pid, port, the game mode and pause as last seen, versions, the plugins registered. |
| `inspect` | frame | `kind`: `game` (mode, paused, area, in combat, party, selected and controlled creature), `creatures` (every creature in the area or within a radius: id, name, tag, side, position, health, mana or stamina, dead, current command), `creature` (one in full: attributes, effects with type, ability, creator and time left, abilities and cooldowns, weapons and reach, command queue), `geometry` (distance, path length, line of sight between two creatures or points), `character` (the sheet: race, class, level, attributes, combat numbers, every property by the game's own name, what is worn), `abilities` (every talent, spell, skill and item ability it has), `tactics` (its slots, preset and behaviour), `equipment`, `inventory` (a creature's backpack or a container's contents) and `item` (items with type, material, stats and properties by the game's tables), `journal` (the quests under way and done, from the data tree the journal's window reads), `plot` (a quest's or story's flags by name, which are set, the journal's text), `exits` (the area's transitions and where each leads, from the scripts' own local strings), `placeables`, `dialogue`, `gui` (what the interface has up: the screens shown and the full-screen one, through DAOLib's `ShownScreens`; the message box, which was `kind=popup` until #2 folded it in; the loot panel, with what each button sends), `window`, `cursor`, `data` (a row of one of the game's tables), and kinds a plugin adds (`turnbased.state`). The names come from the game's tables (`properties`, `itemstatdefines`, the tactics' tables), not from script.ldf, which is stale on some. |
| `order` | frame | An order as the player gives it, by id: attack, move to a point or creature, use an ability or item on a target, stop; or control, a party member made the creature controlled (the scripts' SetPrimaryControlled, as a portrait's click), so the clicks after it are that member's. `via: "click"` (the default) sends the message explore mode's click handler receives; `via: "command"` queues the command directly, as the handler then does. Returns queued, or refused with the reason (the gate's refusal is an event in the same frame). See "What an order goes through". |
| `events` | listener | The bus from a cursor, filtered by topic. Every event of the run is also kept in `eluvian.events.jsonl` beside the logs (core/EventLog.h), which the bridge's `game_events` reads after the game has gone. |
| `wait_for` | listener | Block until an event matching a topic and fields arrives, or time out; returns it. |
| `game` | frame | Pause and unpause; press a button of the message box or the loot panel (Take All, Close) as a click sends it; later save, load by name and list saves, a load's end being an event. Not the game's speed: each plugin's copy of DAOLib scales the frame's clock by its own, so two plugins setting it multiply (DAOLib `ed55ab8`), and the turn-based mod owns it. |
| `log` | listener | The DAOLib logs (`loader.log`, `eluvian.log`, each plugin's own) from a cursor, filtered. |
| `travel` | listener | Walk a creature to a target or point, opening the doors in the way: asks `inspect kind=geometry` for the walk, uses the `blocker` it names, waits for the command's end, asks again, moves. Decided over other tools' answers alone, so it is in `src/core` and tested with stand-ins. It stops and says why when the journey is no longer its to make: a fight, a conversation, a cutscene, a pause, the time. With `exit` or `area`, the party to another area: the exit from `inspect kind=exits`, walked to, used, and the answer once the scripts' area-loaded event has come or the game's area has changed. |
| `loot` | listener | Take what a container holds: walk to it (`travel`), use it, wait for the loot panel, Take All as the panel's own button does it (`game action=press`), and answer what was taken and what is left. Decided over other tools' answers, in `src/core`, tested with stand-ins. Asked for by the first playthrough driven by MCP alone, stuck in the panel at its first chest (#1). The panel's buttons were read from the inventory movie's script: what each sends, in `src/game/Gui.cpp`. |
| `levelup` | listener | A creature's level-up points spent as the level-up screen spends them, so the game's rules hold: the screen opened for it, its Auto Level Up, ended complete and closed (its own messages, read from its movie and seen in play), answering the attributes raised and the abilities gained. In `src/core` over `input` and `inspect`, tested with stand-ins. Points chosen by name are to come (#5). |
| `store` | frame | Buy or sell at the Store screen that is up, by item id and count, as the screen's own buttons do (`Store.BuyItem` and `Store.SellItem`, read from its movie and seen in play), answering the money before and after; `inspect kind=gui` reads the stock with prices and the pack with sell values (#9). |
| `party` | listener | The party picker as a player works it: the chosen read from the data tree (PartyPicker.SelectedPartyMember); a follower chosen in by holding the cursor across the view until PartyPicker.MouseOverName is theirs, then the scene click's binding SelectPartyMemberUnderCursorMessage there (the picker's scene has its own camera, so the world's pixel is no guide); unchoose by the portrait's message; clear; accept, waiting for the picker to close |
| `equip` | frame | An item of the pack put on or taken off as the paperdoll does (`GUI.SelectedChar.EquipItem <item> <slot>` and `UnequipItem`, read from the inventory movie), on the character the interface has selected, answering the slot before and after (#18). |
| `dialogue` | listener | Answer the conversation on the screen or skip a cutscene, or with `until: "choice"` run on to the next real choice in one call, answering the lines spoken on the way (#17): in `src/core` over the unlisted frame step `dialogue.step`, which does each act. Answering or skipping: `choose` the reply at a place, `continue` as the skip key does, `skip_cutscene` as Escape. `inspect kind=dialogue` reads the line spoken and the replies offered. Without it a test stops at the first conversation between it and the fight it wants (the first test with no computer use, 2026-10-01, could not leave the clearing it loaded into). Through DAOLib's `Dialogue.h`. |
| `stage` | frame | Test set-up: spawn a creature from a template at a point, apply an effect (stun, knockdown, root, grab), set health, mana or stamina, kill, set hostility, move. The fixtures for cases a fight rarely reaches. |
| `script` | frame | Any script command by name, with arguments, returning its value: the general primitive when no tool fits. |
| `camera` | frame | Read it (what it follows, focus, position, yaw, pitch, zoom); `face` a target or point at once, `follow` a creature, `turn` to a yaw. Makes the visual checks cheap: a screenshot that shows what the test means. Through DAOLib's `Camera.h`. |
| `gui` | frame | The Flash UI: call a movie's registered call as the movie would (`TB_EndTurn`, `TB_Focus`), read what the game last sent a movie. Through DAOLib's `Gui.h`. |
| `input` | frame | A key or button at the game's input layer, not the desktop's, so focus and covering windows do not matter: the pause key, the quickbar. With `screen`, a shown screen's own script: `call` a function of its movie, `set` a variable, `get` a number or bool (DAOLib's ScreenMovie, #28): a tab, a scroll, what no message reaches. |
| `screenshot` | frame | The game's own frame as a PNG, as MCP image content: no window can cover it. Waits on DAOLib (below). |
| `scenario` | listener | A list of steps run server-side (a tool call, `wait_for`, a pause, an assertion over a tool's answer), returning a transcript: one call for a whole checklist step. |
| `screen` | frame | For the tests that must go through the real mouse: a pixel to click a creature or object at, where the game's own pick meets it before anything else (not its feet, not behind the party member in front), a world point's pixel, and what lies under a pixel; each place in the game's pixels, as a fraction of the picture, and as a desktop pixel. `inspect kind=window` is the window's rectangle and `kind=cursor` what the game has under the real mouse. Through DAOLib's `ToScreen`, `AimAt` and `PickAt`. |

A mod's own come through the C ABI. The turn-based mod's, as the first example (turn-based-agent, 2026-09-30; its own docs are the reference): `inspect kind="turnbased.state"` (phase, round, current combatant, the order, action and movement left, holds and the turns they cost, held cooldowns, the last refusal, settings, game speed), and tools for end turn, pass, end the fight, toggle, settings in memory, and the initiative seed. A tool's name reaches the model, whose API takes only letters, digits, `_` and `-`, so they are `turnbased_end_turn` and the like; kinds and topics keep their dots.

### What an order goes through

A click reaches a mod watching it twice: explore mode's message handlers first, then the command they queue through `CCObject::QueuePlayerCommand`. What a mod hooks may hang on either; the turn-based mod, the case where this was found, hangs on both (turn-based-agent, 2026-09-30):

| Path | What of the turn-based mod's it drives | Reached by |
|---|---|---|
| The command (`Command::PlayerIssued`, from 0x0073D054) | The gate's verdicts, "Too far", movement, turn rules, cooldowns | `order via:"command"`, and `via:"click"` after it |
| Explore mode's messages (`MoveCharacterToLocation`, `PerformAbility*`, `PerformDefaultActionOnTarget`, `Perform*ActionOnTarget*`, `PerformUseObject`) | The order watch: `order:` lines, "an order but no command while paused", "the order queued no command"; and talent targeting (`pick_targets`), which intercepts the quickbar's `PerformAbilityMessage` | `order via:"click"`, which sends the message through `dao::messages::Send` |
| The pause key; the bar's `TB_EndTurn` | End turn by Space or the button | `input` (a key at the game's input layer) and `gui` (a movie's call, as the movie makes it); `turnbased.end_turn` covers the logic alone |

So `via:"command"` tests the rules and `via:"click"` tests the click. What neither reaches is the mouse itself: the cursor, hover, and which object is under it. That stays with computer use, helped by `screen`.

### Outside the game

A game that is not running cannot serve anything, and a direct MCP connection dies with the process. `tools\eluvian_bridge.py` is a stdio MCP server the agent keeps: it proxies to the game's REST API while the game is up, keeps the last tool list while it is down, and adds what only an outside process can do: `launch` (start `DAOrigins.exe` once, refusing a second instance, since a second one truncated the running game's logs on 2026-09-30), `quit`, and `status`. With it an agent goes from a build to a fight with no screen at all.

## Events

Every event carries a sequence number, the frame, real milliseconds and world milliseconds (so `ai_turn_speed` can be measured), a topic and a payload whose creatures are ids.

- `eluvian.command.started` and `eluvian.command.ended` {id, type, playerIssued, result, success}: every creature's commands as its queue runs them (DAOLib `dao::commands`), what a test waits on after an order.
- `eluvian.*`, from the engine through DAOLib: `area.loaded`, `mode.changed`, `paused`, `combat.began`, `combat.ended`, `creature.died`, `load.finished`, `save.finished`. Script events, messages, the game mode and `OnGameLoaded` cover these (below); a command's start and end is asked of DAOLib.
- `eluvian.dialogue.line` {index, speaker, text}, `eluvian.dialogue.replies` {replies} and `eluvian.dialogue.chosen` {index, text}: a conversation as it goes, ambient lines among them.
- A mod's, under its own prefix, documented by the mod. The turn-based mod's, the first (its names have changed since; its docs have them): `turn.started` (id, round, side, yours, movement), `turn.ended` (reason), `refused` (id, command, reason), `round.started`, `joined`, `fell`, `gone`, `up`, `condition.held`, `condition.ended`, `condition.still_held`, `cooldown.held`, `cooldown.released`, `speed.set`, `fight.began`, `fight.over` (reason).

## Test arenas

A fight a test can rely on, set up the same way each time (turn-based-agent's proposal): a save or area, creatures spawned from templates at fixed points, the party placed, hostility set, initiative seeded. Named scenarios: "two melee and two archers", "a mage", "an ogre's grab", "a knockdown in the creature's own turn". The runner and `stage` live here; the arenas, being the mod's test cases, live in DAOTurnBased as data. The Castle Cousland hall save stays the quick real fight.

## What still needs eyes

The turn bar's look and portraits, floating text actually drawn, selection circles, the talent cursor, camera occlusion. `screenshot` and `camera` make these cheap; they do not replace looking. The floaties can at least be asserted as emitted, since they are effects in the machine's output.

## The C ABI

`src/api/EluvianAPI.h`, MIT so any plugin can include it whatever its own licence. A plugin finds it with `EluvianFind()` (`GetModuleHandleW(L"eluvian.dll")`, then `GetProcAddress("Eluvian_GetInterface")`) on its first frame, since the loader loads plugins sorted by name and has no registry. `EluvianInterface1` is versioned like devbench's `IDevBenchInterface001` and grows only by appending:

- `RegisterTool(name, descriptorJson, handler, context, flags)` and `RegisterInspectKind(kind, ...)`: the handler gets the arguments as JSON and answers once through an `EluvianReply`, `ok(json)` or `error(sentence)`. It is called on the frame thread unless `ELUVIAN_ON_LISTENER`; `ELUVIAN_READ_ONLY` is MCP's read-only hint. The bench's own tool names are refused.
- `EmitEvent(topic, payloadJson)`, from any thread.
- From 0.2.0: `RegisterToolExtension(baseTool, key, ...)`, a choice of one of the tools made of choices: `inspect` (by `kind`) or `stage` (by `action`, a set-up only the mod knows, such as its arena), as devbench's extensions of its base tools; and `DescribeEvent(topic, descriptorJson)`, what a topic means and what its payload holds, listed with Eluvian's own by `events describe=true`. A plugin checks `ELUVIAN_HAS(eluvian, member)` first, since an older Eluvian's interface ends before them.

JSON strings cross the boundary, never C++ objects.

## HTTP

No library: cpp-httplib refuses 32-bit Windows in vcpkg (2026-09-30), and the game is 32-bit. `src/core/Http.h` parses requests (Content-Length and chunked bodies, keep-alive, pipelining) and routes them, tested without a socket; `src/server` is the Winsock loop, a thread per connection that polls so it can stop without closing a socket under another thread. MCP's streamable HTTP is answered request by request in JSON, with no stream to GET; the Origin header is checked, as the transport requires.

## Order of work

1. **The bench.** The repository, core (MCP, registry, bus, queue) with tests, the server, the plugin with `health`, `events`, `wait_for`, `log`, `inspect game`, and the ABI. Seen in the game: Claude Code connects, `health` answers in menus, paused and loading.
2. **Reads**, in the testers' order: a creature's effects, the creatures with positions and health, geometry, then `turnbased.state` and the mod's events through the ABI.
3. **Driving**: `order` through the player's path, `game`, `camera`.
4. **Set-up**: `stage`, `scenario`, the arenas.
5. **Eyes**: `screenshot`, `screen`, the bridge's `launch`.

Where it stands, 2026-10-01: all five built but `screen` and the arenas, and seen in the game (`dev/TESTING.md`): a test launched into a save, driven through a conversation and several fights, and closed, with no computer use. Beyond the list: `dialogue`, `inspect kind=placeables`, `popup` and a creature's room and group, `stage action=group` and `cooldown`, the call log, the bridge from the command line, and `owner` in `health`, each asked for by a tester or shown wanting by a run. Still to come: `gui` and `input` (pressing a message box's button waits on DAOLib), `screen`, a cutscene skipped in play, and a way to keep a test from writing the player's saves (the game autosaves at some doors and plot steps; a test on a player's own playthrough wrote one, 2026-10-01).

## What DAOLib gives, and what it was asked for

Answered by lib-agent, 2026-09-30. `DAOLib\dev\PLUGINS.md` is the API table, `DAOLib\dev\LIBRARY.md` what is verified in play; what is marked "not yet seen in play" there is unproven here too.

| Need | DAOLib today | Asked for |
|---|---|---|
| Finding each other | Nothing: `LoaderInfo` is paths and a version. Plugins load sorted by name. | Nothing yet: `GetProcAddress` on eluvian.dll, resolved on the first frame. A loader registry if a second host needs one. |
| Script commands by name | `dao::script::Invoke<R>(id, args...)`, typed at compile time; `generated/ScriptApi.h` has an id and a wrapper per command | 1st: a run-time table (name, id, parameter types and defaults, return type) from `gen_script_api.py`, and a dynamic `Invoke` over values |
| Orders as the player | `CCObject::QueuePlayerCommand` (0x0073D060), the engine's own path, `QueueNewCommand` with playerIssued set. It reaches `QueueCommand` from 0x0073D054, a caller turnbased's gate knows, and the gate reads `Command::PlayerIssued` alone for `from player`. | 2nd: a wrapper doing what explore mode's click handlers do around it |
| Creatures | `CCObject::Position`, `AreaId`, `Area`; `CCCreature::Property` for vitals; `CurrentCommand`, `CommandAt`, `CommandQueueSize`; the scripts' `GetPartyList`, `GetObjectsInArea`, `GetNearestObjectByHostility`, `IsObjectHostile`, `HasAbility`, `GetRemainingCooldown` | |
| Effects | `CGameEffect`'s type, creator, ability, duration, remaining and parameters; `GetEffects` as an array | 5th: a native walk |
| Events | `dao::OnScriptEvent`; `dao::messages::OnAnyMessage`, `OnMessage<M>`; `CClientExoApp::CurrentMode()`; `dao::saves::OnGameLoaded`. `COMMAND_COMPLETE` reaches script events only sometimes. | 3rd: command start and end |
| Save, load, pause, speed | `SaveGameMessage`, `LoadGameMessage` typed (sending them from a plugin untested); `CClientExoApp::SetPaused`; `dao::clock::SetSpeed` (seen at 2x) | |
| Screenshot | No D3D9 access. Leads: the device's `Present` through the engine's D3DDriver classes, or the engine's own save thumbnail (`SaveLoadProxy::ScreenshotPath`). | 4th |

Rules that come with it: script commands only on the scripts' thread, and not while the mode is `Loading`, when objects come and go; no conversation started or area changed from inside another hook's callback (a frame callback is fine); `OBJECT_INVALID` is 0xFFFFFFFF; every callback wrapped so no exception reaches the game (`dao::detail::Guard`); the listener never reads game memory itself.

For DAOLib's own reverse engineering Eluvian adds a guarded typed read at an address (`dao::TryRead`, `dao::TypeName`), the data tree (`dao::data::Text`, `Number`), and the probe's counters. Nothing writes arbitrary memory.
