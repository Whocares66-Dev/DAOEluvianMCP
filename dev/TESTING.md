# Testing

Two kinds, as in DAOTurnBased. Without the game, on every build: `src/core` by `el_tests` (HTTP, MCP, REST, the registry, the event bus, the frame queue, arguments, logs), `src/server` by `el_server_tests` over loopback, and the bridge by `tools/test_bridge.py` against a stand-in for the game. In the game: everything under `src/game`, by calling the tools over REST and reading what comes back.

## A check in the game

The game closed: `.\tools\build.ps1 -Preset debug -Test`, then `.\tools\deploy.ps1`. Start the game (or the bridge's `game_launch`), then from a shell:

```sh
B=http://127.0.0.1:8930
curl -s -m 70 -X POST $B/api/tool/wait_for -d '{"topic":"eluvian.game.loaded","since":0,"timeoutMs":60000}'
curl -s $B/api/health
curl -s -X POST $B/api/tool/inspect -d '{"kind":"game"}'
curl -s -X POST $B/api/tool/inspect -d '{"kind":"creatures","radius":40}'
curl -s -X POST $B/api/tool/inspect -d '{"kind":"creature","id":"0x82"}'
curl -s -X POST $B/api/tool/inspect -d '{"kind":"geometry","id":"0x82","target":"0x2df"}'
curl -s -X POST $B/api/tool/script -d '{"command":"GetCreatureProperty","args":{"oCreature":"0x2df","nProperty":7}}'
curl -s "$B/api/events?since=0"
```

`eluvian.log` beside the game's logs has the start-up; `eluvian.json` the port.

## Seen in the game, 2026-09-30

The first run, 18:13 to 18:20: Eluvian built against DAOLib `d25026d` beside turnbased.dll (turn-based-agent's working tree of 17:30) and DAOLib's examples. computer-use-agent loaded Castle Cousland, Aedan's manual save 2, and gave no orders; Eluvian was driven over REST only, and gave none either (turnbased's build of the time called FindPath on every player order).

Worked as built:

- The listener came up at load (`serving MCP at http://127.0.0.1:8930/mcp`) and `health` answered from the first frame. MCP's `initialize` and `tools/list` over `POST /mcp`, as Claude Code sends them.
- Events in order with frames and times: `eluvian.paused`, `eluvian.mode.changed` none to pregame, pregame to loading, the pause the load took, `eluvian.game.loaded {character: Aedan, kind: manual, number: 2}`. `wait_for` from cursor 0 returned the load the moment it came; another with nothing coming returned `found: false` at its timeout.
- `inspect kind=game`: mode, pause, speed, hero `0x82` Aedan, the area (Castle Cousland, `bhn200ar_castle_after_siege`), the party with the dog `0x2df` and Eleanor `0x6a8`, the ids turnbased.log uses.
- `inspect kind=creatures`: eight within 40 m, nearest first, the party's sides right, health (146 of 146 for Aedan), the dead of the siege as dead.
- `inspect kind=creature`: twelve effects on Aedan, each named (`EFFECT_TYPE_MODIFY_PROPERTY`, `EFFECT_TYPE_REWARD_BONUS`) with the item that made it as its creator (The Lion's Paw, Band of Fire); none timed, none from an ability, so `dao::abilities::Name` was not reached.
- `inspect kind=geometry`: line of sight clear to the dog at 1.8 m, cut at the door frame towards Lady Landra 38 m away.
- `script`: `GetGameMode` 8 in the pregame, `GetHero` null there and `0x82` after; `GetCreatureProperty` by parameter name gave the dog's 145 health. `describe` gave a command's parameters and defaults.
- `game` pause and unpause, the pause caught by `wait_for` from a cursor taken before.
- `order`'s refusals (`via: "click"` not built, `target` missing): sentences, no order given.
- `log`: the folder listed, turnbased.log grepped.
- The bridge, over stdio against the running game: `initialize`, the bridge's and the game's tools listed together, `game_status`, a call forwarded; then `game_quit` closed the game.

Found:

- **The main menu runs in pregame, not chargen.** The mode went none, pregame (8), then loading when the save was chosen; DAOLib's GameModes.h says the main menu runs in CharGen. Told to lib-agent.
- **A creature's movement keeps its last path.** Aedan, standing since the load, held a path 2.45 m long, two waypoints, the next the first. So the path alone does not say a creature walks; `inspect` now shows the current command (its type and whether the player gave it) and marks the path `beingWalked` only when that command is a move.
- **The bridge dropped answers when its input ended.** Four requests piped in, one answer out: each request runs on a thread, and the process ended with the input. It now waits up to 30 s for the requests in flight (`test_input_that_ends_still_gets_every_answer`).

Not reached: orders (none were to be given), a fight and its events (`eluvian.script.*`, `eluvian.creature.died`), effects with time left and abilities, turnbased's registrations (its build of the time had none). These are for the next play test, with turnbased's build that registers with Eluvian.

## The third turn-based play test, 2026-09-30

18:29 on: computer-use-agent's fight in the Howe hall with turnbased `5ae088b`, the first build of the mod that registers with Eluvian, beside the eluvian.dll of 18:11. The tester drove part of it through Eluvian's REST calls (health, inspect creatures, events, an attack order, turnbased_settings) instead of screenshots.

- The mod's tools registered on its first frame (turnbased_end_turn, turnbased_seed, turnbased_settings) and its events reached the bus in order beside the engine's: turnbased.fight_began, round_started, turn_started and turn_ended with their reasons (ended by the player, done, out of movement), joined, cooldown_held; eluvian.script.damaged, attack_impact, ability_cast_impact, command_complete.
- `inspect kind=turnbased.state` was refused: the mod's descriptor had unescaped quotes inside its description, and the mod logged success without reading the -1. Eluvian's refusal now says where the JSON breaks; the mod checks its returns and tests its descriptors (turn-based-agent).
- The mod wrote ids as "0x00000082" under "who", where Eluvian writes "0x82" under "id", so a `wait_for` matching an id from `inspect` would never have fired. The mod writes "0x82" under "id" from its next build.

What the tester measured (computer-use-agent, 18:37): a 60-line script played the party's turns through Eluvian alone (inspect creatures on the hostile side near the creature acting, order attack, wait_for turnbased.turn_started or fight_over), and rounds 3 to 7 took 58 s of wall clock, where the first play test took 34 minutes for 11 rounds by computer use. `order attack` reached the mod's gate as the player's ("Dog: attack (action, from player)"); turnbased_settings applied within 120 ms. A wait_for with an old `since` returned an old turn_started at once, so the call after it came 3.9 s into the turn meant; with `since` taken just before, 160 ms. A scenario's wait starts from the cursor before its last tool step, which keeps this right without thought. Game speed 2, set through Eluvian's `game` (since taken out), did not make an 18 m walk faster (3.3 to 3.6 s at both): DAOLib's and the mod's to find. Its wishes: whose turn it is now in a read (turnbased.state), and an event for a refused order with its reason (turnbased.refused); both the mod's.

## The click path and command events in play, 2026-09-30

18:46 to 18:53, computer-use-agent's eight-round fight on the eluvian.dll of 18:41 (turnbased `5ae088b`; log `DAOTurnBased\build\playtest-2026-09-30-6-turnbased.log`).

- `eluvian.command.started` and `ended`: the hero's attack gave started {Attack, playerIssued true}, an attack impact, then ended {success}; a `wait_for` on the end with {id, type} returned 0.4 s before the mod's turn_ended.
- `scenario`: order, wait for command.ended, expect `/payload/success` true, wait for turnbased.turn_ended, in one call: ok, 4 steps, 3.6 s, each step with its time.
- `order via:"click"` with an ability and no target, on Eleanor's turn: the mod logged "asking for its target", as for the quickbar key. Finished by the default via 21 s later; the soldier was knocked down.
- `order via:"click"` attack, in combat on the hero's turn: nothing queued, and the tool said explore mode did not take it. The mod saw the click ("primary action under the cursor") and then "the order queued no command". lib-agent read why: the primary action's handler reads the object set (no re-pick), but orders an attack from every party member selected in the interface, not the controlled creature; and its answer is a void function's leftover register. The tool now ignores that answer and says the events tell who obeyed. Attacks go by the default via.
- An attack's impact is delivered to the attacker (receiver and creator both the hero). Script events now carry their parameters, named for attack_impact (attacker, target, hit, effectId) and damaged (damager, amount, damageType, ability), from rules_core.nss.
- A `wait_for` on turn_started with `since` taken after the turn began waits out its timeout: the turn under way has no event ahead of the cursor. `inspect kind=turnbased.state` answers whose turn it is (the mod's next build).

## stage in play, 2026-09-30

19:13 to 19:28, two fights by computer-use-agent on the eluvian.dll of 19:07 and turnbased `b63e52f` (its report: `DAOTurnBased\dev\PLAYTEST-2026-09-30-4.md`; scripts `DAOTurnBased\build\stage3.py`, `stage6.py`).

- `stage effect knockdown` for 6 s put EFFECT_TYPE_KNOCKDOWN on the dog in its own player-run turn and on a soldier in its AI turn; the mod took both as the game's own ("is held ...: 1 of its turns lost", "turn ends: stunned"). Cases no play test had reached before.
- `stage kill` felled Eleanor in an enemy's turn ("Teyrna Eleanor falls"), and the four enemies one after another, 35 ms apart, to end a fight on purpose.
- The answer to `kill` named the creature only, so a script reading `creature.dead` found nothing. Every `stage` answer now carries the creature as the change leaves it (health, dead, command); after a kill it still lives in that answer, and the death is `eluvian.creature.died`, which these scripts did not wait on: whether it comes after `stage kill` is still to see.
- With turnbased `b63e52f`, `inspect kind=turnbased.state` is registered and answers whose turn it is, so a script started in the middle of a turn acts at once; `turnbased.refused` comes with its detail, command and note.
- Not yet tried: `stage` spawn, health, remove_effects; `order via:"click"` since its answer changed.

The rest of stage, and the click, 19:34 to 19:36 on the eluvian.dll of 19:31 (computer-use-agent):

- `stage health` 5 on a soldier: 5 of 85, and the next hit killed it. `stage effect stun` 10 s, then `remove_effects` stun 2.6 s later: no effects left, and the mod saw the condition go.
- **No `eluvian.creature.died` for any of four deaths**, nor `eluvian.script.death`; the mod's `turnbased.fell` came for each. EVENT_TYPE_DEATH does not pass the engine's event queue where DAOLib hears script events. Deaths are now watched instead: a creature hurt (a damage, an attack's impact) or staged is looked at each frame until it dies, leaves, or goes a minute unhurt, and its death is `eluvian.creature.died` with the last who hurt it.
- **`stage spawn` found no template**: a creature's tag (`bhn200cr_howe_landra1`) names the instance, not its template. Each creature's summary now carries `template` (GetResRef), and spawn tries it with `.utc` too.
- `order via:"click"` attack with the real cursor parked on a door: given, `selected` [Teyrna Eleanor], `eluvian.command.started` {Eleanor, Attack, playerIssued}, and the mod logged the click's own lines first. The case that failed at 18:49 works with the party's selection known.
- A script's mistake re-ordered a refused attack 172 times in 65 s; every call was answered, nothing broke.

Deaths and spawn, 19:42 to 19:43, on the eluvian.dll of 19:40 (computer-use-agent):

- `eluvian.creature.died` comes, watched: a staged kill of a soldier, the scenario's wait matched in 22 ms, one event before the mod's turnbased.fell; and for a party member, Eleanor felled in her own turn, `IsDead` true and health 0 of 176 while she is down. `killer` is null for a staged kill.
- `template` in each creature's summary: the Howe soldiers are `bhn200cr_howe_dual` and `bhn200cr_howe_long`, the archers `bhn200cr_howe_archer`, while their tags are `bhn200cr_howe_landra1` to 4. `stage spawn` of `bhn200cr_howe_long` made a hostile soldier, which joined the fight by itself 316 ms later. Its answer read health 1 of 1, three seconds later 85 of 85: a creature spawned is set up over the next frames, which the answer now says.
- DAOLib `bc081ea` hooks script events where every one runs, queued or immediate (lib-agent: deaths are the engine's own and go straight to the creature), so `eluvian.script.death` may come now; the watch stays until that is seen.

Eluvian 0.2.0, 20:07 to 20:14, two fights with turnbased's newest build (computer-use-agent; its report `DAOTurnBased\dev\PLAYTEST-2026-09-30-5.md`, scripts `build\stage7.py` to `stage9.py`):

- `events {"describe": true}`: the twenty eluvian.* topics with their fields. The mod's topics are not among them yet: its build does not call DescribeEvent.
- Both deaths come now. After four staged kills: `eluvian.creature.died` first (killer null), the mod's turnbased.fell within 0.1 s, `eluvian.script.death` 1.5 to 2.5 s later. After a death from a real blow (Aedan's on a soldier): `died` with killer Aedan, turnbased.fell 43 ms later, `script.death` with creator Aedan 2.1 s after `died`. DAOLib `bc081ea` works; the watch is kept for the moment of the death, the engine's event marks its end.
- `inspect kind=game`'s controlled and selected followed the mod's hand-over of control between turns.
- `script BeginConversation` in a fight answered 1 and started nothing: the engine's answer, not to be trusted there.

Screenshots, 20:29 to 20:46:

- The game's script command `Screenshot` writes nothing in this build: called by name with no arguments and with a file name, in combat and in explore, it answered at once and no file appeared anywhere (computer-use-agent). The screenshot key's own pictures (`Screenshots\User\Screenshot<time>.jpg`) come another way, asked of lib-agent.
- So Eluvian draws the game's window itself, from inside the process: PrintWindow with PW_RENDERFULLCONTENT into a bitmap, encoded as JPEG by WIC. At the main menu it gave the whole 1600 by 1000 menu, Flash interface and all, while the game sat behind other windows; over MCP, a 71 KB JPEG at quality 0.6 as image content, in 149 ms. Not yet taken in a fight.
- The bridge's `game_launch` started the game and answered once Eluvian served at the main menu (pregame, turnbased's registrations in); its `game_quit` closed it. Its `game_saves` matched the load screen entry for entry, without the space the menu shows in "Castle Cousland".

## A session of the user's own, read off the bus, 2026-09-30

22:11 to 23:17: eight turn-based fights played by hand, Eluvian 0.2.0 (the eluvian.dll of 20:52, DAOLib `f253b8c`) and turnbased's build of 20:44 beside it. Read afterwards through `GET /api/events`: 12,743 events, none dropped, 66 minutes.

- Nothing of Eluvian's failed: its log has the start and the mod's four registrations and nothing more; no crash log since the morning's.
- Deaths agree three ways: 39 `eluvian.creature.died`, 39 `turnbased.fell`, 39 `eluvian.script.death`, one of each per creature. The engine's event came a median 2.15 s after the watch's (5.14 s at most; once 20 ms before). Every `died` named its killer: Eleanor 14, Aedan 9, the dog 7, a servant 4, Howe soldiers 3, guards 2.
- Commands: 1,739 started, 1,699 ended; the 40 between are `ExecuteEffect`, which a dying creature starts and never ends. Ends: success 1,286, cleared 315, INVALID_PATH 29, TIMEOUT 5, PATH_ACTION_REQUIRED 3, and 61 `PlayAnimation` ending with result 2, which nothing names (told to lib-agent).
- Attacks: 292 hits, 174 misses, 21 criticals, 10 blocked, 2 backstabs; 3,582 physical damage in 382 events.
- The bus's rate is what a turn-based session costs: `eluvian.paused` 1,475 (the mod pauses and unpauses each turn), `eluvian.script.command_complete` 2,094. At that rate 16,384 events last under an hour and a half. So the bus keeps 32,768 (`ELUVIAN_EVENTS` chooses another), and command_complete, which `eluvian.command.ended` says better, is no longer on it.
- For the mod (told to turn-based-agent): `turnbased.up_again` 311 times against 39 falls, two living creatures getting one every 2.6 s for eleven minutes; 1,097 refusals, up to 64 in a second, most of them followers' moves out of turn; one fight ended "the game ended combat" three seconds after its last enemy fell; AI commands timing out as two fights began.

Loading without the menus, 2026-10-01 (computer-use-agent, on the eluvian.dll of 20:52, DAOLib `f253b8c`):

- `game action=saves` listed the user's ten saves. `game action=load` for Aedan's manual save 2, asked at the main menu (health's mode pregame), answered `asked` at once, and the save was loaded and in explore mode within 14 s, no menu touched. DAOLib's `dao::saves::Load` works from the menu.
- `order action=use` on a door: the hero walked 13 m and opened it.
- Not called: `save` (its default was then a quicksave, which would have written over the user's; it is a new manual save now).
- On that the bridge's `game_launch` takes `load`, and starts a test in its save: asks for the load until the menu takes it, then waits for `eluvian.game.loaded`.

Play test 7 and a load that stopped at a dialog, 2026-10-01:

- computer-use-agent, on the eluvian.dll of 00:09 with the turn-based mod, 27 minutes and two fights: `inspect kind=creature`'s abilities gave a cooldown's remaining and total at each turn, matching the quickbar's sweep; `stage spawn` made 14 soldiers from `bhn200cr_howe_long`, each hostile and fighting within a second; `stage kill` and `stage health` in a fight; `screenshot` with `image: false` in about 50 ms; `order use` on a door in explore mode. In a turn-based fight the door's command ended -1 (COMMAND_FAILED), which was the mod's to fix.
- A wish from it: a spawned creature set fighting another group, so that a fight out of the party's sight can be staged.
- That night the user installed EA's game beside GOG's. Its installer rewrote the shared `Settings\AddIns.xml`, the user's mods were no longer registered, and `game action=load` answered `asked` and then nothing: the game showed "This saved game cannot be loaded. It requires the following content ... Force Load / Cancel", which no plugin is told of (DAOLib `d8ef457` says so in `Saves.h`). `screenshot` showed it. Posted mouse messages did not reach the dialog. Since then the bridge's `game_launch` with `load` says so when the mode has not moved in 20 s, and the bridge knows its own install's game by its file, not its name.
- Not yet seen in the game: `room` and `attackTarget` in `inspect kind=creatures` (DAOLib `4262d70`), `game action=save`, `screenshot source=game`, `stage action=cooldown`.

A test driven with no computer use, 2026-10-01, 01:22 to 01:24 (the unmodded game the user began that night, Lyna, Forest Clearing; eluvian.dll on DAOLib `66d5567`, the turn-based mod's `93f125f`):

- The bridge's `game_launch` with `load` for Lyna's manual save 1: the save loaded 11 s after the process started, asked 2 s after the menu served, no menu touched. The loaded event came while the mode still read loading; the bridge now waits for the mode to leave it.
- `inspect kind=creatures`: `room` and `attackTarget` for all 11 creatures. Outdoors the rooms are the terrain's cells (196612, 262147, ...), each `seen` in view, `wanted` 6 (a value DAOLib does not name), `load` not loaded, `shown` true. A first reading, on DAOLib `4262d70`, had `shown` false for everything there, which DAOLib fixed. `includeInactive` showed five genlocks the story keeps for later.
- `stage action=spawn` with template `wolf` at a point: a hostile wolf, fighting at once; the mod's fight began with it.
- `stage action=cooldown`: 30 s on an ability read back 29.99 of 30 in `inspect kind=creature`, and 0 made it ready. `available` stayed true throughout, being the scripts' own answer; `ready` now says available and not cooling down.
- `screenshot source=game`: the game's own picture, 512 KB in `Screenshots\User`, in 381 ms. `screenshot` of the window: 69 ms.
- `stage action=kill`: `eluvian.creature.died`, and the mod's `fight_over` 0.27 s after.
- `game_quit`: closed. The user's saves were as before: nothing saved, no transition crossed.
- eluvian.log held all 40 calls with their arguments, times and refusals.
- Still not seen: `game action=save`, a fight with creatures in rooms not seen (indoors), dialogue (no tool yet).

The Elven Ruins, 2026-10-01, 01:49 to 02:05 (Lyna's AutoSave_2, loaded only; computer-use-agent's runs and two of mine):

- Indoors `room` gives the rooms' own numbers: the party's 1574 `seen` in view, `wanted` kept, `load` loaded; six rooms ahead (1575 to 1580) `seen` never, with thirty creatures standing in them. The spiders' fight there: the turn-based mod's rule line, four combatants, the room drawn.
- `order action=use` by the command path left a chest and a door shut, the command ending COMMAND_FAILED (computer-use-agent; seen again on chest 0x1fd): it always sent PLACEABLE_ACTION_USE. By click both opened, in explore mode, in a turn-based fight and in a real-time one. Since `47bb9ef` a use by the controlled creature goes by click by default (seen: the answer's `via` click, the command's end success), and the command path takes `objectAction`; `open` by command is not yet seen.
- `inspect kind=placeables`: three within 45 m of the entrance (two ways back to the clearing, a cocoon), each with state, interactive and lock.
- `camera`: read (following Lyna, yaw 357.5, pitch 85, zoom 1); `face` the cocoon turned the view down the passage at once; `turn` to 90; `follow` a spider 40 m ahead cut the view to its room, and `follow` Lyna brought it back. Screenshots after each showed the view asked for.
- `eluvian.dialogue.line` carried Tamlen's remark with its text, twice a millisecond apart (DAOLib notes the same). No conversation with replies was in reach: `dialogue` choose, continue and skip_cutscene are not yet seen.
- A game loaded from inside the game (`game action=load`, then `wait_for eluvian.game.loaded`): explore mode 3 s after asking.
- `wait_for eluvian.command.ended` matched on `id` alone returned an earlier command's end; match on `type` as well.
- Once, 40 s after a process's first load and before any fight, `inspect kind=creature` gave the hero no abilities (computer-use-agent); three every time since. Not reproduced.
- The user's saves were as before after every run.

A fight out of the party's sight, staged, 2026-10-01, 02:04 to 02:15 (the Elven Ruins, loaded only):

- computer-use-agent, with `script`: a spider spawned inside a room never seen and moved to group 2 (SetGroupId) was fought at once by the room's three hostile spiders, the party not in it. GetGroupHostility(2, 1) is 1 and (2, 0) is 0 as the game sets them.
- On that, `group` in every creature's summary and `stage action=group`. Seen: a spider spawned beside a genlock in room 1579 (never seen) read group 1, side hostile; `stage action=group` to 2 answered `was` 1, `hostileToParty` false, `hostileToGroup1` true, and its side read neutral after. No fight followed there: that genlock is one the story has not made active yet. A room whose creatures are active is the one to stage it in.
- `camera action=follow` on the creature in the never-seen room: the screenshot is black but for the interface, and its `room` reads `seen` never. So "the camera is on a black room" can be told by a call and shown by a picture.
- `order action=use` with no `via` on a door in a turn-based fight: by click, success, the door's state 0 to 2 (computer-use-agent). `inspect kind=placeables` by the door listed it and the pressure plates near it.

The message box read, and a game driven by mistake, 2026-10-01, 04:13:

- `inspect kind=popup` after the scripts' `ShowPopup(379897, 1)`: text "Shield Pummel", resolved, and two buttons, Yes sending "GUIPopupLayer.ScriptDialogResult 1 1 -1" and No "... 1 2 -1"; null before it. `inspect kind=game` carried the same. A save asked for with that box up loaded all the same. Nothing presses a button yet.
- That box went up in another agent's game. It had started the game eight seconds before my `game_launch`, which answered `launched` false, `already`; my script read no further and its next calls went to the port, which was that game's. Since then: `game_launch` takes `owner` and `health` gives it, the refusal says whose the running game is and that it is not the caller's to drive, and the tester's guide says to read the launch's answer first.
- Eluvian's `deploy.ps1` and the bridge's `game_launch` work beside another install's running game (EA's, which DAOLib's own work runs).

Whose game it is, and what a chest takes, 2026-10-01, 04:38 to 04:42:

- `game_launch` with `owner`: `health` and `game_status` gave `owner` "eluvian-agent" for the game I started, and null for one a tester started with Start-Process. Another agent read it and waited for that game to end.
- `eluvian_bridge.py --call game_status`, `game_launch` and `game_quit` from a shell.
- `stage action=group` on a tester's run: `was` 1, group 2, `hostileToParty` false, `hostileToGroup1` true, and the fight out of sight followed at once (computer-use-agent).
- `order action=use` by command with `objectAction`, on the chest 0x1fd from 1.6 m: at state 1 (locked) `open`, `open_inventory`, `use` and `examine` each ended COMMAND_FAILED with the state unchanged, and `unlock` ended success, state 1 to 0; at state 0 `open` ended COMMAND_FAILED and `open_inventory` success. The command must carry the action the object takes as it is; a click chooses it.
- A script message box held nothing: loads, orders, a fight and stage calls went on under it for six minutes (computer-use-agent). One `screenshot` of the window came back black in that time, cause not known; `source=game` is to be tried when it does again.
- An attack by click on a creature behind a shut door 12 m off: the creature walked 3 m and stood at UseObject with the attack queued, the turn-based mod on or off; from 3 m it opened the door and went through (computer-use-agent). Whether a real click does the same is not known.
- Still not seen: `dialogue` choose, continue and skip_cutscene.

A conversation answered, 2026-10-01, 04:45 to 04:49 (the Elven Ruins from Lyna's AutoSave_2, the game started with owner "eluvian-agent"):

- The statue's conversation with Tamlen, reached by `order action=use` on the mirror room's door, which walks the hero past its trigger: `eluvian.dialogue.line` "I can't believe this. You recognize this statue, don't you?", `eluvian.dialogue.replies` empty while he spoke and, four seconds on, three replies; `inspect kind=dialogue` gave the same, mode dialog.
- `dialogue action=choose place=0`: answered with the reply chosen; `eluvian.dialogue.chosen` followed, then the next line.
- `dialogue action=continue` on that line: the replies came at once, one with no text, which the game went through (`eluvian.dialogue.chosen` with empty text), and the conversation ended, mode explore a second later. So the loop in the tool's description works as written.
- `dialogue action=skip_cutscene` is not yet seen. Opening the mirror room's door read as mode loading for a second, not cutscene.
- A move across the ruins ended COMMAND_FAILED_PATH_ACTION_REQUIRED where a shut door stood on the way; `order action=use` on the door itself walked there and opened it.
- That door's opening made the game autosave by plot (`eluvian.game.saved` auto 3) though no area transition was crossed: a test that must write no save stops before it. The save it made holds the test's state, and was left for the user to decide on.

The walk there, 2026-10-01, 05:28 (the ruins' entrance, Lyna's AutoSave_2 loaded only, DAOLib `cb08d00`):

- `inspect kind=geometry` with `walk: true`, from Lyna: to Tamlen 0.9 m off, found, 0.91 m, 2 waypoints; to a spider 40.7 m off in the same room, found, 44.18 m, 29 waypoints; to creatures 80 to 143 m off in rooms never seen, behind shut doors, not found, "gave up part way" with `reached` (-3.8, 0.4, 0.0) and 53.4 m so far, the same point each time. No `blocker` was named: the pathfinder's answer there was 1, not 6.
- After five searches a move order of 6 m ended success, 6.0 m walked: the creature is left as it was.
- The run wrote no save (`eluvian.game.saved` did not come), and the saves' folder was as before it.

A message box's button pressed, and the walk from where it stopped, 2026-10-01, 05:38 to 05:46 (the ruins, loaded only; DAOLib `bf85cb1`):

- `game action=press` on the scripts' Yes / No box: "No" by its label answered what it sent and `popup` null; `inspect kind=popup` null a second later. The game's own screenshot with the box up shows the interface dimmed and the hero's bar gone, and after the press both back. A wrong label is refused with the box's labels; with no box up, refused.
- `input` with the Yes button's own text ("GUIPopupLayer.ScriptDialogResult 1 1 -1"): `sent` true and the box gone; "Nobody.NoSuchThing 1": `sent` false.
- `screenshot` of the window was black all over in this run while `source=game` was whole: the display was asleep. The answer now says `black` and to use `source=game`.
- `inspect kind=geometry` with `walk` and `trace`, on DAOLib `52e87a7`: from the entrance the walks to creatures behind shut doors still ended "gave up part way" at (-3.77, 0.39, 0.00), 53.4 m, the search's second leg ending as the first. From that very point (the hero moved there and standing, door 0x1f1 1.2 m off and shut) the same walks answered "something is in the way" in 3 slices, with no `blocker` named. Both sent to lib-agent.
- No run wrote a save.

The walk search on DAOLib `2f549df`, 2026-10-01, 05:55 (the ruins, loaded only):

- Standing 1.2 m from the shut door 0x1f1: `walk` to creatures beyond it answered "something is in the way" with `blocker` 0x1f1, the door, and the note to use it first. That is the answer a test needs before a move that would end COMMAND_FAILED_PATH_ACTION_REQUIRED.
- From the entrance, 53 m short of that door: still "gave up part way" with no `blocker`; the search's probes past the give-up ended without the pathfinder's 6. Sent to lib-agent with the answers.
- A move after the searches ended success; no save was written.

The door in the way named from afar, 2026-10-01, 06:05 (DAOLib `c8d7270`; the ruins' entrance, loaded only):

- `walk` to three creatures behind shut doors, 80 to 143 m off: "gave up part way" at 53.4 m as before, and now `blocker` 0x1f1, the door 1.2 m past where the search stopped, with the note that it is likely what is in the way. One search of 25 slices; the calls took 42 to 48 ms, a found walk of 44 m 25 to 45 ms.
- With the pathfinder's own answer (standing at the door) `blocker` is its word; where the search gave up it is what lies across the way just ahead, a strong hint. The tool's note says which.
- A move after the searches ended success; no save was written.

After the game's tools were split into a file each, 2026-10-01, 06:40 to 06:50 (no behaviour meant to change; the ruins, loaded only):

- Every tool once on the new build: 33 of 34 checks as before the split (the tools listed, each `inspect` kind and a mod's own, `script`, `order` move and use, `stage` spawn, group, effect, health and cooldown, `camera`, both screenshots, `game` pause, saves and press, `eluvian.script.popup_result` on the bus after a press, `input`, `dialogue`'s refusal with none up, `events` describe, `log`, `scenario`), and no save written.
- The one that failed: no `eluvian.creature.died` within 10 s of a `stage action=kill` on a wolf spawned, regrouped, stunned and set to 10 health a moment before. Not seen again: seven kills in two further runs (at once and after 3 s, hostile and group 2, stunned, health set, all three together) each gave the event and a dead creature. The cause, found the next hour: the hostile wolf's appearing 8 m from the hero started a fight and the game was paused for it; the death came 14 s later, the moment `game action=unpause` let the game run. A death does not play out while the game is paused. `stage action=kill` now answers `paused` and says so.
- A creature spawned reads 1 of 1 health for its first frames, and `stage action=health` in those frames leaves it there.

`tools/smoke.py`, 2026-10-01, 07:07: every tool once from the command line (`python tools/smoke.py --character Lyna --kind auto --number 2`), the game started through the bridge under the owner `eluvian-smoke`, loading only, closed after. 33 checks; it is what to run after a change that moves code in `src/game`.

The pause that held the wolf's death, explained (turn-based-agent, 2026-10-01): it was the game's own. With the player's auto-pause option on, the game's scripts pause it about half a second into every real-time fight; a mod running a fight of its own may swallow that, and did not here because it had no fight to run (the wolf was no enemy of the party by then). So after anything that starts a fight, a test unpauses, or waits with `wait_for`, whose timeout now says `paused`.

A key's message without the key, 2026-10-01, 08:58 (DAOLib `0632df5`; the ruins, loaded only):

- `input` with `binding` "UserTogglePauseMessage": `sent` true and the game paused; again, and it ran. "NoSuchMessage 1": `sent` false, nothing happened.
- "SelectPartyMemberIndexMessage 1 1": `sent` true, the selection Lyna and Tamlen, Lyna still controlled; then "... 0 1": the selection Tamlen alone, and Tamlen controlled. So the second field acts as the key held to add to or take from the selection does.
- "SaveGameMessage -1 2" (the quicksave key's) is refused by the tool unless `confirmSave` is given; it was not sent. No save was written, no crash.

`travel`, and the Dalish origin's mirror driven to the camp, 2026-10-01, 09:44 to 09:52 (Lyna's AutoSave_2, the game started with owner "eluvian-agent", no computer use):

- `travel` to a genlock in room 1579 from the ruins' entrance, 80 m and a shut door off: the first call ordered the use of door 0x1f1, named from afar, and stopped with "a fight began" (the spiders' drop); the fight ended with `stage`, the second call opened the door (`opened` 0x1f1) and stopped at the next fight; the third answered `arrived`, 0.8 m.
- `travel` toward the mirror room's door: "a conversation took the screen" at the statue; `inspect kind=dialogue` and `dialogue` choose and continue took it to its end; on, through a fight, to 5.9 m from the door, where the move to the door's own place ended COMMAND_FAILED_INVALID_PATH. The same 20 m from the mirror. Since: `travel` moves to the walk's own end and answers arrived "as near as the walk goes".
- `order action=use` on the door, the Bereskarn's fight, then on the mirror: the mirror's conversation, an area's load in the middle of it, the camp's conversation with Fenarel and the keeper, 38 lines in all, each continued or answered with its first reply by the same loop, to explore mode in the Dalish Camp. The origin's "cutscenes" here are conversations: the game's mode read dialog throughout and never cutscene, so `dialogue action=skip_cutscene` is still not seen.
- The game autosaved at the mirror room's door (AutoSave_4), which the user has said is no concern.
- `screenshot source=game` now answers the picture written again as a plain JPEG (the game's own carries an Exif header a reader rejected).

`travel` to a thing that fills its own place, 2026-10-01, about 09:50 (eluvian.dll of `dd5a424`): to the shut door 0x1f1 from the ruins' entrance, 53.5 m: stopped for the spiders' fight, then arrived, 0.5 m; to the chest 0x1fd: arrived, 1.09 m. `tools/smoke.py` on the same build: 38 of 38.

A sweep of the ruins' fights for the turn-based mod's room rule, 2026-10-01, 09:58 to 09:59 (`build/sweep.py`: Lyna's AutoSave_2 at debug level, the mod's dll of 09:54:55, no computer use):

- `travel` to room 1579 and on to the mirror room, with a survey at each fight it stopped for, the mod's `turnbased_end_turn` to reach an enemy's turn, `camera` for what the view follows, then the fight ended with `stage`. Four fights: two spiders in room 1574; three spiders across rooms 1578 and 1579 after door 0x1f1; two skeletons in 1580; the Bereskarn across 1575 and 1580 after the mirror room's door.
- In each, every creature in the order stood in a room that read shown or had an attack target in the party, no hostile in combat was left out, and on the enemy's turn the camera followed that enemy, its room `seen` in view. One rule line in the mod's log (the rooms the party has seen), no warning, no error.
- The game autosaved at the mirror room's door, into AutoSave_1 this time: its slots go round, so that one wrote over the autosave of 01:12.

For the real mouse, 2026-10-01, about 10:35: `inspect kind=window` gave the client area at (160, 111) on the desktop, 1600 by 1000, not minimised, not the foreground window; `inspect kind=cursor` answered with no object and no hits, the mouse not being over the game. Neither has yet been used with a real click. `tools/smoke.py`: 40 of 40.

`screen`, and a game hung, 2026-10-01, 11:04: the first call of the new `screen` tool (no arguments: the view's size and the window), on DAOLib `d5a1f3e`, never answered. `health` read state stalled, 68 s with no frame, explore mode, no crash log; the game was force-quit, no save written. The handler's first call into DAOLib is `dao::world::ScreenSize()`; the window's own part had answered on the frame thread in `inspect kind=window` minutes before. The tool is built but not registered until the cause is found; `inspect kind=data` from the same hour is fine (`tools/smoke.py` 41 of 41 before `screen` went in).

`screen` on DAOLib `2327f19`, 2026-10-01, about 11:20 (the hang was `ScreenSize` calling the wrong object's slot, which wrote into the camera; fixed there). One call at a time, at the ruins' entrance, the close view:

- The view 1600 by 1000, the window's client area the same, at (160, 111) on the desktop.
- Lyna's feet at pixel (800, 926), in front and in view; a point 5 m behind the camera `inFront` false. The pick 60 pixels above her feet: Lyna, then the ground.
- `aim` for Lyna (800, 683), for Tamlen (546, 632, his feet at 553, 913), and for a wolf spawned 2.5 m behind him from the camera (576, 673): the pick at each aim met that creature first. Bare corners of the picture and a pixel outside it: no hits.
- After nine such calls a 5 m walk ended success, the camera's yaw, pitch and zoom as before, frames running. `tools/smoke.py`: 43 of 43 with two `screen` checks.
- Not yet done: the real mouse moved to an `aim`'s desktop pixel, and `inspect kind=cursor` asked whether the game has the creature under it. That is the tester's.

`screen` with the real mouse, 2026-10-01, 11:20 to 11:35 (computer-use-agent's clicks, then a steadier aim):

- The mouse sent to a spider's `aim` (the `desktop` pixel scaled into the tester's frame): its label came up, `inspect kind=cursor` said the spider, and the right-click was an attack. The same call for a spider standing behind Tamlen answered no aim, which was true.
- A second spider's `aim` sat on the top edge of its body. The mouse sent there: `inspect kind=cursor` had no object and no hits at all, while `screen` with that `pixel` said the spider; the click became a move order. 28 of the tester's pixels lower both agreed and the click attacked. So `inspect kind=cursor` is what to click by, and an aim on an edge does not survive a scaled frame.
- Since then `aim` is a pixel well inside what the pick meets: from DAOLib's first hit and the feet, the places between and a little to each side are tried, and the one with the most of its eight neighbours (10 px away) on the object wins; `clear` says how many. Seen: Lyna (800, 781), Tamlen (179, 676), two wolves, each `clear` 8, the pick at each the creature; 13 to 23 ms a call.

Why the cursor read nothing at the spider's top edge (lib-agent, from the cursor's ray cast): with no place for the cursor the game casts no ray and the hits are empty, ground and all, which is what it does when the cursor is over an interface element or outside the view; a creature's name label, drawn at about the top of its body, is the likely one. So empty `hits` in `inspect kind=cursor` says "over the interface", and an aim at the body's middle keeps clear of the label. DAOLib's own `AimAt` keeps off edges too from `b284b9e`.

A cutscene played and skipped, 2026-10-01, 14:25 (DAOLib `4757f0e`; the ruins' entrance, Lyna's AutoSave_2, loaded only, one call at a time):

- `stage action=cutscene` with `bed110cs_tainted_mirror.cut`: the game's mode was cutscene within half a second (`eluvian.mode.changed` explore to cutscene), `inspect kind=dialogue` said `cutscene` true, and the game's own screenshot three seconds in shows the hand at the mirror between the cutscene's bars. The same name without `.cut`, and `bed200cs_farewell` with or without it, did nothing; the tool now adds `.cut` to a name that has no extension.
- `dialogue action=skip_cutscene`: answered `skipped` cutscene, and the mode was explore again within half a second, `cutscene` false, frames running, the hero where she had stood. With none playing it is refused.
- A pre-rendered movie's skip (`skipped` movie) is built on the same call and not yet seen.
- No save was written. The origin's own playing of that mirror scene had gone by as a conversation's mode in the morning's run; started by the scripts' LoadCutscene it is the cutscene mode.

A pre-rendered movie played and skipped, 2026-10-01, 14:35 (the same place, one call at a time): `script` calling PlayMovie with `bed100cs_duncan_finds_pc.bik` put the game in mode movie within half a second, `inspect kind=dialogue` saying `movie` true and `cutscene` false; `dialogue action=skip_cutscene` two seconds in answered `skipped` movie and the mode was explore again within half a second; the hero walked after. With that every call of DAOLib's `dao::dialogue` has been seen in play.

The walk search with the game paused, 2026-10-01, 15:46 (for lib-agent): `inspect kind=geometry` with `walk`, the hero to a spider 40.7 m off, asked running, paused, paused again and running again: each time found, 44.18 m, 29 waypoints, 20 slices, the same answers slice for slice. A pause changes nothing in it.

A character's sheet, equipment, backpack and items, 2026-10-01, about 17:05 (the user asked for them; the ruins' entrance, loaded only):

- `inspect kind=character`: Lyna an Elf, female, Rogue, level 1, 857 experience, rank Player; strength 12 of base 10, dexterity 22 of 18, cunning 23 of 18; attack 62, defense 62, armor 5.74; what she wears by slot, and the other weapon set's two daggers. Tamlen an Elf Warrior, a follower, state 1.
- `inspect kind=equipment`: the shortbow (base type "Weapon - Shortbow", material "Bow, Horn", damage 5, value 1700), the boots with four properties and their powers ("Increase Armor" 1, "Increase Chance to Dodge Attacks" 2, ...).
- `inspect kind=inventory`: 17 items of 70, each with its stack and base type; `inspect kind=item` on the poultice: a stack of 3 of 99, its property "Flag: Is Healing Potion", and the ability using it casts.
- An item's id given where a creature's is wanted, and the other way, is refused with what to ask instead.
- The labels come from the game's own rule tables (races, classes, base items, materials, item properties, ranks). Not yet named: an item's stats beyond armor, damage and fatigue (given by number), and a creature's properties beyond those script.ldf names. A race's name in the player's language was missing in that run (its table keeps it in another column); fixed after, not yet seen.

The sheet by the game's own names, and all a creature knows, 2026-10-01, 18:41 (DAOLib `28906b4`; one call at a time on a game of this session's own, the folder claimed for it):

- `inspect kind=abilities`: Lyna 4 talents (Dirty Fighting, Dual-Weapon Training, Dual Striking and the class's hidden one), no spells, 6 skills (Poison-Making, Survival, Combat Training, Coercion and two hidden traits), 2 item abilities (the poultice, elfroot): 12, the game's own count. Tamlen 7 talents, 3 skills, 2 items: 12.
- An item's stats by the game's table: the shortbow damage 5, critChanceModifier 6, armorPenetration 2, optimumRange 25, attributeMod 1; the chest piece armor 3, fatigue 2.
- The sheet's `properties` by the properties table's names (Strength ... Electricity_Damage_Bonus, rows 1 to 59). Those names showed script.ldf wrong on two: its MOVEMENT_SPEED_MODIFIER (20) is the table's Flanking_Angle (60 on Lyna) and its MAGIC_CRIT_MODIFIER (22) the table's Missile_Shield; `combat` goes by the table since.
- A race's name in the player's language ("Elf") is there now.
