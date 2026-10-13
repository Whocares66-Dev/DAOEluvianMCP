# Playbook: driving a test through Eluvian

This guide is for an agent testing Dragon Age: Origins, or a mod in it. Every call below is an MCP tool, reached through the bridge (`tools\eluvian_bridge.py`). The same tools are served over plain HTTP too: `POST http://127.0.0.1:8930/api/tool/<name>` with the arguments as the JSON body.

Eluvian covers the game itself, not any one mod. A mod that registers with Eluvian brings its own tools and events and documents them itself (see [A mod's own tools and events](#a-mods-own-tools-and-events)).

## Rules for an honest test

- **Use ids, not names.** Every creature has an id string such as `"0x7d9"`, the same form the logs use. Two creatures called "Howe Archer" differ only by id. Get ids from `creatures` and pass them to every other tool.
- **Wait for events from the cursor of the call that acted.** A call that sets something off returns a `cursor` (`order` and the staging tools do; `health` and `state` always do). Pass it as `since` to `wait_for`, and the event is found even if it happened before the wait began. A wait from an older cursor returns an old event at once, so always use the cursor from the call that acted. A wait cannot find anything that happened before its cursor: for the current state, read it instead.
- **For states, not events, use the other waits.** `wait_until` calls a tool until its answer meets a condition (a creature dead, health back). `wait_ready` waits until the game takes input again after a load, an area change, a fight, a conversation or a cutscene.
- **`scenario` handles the cursor for you.** Its wait steps start from the cursor taken just before the latest tool step.
- **Ask the game; don't guess.** There is a tool for each question: which effects are on a creature, what it is doing, whether it can see a target, what a mod's state is. Read the log to learn what happened afterwards, not to learn the current state.
- **Leave the game's speed to one plugin.** Each plugin scales the game clock on its own, so if two plugins set a speed, the speeds multiply. Eluvian never sets it; a mod that changes the speed owns it.

## Getting into the game

| Call | What it does |
|---|---|
| `game_saves {"character": "Aedan"}` | Bridge tool. Lists the saves on disk, newest first. Works before the game runs. |
| `game_launch {"owner": "my-name", "load": {"character": "Aedan", "kind": "manual", "number": 2}}` | Bridge tool. Starts the game (never a second copy), loads the save without going through the menus, and returns once the world is ready. |
| `health {}` | Should report state `running` and mode `explore`. Also lists the mods that registered. |
| `state` | The hero, the party and the area. |

Several agents may share one game. If a game is already running, `game_launch` returns `launched: false` and says who started it (the same `owner` that `health` shows; no owner means the player started it by hand). Read that answer before doing anything else, because every other call goes to that game. A game that is not yours is for reading only: do not drive it or close it.

### What runs, and what it left

| Call | What it does |
|---|---|
| `game_status {}` | Bridge tool. Whether a game is running, whose it is (`eluvian.owner`), the other copies running (`copies`), and `why` when a game runs but Eluvian is not serving in it. |
| `game_events {"topic": "eluvian.creature.*"}` | Bridge tool. Reads the last run's events from its file, even after the game closed or crashed. Takes `since`, `topic` and `limit` like `events`. |

Nothing reserves the game between two runs (a claim did until 2026-10-11, #118). A game someone else started says so in `health`'s `owner`, and is theirs. A deploy into the game's folder needs every copy of the game closed, and changes what every later start loads: a test that wants its own build does better with an instance and its own `plugins` folder, below, which puts nothing there.

### A game of your own beside another's

The game runs one at a time, except as a named instance: a second copy with its own saves, settings, logs and port. It needs a loader of DAOLib #67 or later in the game folder.

| Call | What it does |
|---|---|
| `game_instance {"name": "mytest"}` | Bridge tool. From now on every tool of this bridge is that instance's: `game_launch` starts it, `game_status`, `game_saves` and `game_quit` are of it, and the game's tools go to it. An empty `name` goes back to the game as ever; no `name` only says which copy it is and which others run. |
| `game_launch {"owner": "my-name", "seed": "save", "load": {"character": "Lyna", "kind": "manual", "number": 4}}` | Starts the instance. `seed: "save"` gives a new instance only the settings and that one save; without it the loader copies the whole default profile on the first start, every save of every character. |
| `game_launch {..., "plugins": "C:\\path\\to\\build\\release\\daolib\\plugins"}` | Loads the plugins from that folder in place of the game folder's, so nothing is deployed. The folder must hold `eluvian\eluvian.dll`. |

A script does the same with `python eluvian_bridge.py --instance mytest --call game_launch ...`. An instance's saves are its own from then on: what it saves is not in the default profile, and what the default profile saves later is not in it. A deploy into the game's folder still needs every copy closed. The first copy may freeze for a few seconds while a second starts.

If `game_launch` says the game is running but Eluvian is not serving, its `error` quotes the log line that explains why. The usual cause is a loader built for a different plugin API version than Eluvian. The loader and every plugin in the game folder must share one API version, so when DAOLib's API changes they are deployed together.

### Saves

- **Loading with the game running.** `saves {"action": "list"}` lists the saves, and `saves {"action": "load", ...}` loads one, from the main menu (mode `pregame`) or in the world. `wait_for {"topic": "eluvian.game.loaded", "since": <cursor>}` tells you when the world is there.
- **Saving.** Use `saves {"action": "save", "name": "..."}`. It makes a new manual save and overwrites nothing. Writing over a used slot or the player's quicksave is refused unless `overwrite` is true, and `input` refuses a message that saves unless `confirmSave` is true.
- **The player's own playthrough.** A test there only loads, or writes a new save of its own by name. The game's own autosaves are allowed, and the game makes them at more than area changes: some doors and plot steps autosave too (`eluvian.game.saved` reports each one). A test that must leave the saves exactly as they were stops before such a step, or runs on a copy of the saves.
- **Missing content.** A save made with content the install no longer has stops at the game's "requires the following content" message. Only where the user allows it, `game_launch` with `"load": {..., "forceLoad": true}` presses Force Load there, or `saves {"action": "force_load"}` does it with the game already running. A game loaded this way lacks that content and must never be saved: `saves` refuses to save until another save is loaded.

## Looking around and getting things done

### Characters and quests

| Call | What it does |
|---|---|
| `character {"id": "0x82"}` | The character sheet: race, class, level, attributes, combat numbers, and what it wears. |
| `abilities {"id": "0x82"}` | Every talent, spell and skill it knows, not only those on the quickbar. |
| `tactics {"action": "look", "id": "0x82"}` | Its tactics slots: the target, condition, command and ability of each. |
| `equipment {"id": "0x82"}` | Each worn item in full: type, material, stats, properties. |
| `inventory` | The party's backpack. |
| `item {"id": "0x20c5"}` | One item in full. |
| `journal` | The quests in progress and the one marked: plot, name, objective, text. |
| `plot {"plot": "bed000pt_main"}` | A quest's or story's flags: which are set, and the journal text for those. Add `"flag": "NAME"` to read just one. A plot is named by its resource name (the game's plot files, or a mod's). |

### The area

| Call | What it does |
|---|---|
| `surroundings` | A quick look around, one line per thing: creatures, things that can be used, and exits within 30 m. `brief: true` on `creatures`, `placeables` or `exits` gives the same one-line form for one kind. |
| `placeables {"radius": 30}` | Doors, chests, levers: their state, tooltip and lock. |
| `exits` | The ways out of the area: door or trigger, where each leads, and how far it is. |
| `inventory {"id": "0x1fd"}` | What a chest, body or pile of bones holds, without opening it. A body's treasure may only be chosen when it is opened, so an empty list does not prove it is empty. |
| `geometry {"id": "0x82", "target": "0x2ec", "walk": true}` | The walk there: its length, or the door in the way (`blocker`). |
| `gui` | What the interface shows: the open screens (Inventory, Map, Store...), the full-screen one, any message box, and the loot panel with its items. |

### Moving, looting, buying, leveling

| Call | What it does |
|---|---|
| `travel {"target": "0x2ec"}` | Walks there, opening doors on the way. Returns whether it arrived, or why not. |
| `travel {"area": "bed100ar_forest_clearing"}` | Goes to another area by the exit that leads there (see `exits`): walks to it, uses it, and returns once the new area has loaded. |
| `order {"action": "use", "target": "0x1fd"}` | Uses it the way a click would (and by an actual click when the creature you control does it). |
| `loot {"target": "0x1fd"}` | Walks there, opens it and takes everything. Returns what was taken and what is left, or why it did not open (`empty`: emptied remains open nothing). |
| `press {"button": "Take All"}` | Presses a button the way a click does: a message box's button, or the loot panel's Take All or Close. |
| `order {"action": "ability", "ability": 200010, "itemTag": "gen_im_qck_health_101", "target": "0x82"}` | Uses an item's ability (here a lesser health poultice). The item is found in the creature's inventory by its tag. |
| `equip {"item": "0x20c5"}` | Puts the item on the selected party member, in the first slot it fits (`slot` to choose). `{"action": "unequip", "item": ...}` takes it off. |
| `levelup {"id": "0x82", "auto": true}` | Spends its level-up points as the screen's Auto Level Up does. Returns what was raised and gained. |
| `store {"action": "buy", "item": "0x673", "count": 2}` | Buys at a merchant's store screen (talk to the merchant, then pick the reply that shows the wares). `gui`'s `store` lists the stock, the prices and your money. |

### Setting up a test

| Call | What it does |
|---|---|
| `resources {"type": "utc", "match": "ogre"}` | Finds the game's templates by a word: creatures (`utc`) for `stage_spawn`, items (`uti`) for `stage_equip`, plots (`plo`), cutscenes (`cut`). |
| `stage_equip {"id": "0x82", "template": "..."}` | Creates an item from its template and equips it. |
| `stage_reset` | Restores the party to full health, revives the dead, clears cooldowns and removes the creatures that died, so another fight can run in the same session. |

### Seeing it

| Call | What it does |
|---|---|
| `camera {"action": "face", "target": "0x305"}` | Turns the camera to face it at once. Take the screenshot after this call. |
| `camera {"action": "follow", "target": "0x305"}` | Moves the camera to a creature elsewhere and follows it. |
| `camera {"action": "face", "point": [x, y, z], "tilt": true}` | Turns and tilts it, so a point above or below (the sky, a ledge) is in the middle of the picture. |
| `camera {"action": "turn", "pitchDegrees": 120}` | Tilts it: 90 is level, more looks up, less looks down. Close in the game keeps it between 30 and 150. |
| `camera {"action": "zoom", "zoom": -1}` | Zooms as the mouse wheel does: -1 is the tactical view, and 0.5 (farthest) to 1 (closest) the close view; the game has nothing between. It eases there, so read `zoom` again before the screenshot. |
| `camera {"action": "behind", "target": "0x305"}` | Brings it round behind a creature, looking the way it faces. No `target`: behind what it follows. |
| `pointer {"pixel": [800, 500], "seconds": 120}`, then `camera {"action": "follow", "target": "0x82"}` | In the tactical view: keeps the game from panning toward the real mouse at the picture's edge, then centres on a creature. Without the first the party sits in a corner. |
| `pointer {"target": "0x2ec"}` | Holds the game's cursor over it without clicking, so hover previews show. Then read `cursor` and take a screenshot. `{"release": true}` ends it. |
| `screenshot {}` | A picture of the game window as it is drawn. `{"image": false}` only saves the file. |

## Conversations and cutscenes

A conversation or cutscene holds up the world until it is answered. `health`'s mode says which one is on (`dialog` or `cutscene`).

| Call | What it does |
|---|---|
| `dialogue {"action": "look"}` | The last line spoken, the replies offered (place, text, icon), and whether a cutscene is playing. |
| `dialogue {"action": "continue"}` | Like the skip key: cuts the current line short. Also goes through a reply that has no text. |
| `dialogue {"action": "choose", "place": 0}` | Picks the reply at that place. |
| `dialogue {"action": "choose", "place": 0, "until": "choice"}` | Picks the reply, then carries on to the next real choice in one call. Returns the lines spoken (`lines`), then either the next `replies`, or that the conversation `ended` and what came after. |
| `dialogue {"action": "skip_cutscene"}` | Like pressing Escape in a cutscene or a pre-rendered movie. |
| `stage_cutscene {"resource": "bed110cs_tainted_mirror.cut"}` | Plays a cutscene the way the game's scripts do (to test the cutscene, or skipping it). |
| `script {"action": "call", "command": "PlayMovie", "args": ["game_introduction.bik"]}` | Plays a movie. |
| `wait_for {"topic": "eluvian.dialogue.*", "since": <cursor>}` | Returns the next `eluvian.dialogue.line`, `.replies` or `.chosen` event as it comes. |

Step by step:

1. Read the conversation with `look`.
2. If there are no replies, or the first reply has no text, `continue`.
3. Otherwise `choose` a reply.
4. Wait for what follows, from the `cursor` in the answer.

The game ignores a choice made in the moment right after the replies appear, so if nothing follows within a second, choose again (`until: "choice"` does this for you). The test decides which reply to pick: the texts are the game's own, and some replies end the conversation or start a fight.

## Reading and acting in a fight

| Call | What it does |
|---|---|
| `creatures {"side": "hostile", "radius": 30}` | Who is around: ids, health, and what each is doing. |
| `creature {"id": "0x7d9"}` | One creature in full: its effects with time left, its abilities and cooldowns, and its current command. |
| `geometry {"id": "0x82", "target": "0x7d9"}` | Distance, line of sight, and the path it is walking. |
| `order {"action": "attack", "target": "0x7d9"}` | Gives the order. Returns `given` and a `cursor`. |
| `wait_for {"topic": "eluvian.command.ended", "match": {"id": "0x82", "type": "Attack"}, "since": <cursor>}` | Waits until that attack ends, and says how it ended. |
| `stage_effect {"action": "apply", "id": "0x82", "effect": "knockdown", "seconds": 6}` | Puts a condition on a creature, for a case a fight rarely produces. |
| `stage_cooldown {"id": "0x6a8", "ability": 617, "seconds": 0}` | Makes an ability ready (with more seconds, holds it on cooldown). |
| `script {"command": "GetRemainingCooldown", "args": {"oCreature": "0x82", "nAbilityId": 617}}` | Calls any of the game's script commands, for anything no other tool covers. |

When you wait for an order to end, match the command's `type` as well as the creature (`{"id": "0x82", "type": "UseObject"}`). Otherwise an earlier command of the same creature, canceled by the new one, may end first and match.

The same test as one `scenario` call:

```json
{"steps": [
  {"tool": "order", "args": {"action": "attack", "target": "0x7d9"}, "as": "order"},
  {"expect": {"from": "order", "pointer": "/given", "equals": true}},
  {"waitFor": {"topic": "eluvian.command.ended", "match": {"type": "Attack"}}, "timeoutMs": 30000, "as": "end"},
  {"expect": {"from": "end", "pointer": "/payload/success", "equals": true}},
  {"tool": "creature", "args": {"id": "0x7d9"}, "as": "target"},
  {"expect": {"from": "target", "pointer": "/health/current", "atMost": 100}}
]}
```

The game pauses itself about half a second into every real-time fight if the player's auto-pause option is on, and a paused game plays nothing: no deaths, no commands ending. After anything that starts a fight (a hostile creature spawned near the party, a door opened on enemies), call `pause {"paused": false}`. A `wait_for` that times out says `paused` when that is why.

## A screen's own controls

Some parts of a screen, such as tabs, lists and sliders, are run by the screen's own Flash (ActionScript) code, which `input` messages cannot reach. `movie` with `screen` (a shown screen's name, as `gui` lists it) calls a function in that code, sets a variable, or reads a number or true/false value back. For example, to show the Options screen's Controls tab scrolled to the bottom of the key bindings:

| Call | What it does |
|---|---|
| `input {"message": "GUI.ToggleFullScreenGUI Options"}` | Opens the Options screen on its Game tab. Sending it again closes the screen. |
| `movie {"screen": "Options", "call": "SceneManager.PlayScene", "args": ["Controls"]}` | Shows the Controls page. |
| `movie {"screen": "Options", "call": "_root.KeyBindings_mc.SetScrollPercentage", "args": [100]}` | Scrolls the key bindings list to the bottom. |
| `movie {"screen": "Options", "get": "OptionsMenu.ms_nCurrentTab"}` | Reads a number back; the path is a string. Not yet tried in the game. |

`SceneManager.PlayScene` shows the page but does less than clicking the tab button. The button also sets `OptionsMenu.ms_nCurrentTab` (2 for Controls) and updates whatever depends on it. So after `PlayScene` alone, Restore Defaults would restore the Game page's settings, and the tab strip still highlights Game. That is fine for a screenshot; to do more, set the tab number as well (`movie` with `set`). Calling `OptionsMenu.OptionTabAction` does nothing: it returns the button's handler rather than running it.

To just read the key bindings, `controls` lists them without opening any screen.

## A mod's own tools and events

A mod that registers with Eluvian (through `src/api/EluvianAPI.h`) appears in `health`'s `plugins`. Its tools are in the tool list under its prefix, for example `mymod_state` to read its state and `mymod_reset` to set something up. Its events arrive on the same event stream under its prefix (`mymod.*`). `events {"describe": true}` lists every topic, Eluvian's and the mods', with what each means and what it carries; the mod's own docs cover the rest. The turn-based mod's are documented in DAOTurnBased (`turnbased_state`, the `turnbased_*` tools and `turnbased.*` events: see its `dev/TESTING.md`).

## Events from the game

| Topic | When it comes, and what it carries |
|---|---|
| `eluvian.command.started`, `eluvian.command.ended` | Every creature's commands as they start and end: `id`, `type` (Attack, UseAbility, MoveToObject, ...), `playerIssued`, `result` (success, a `COMMAND_FAILED_*` name, or cleared) and `success`. So `wait_for {"topic": "eluvian.command.ended", "match": {"id": "0x82", "type": "Attack"}}` tells you the attack happened and how it ended. |
| `eluvian.mode.changed` | The game mode changed: `from`, `to`. |
| `eluvian.paused` | The game was paused or unpaused: `paused`. |
| `eluvian.game.loaded` | A save loaded and the world is in place: `character`, `kind`, `number`. |
| `eluvian.game.saved` | The game wrote a save, including its own autosaves. |
| `eluvian.creature.died` | A creature died, the moment it did: `id`, `name`, `killer`. Eluvian sees this by checking every frame the creatures that were hurt or staged. |
| `eluvian.script.death` | The game's own death event, 1.5 to 2.5 s after `eluvian.creature.died`, once the death has played out. |
| `eluvian.script.<event>` | One of the game's script events, as the scripts receive it: `receiver`, `creator`, `objects`, `integers`, `floats`. `<event>` is one of damaged, death, attack_impact, combat_initiated, ability_cast_start, ability_cast_impact, apply_effect, remove_effect, gamemode_change, area_loaded, partymember_added, order_received, popup_result. |

For the `eluvian.script.*` events:

- `receiver` is who the event was delivered to, which is not always who it is about: an attack's impact goes to the attacker.
- `objects`, `integers` and `floats` are the event's own parameters, as a script reads them (with GetEventObject and the like): the first four of each.
- Two events also name their parameters. `attack_impact` adds `attacker`, `target`, `hit` (a `COMBAT_RESULT_*` name) and `effectId`. `damaged` adds `damager`, `amount`, `damageType` (a `DAMAGE_TYPE_*` name) and `ability`.

`events {"describe": true}` lists every topic, including those not shown here.

## Clicks, and what still needs eyes

By default `order` sends orders through the engine's own order functions (`via: "command"`). The order counts as the player's, and any creature can obey. Two cases go by click instead unless you say otherwise: a `use` without `objectAction` by the creature you control, and an item's ability used out of combat.

To move the creature you control as the keyboard does, not to a point, use `steer`: `steer {"direction": "forward", "ms": 1500}` holds the forward key for a second and a half with no key pressed, and the engine drives the creature by the way the camera faces (left and right strafe). It ends by itself; `steer {"release": true}` ends it sooner. `eluvian.command.started` from its `cursor` shows the Drive command, and the creature's position after says how far it went. It works in explore and combat mode only.

`via: "click"` sends what a player's click sends in explore mode, so the game's own click handling runs too, along with anything a mod hooks there. Only the creature you control can click, and only in explore or combat mode. A click can:

- use an ability with its target already chosen. With no target, the game (or a mod) asks for one, as the quickbar does.
- do the main action on a target (attack, use). The engine gives this order to every party member selected. The answer's `selected` is the party bar's selection, which may not match the engine's own list, so only `eluvian.command.started` shows who actually obeyed. `state` also shows the selection.

A move cannot be sent as a click.

What is drawn on screen (a mod's interface, floating text, portraits, the camera's view) is checked with `screenshot`, which captures the game's window even when other windows cover it. `pointer` shows what appears when the cursor rests somewhere. The drawn mouse cursor itself, and menus that follow it, track the real mouse, so checking those still needs eyes.
