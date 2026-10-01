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
game_launch {}                                   bridge: starts the game once, waits for the main menu
health {}                                        state running, mode pregame (the main menu); the mods registered
game {"action": "load", "character": "Aedan", "kind": "manual", "number": 2}     asks, and answers a cursor
wait_for {"topic": "eluvian.game.loaded", "since": <cursor>, "timeoutMs": 120000}
inspect {"kind": "game"}                         hero, party, area, mode explore
```

## Reading and acting

```
inspect {"kind": "creatures", "side": "hostile", "radius": 30}     who is around, with ids, health, what each is doing
inspect {"kind": "creature", "id": "0x7d9"}                        one in full: effects with time left, its command, its path
inspect {"kind": "geometry", "id": "0x82", "target": "0x7d9"}      distance, line of sight
order {"action": "attack", "target": "0x7d9"}                      answers given and cursor
wait_for {"topic": "eluvian.command.ended", "match": {"id": "0x82", "type": "Attack"}, "since": <cursor>}
stage {"action": "effect", "id": "0x82", "effect": "knockdown", "seconds": 6}   a case a fight rarely brings
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
