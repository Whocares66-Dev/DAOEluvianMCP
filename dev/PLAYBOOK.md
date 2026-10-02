# Playbook: driving a test through Eluvian

For an agent testing the game or a mod in it. Every call below is an MCP tool (through the bridge, `tools\eluvian_bridge.py`) or the same over plain HTTP: `POST http://127.0.0.1:8930/api/tool/<name>` with the arguments as the JSON body. Eluvian knows the game, not any one mod: a mod that registers with it brings its own tools, inspect kinds and events, and documents them itself. The first mod to do so, the turn-based mod, played five rounds of a fight this way in 58 s of wall clock, where computer use had taken 34 minutes for eleven (2026-09-30).

## The rules that keep a test honest

- **Ids, not names.** Every creature is an id string, `"0x7d9"`, as the logs write them; two "Howe Archer"s differ only by id. Take them from `inspect`, pass them to everything else.
- **Wait for the event, from the cursor before you acted.** An answer that sets something off carries a `cursor` (`order` and `stage` do; `health` and `inspect kind=game` always do). `wait_for` with `since` set to it returns the event even if it came before the wait began. A wait with an old cursor returns an old event at once: in a play test that made a call land 3.9 s late, 160 ms with the right cursor. And a wait cannot find what happened before its cursor: for the state now, read it.
- **A scenario does the cursor for you.** Its wait steps start from the cursor taken just before the latest tool step.
- **Ask, do not infer.** Which effects hold a creature, what it is doing, whether a target is in sight, what a mod thinks: there is a read for each. Read the log for the story after, not for the state now.
- **The game's speed belongs to one plugin.** Each plugin's copy of DAOLib scales the clock by its own, so two setting it multiply. Eluvian sets none; a mod that changes speed owns it.

## Getting into the game

```
game_saves {"character": "Aedan"}             bridge: the saves on disk, newest first, before the game runs
game_launch {"owner": "my-name", "load": {"character": "Aedan", "kind": "manual", "number": 2}}
                                                 bridge: starts the game once, loads the save without the menus,
                                                 and answers when the world is there
health {}                                        state running, mode explore; the mods registered
inspect {"kind": "game"}                         hero, party, area
```

Several agents may share the one game. `game_launch` answers `launched` false when a game already runs, and says who started it (`health`'s `owner`; none means it was started by hand, the player's own): read that answer before acting, since every other call would go to that game. A game that is not yours is to read, not to drive or close.

With the game already running, `game {"action": "saves"}` lists the saves and `game {"action": "load", ...}` loads one, from the main menu (mode pregame) or in the world; `wait_for {"topic": "eluvian.game.loaded", "since": <cursor>}` says when it is there. A save made by a test is a new manual one by default (`game {"action": "save", "name": "..."}`), which writes over nothing.

## Looking

```
inspect {"kind": "character", "id": "0x82"}      the sheet: race, class, level, attributes, combat numbers, what is worn
inspect {"kind": "equipment", "id": "0x82"}      each slot's item in full: type, material, stats, properties
inspect {"kind": "inventory"}                    the party's backpack; {"kind": "item", "id": ...} is one item in full
inspect {"kind": "placeables", "radius": 30}     doors, chests, levers: state, tooltip, lock
order {"action": "use", "target": "0x1fd"}       opens what a click opens (by click for the controlled creature)
inspect {"kind": "geometry", "id": "0x82", "target": "0x2ec", "walk": true}
                                                 the walk there: its length, or the door in the way (`blocker`)
travel {"target": "0x2ec"}                       walk there, opening the doors on the way; answers arrived, or why not
camera {"action": "face", "target": "0x305"}     turn the camera to it at once; then screenshot
camera {"action": "follow", "target": "0x305"}   look at a creature elsewhere
screenshot {}                                    the window as it is drawn; {"image": false} only writes the file
```

## Conversations and cutscenes

A conversation or a cutscene holds the world until it is answered; `health`'s mode says which (dialog, cutscene).

```
inspect {"kind": "dialogue"}                     the line spoken last, the replies offered (place, text, icon), cutscene
dialogue {"action": "continue"}                  as the skip key: cuts the line short; goes through a reply with no text
dialogue {"action": "choose", "place": 0}        the reply at that place
dialogue {"action": "skip_cutscene"}             as Escape in a cutscene or a pre-rendered movie
stage {"action": "cutscene", "resource": "bed110cs_tainted_mirror.cut"}
                                                 play a cutscene, as the scripts do (for a test of one, or of skipping)
script {"action": "call", "command": "PlayMovie", "args": ["game_introduction.bik"]}
                                                 play a movie
wait_for {"topic": "eluvian.dialogue.*", "since": <cursor>}     .line, .replies, .chosen as they come
```

The loop: read; with no replies, or a first reply with no text, continue; otherwise choose; wait for what follows from the answer's `cursor`. The game ignores an answer in the moment after replies appear, so if nothing follows in a second, ask again. Which reply to choose is the test's to decide: the texts are the game's own, and some replies end the conversation or start a fight.

## Reading and acting

```
inspect {"kind": "creatures", "side": "hostile", "radius": 30}     who is around, with ids, health, what each is doing
inspect {"kind": "creature", "id": "0x7d9"}                        one in full: effects with time left, abilities and cooldowns, its command
inspect {"kind": "geometry", "id": "0x82", "target": "0x7d9"}      distance, line of sight, the path being walked
order {"action": "attack", "target": "0x7d9"}                      answers given and cursor
wait_for {"topic": "eluvian.command.ended", "match": {"id": "0x82", "type": "Attack"}, "since": <cursor>}
stage {"action": "effect", "id": "0x82", "effect": "knockdown", "seconds": 6}   a case a fight rarely brings
stage {"action": "cooldown", "id": "0x6a8", "ability": 617, "seconds": 0}        an ability made ready, or held
script {"command": "GetRemainingCooldown", "args": {"oCreature": "0x82", "nAbilityId": 617}}   anything else
```

Or as one call:

```json
{"steps": [
  {"tool": "order", "args": {"action": "attack", "target": "0x7d9"}, "as": "order"},
  {"expect": {"from": "order", "pointer": "/given", "equals": true}},
  {"waitFor": {"topic": "eluvian.command.ended", "match": {"type": "Attack"}}, "timeoutMs": 30000, "as": "end"},
  {"expect": {"from": "end", "pointer": "/payload/success", "equals": true}},
  {"tool": "inspect", "args": {"kind": "creature", "id": "0x7d9"}, "as": "target"},
  {"expect": {"from": "target", "pointer": "/health/current", "atMost": 100}}
]}
```

## A mod's own

A mod that registers with Eluvian (`src/api/EluvianAPI.h`) shows up in `health`'s `plugins`; its tools are in the tool list (named with its prefix, `mymod_reset`), its reads are `inspect` kinds (`mymod.state`) and its set-ups `stage` actions (`mymod.arena`), each listed in its tool's description, and its events come on the same bus under its prefix (`mymod.*`). `events {"describe": true}` lists every topic there is, Eluvian's and the mods', with what it means and what its payload holds; a mod's own docs say the rest. The turn-based mod's are in DAOTurnBased (`inspect kind=turnbased.state`, `turnbased_*` tools, `turnbased.*` events: its `dev/TESTING.md`).

## The engine's events

`eluvian.command.started` and `eluvian.command.ended` {id, type (Attack, UseAbility, MoveToObject, ...), playerIssued, result (success, a COMMAND_FAILED_*, or cleared), success}: every creature's commands, so `wait_for {"topic": "eluvian.command.ended", "match": {"id": "0x82", "type": "Attack"}}` says the attack happened and how it ended. `eluvian.mode.changed` {from, to}, `eluvian.paused` {paused}, `eluvian.game.loaded` {character, kind, number}, `eluvian.game.saved`, `eluvian.creature.died` {id, name, killer}, the moment it dies (watched: a creature hurt or staged is looked at each frame), and `eluvian.script.death` 1.5 to 2.5 s later, once the death has played out, and `eluvian.script.<event>` {receiver, creator, objects, integers, floats} for damaged, death, attack_impact, combat_initiated, ability_cast_start, ability_cast_impact, apply_effect, remove_effect, gamemode_change, area_loaded, partymember_added, order_received. `receiver` is who the event was delivered to, not always who it is about: an attack's impact goes to the attacker. `objects`, `integers` and `floats` are the event's own parameters, as a script reads them (GetEventObject and the rest), the first four of each. Named for two: `attack_impact` adds `attacker`, `target`, `hit` (COMBAT_RESULT_*) and `effectId`; `damaged` adds `damager`, `amount`, `damageType` (DAMAGE_TYPE_*) and `ability`.

## Clicks, and what still needs eyes

`order` by default gives the order through the engine's own click functions, from the command queue down. `via: "click"` sends explore mode's click message instead, so the click's own handling runs too, and whatever a mod hooks there: an ability with its target selected (none: the game, or a mod, asks for one, as for the quickbar), or the primary action on a target, which the engine gives to the party members selected (the answer's `selected` is the party bar's, which the engine's own list may not match), so whether anyone obeyed shows only in `eluvian.command.started`. `inspect kind=game` shows the selection too. A move cannot be clicked yet. What is drawn (a mod's interface, floating text, portraits, the camera's view) is a `screenshot` away: the game's window drawn by Windows from inside, no window over it in the way. The mouse itself (hover, the cursor) stays with eyes.

When waiting for an order's end, match on the command's `type` as well as the creature (`{"id": "0x82", "type": "UseObject"}`): an earlier command of the same creature may end first, cleared by the new one.

The game saves by itself at more than area transitions: some doors and plot steps write an autosave (`eluvian.game.saved` says so when it happens). A test on a player's own playthrough that must leave the saves as they were stops before such a step, or runs on a copy of the saves.

The game pauses itself about half a second into every real-time fight when the player's auto-pause option is on, and a paused game plays nothing: no death, no command's end. After anything that starts a fight (a hostile creature spawned near the party, a door opened on enemies), `game {"action": "unpause"}`; a `wait_for` that times out says `paused` when that is why.
