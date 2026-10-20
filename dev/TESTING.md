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

The smoke test on the build with the character reads, 2026-10-01, 19:14, under a claim of this session's own: 55 of 55. The sheet's `combat` by the game's table: attack 62, defense 62, armor 5.74, armorPenetration 1.86, damageMainHand 13.95, fatigue 3.53, flankingAngle 60, missileShield 10; `pointsToSpend` all 0 at level 1.

On DAOLib's address-library line, plugin API 2, 2026-10-01, 19:35 (Eluvian `3654ee9` on DAOLib `8b027c6`), put into the game together with the turn-based mod's build on the same line, under a claim: the mod's deploy put in the loader, its dll and the address files at 19:34:45, Eluvian's its dll straight after, with no game running between.

- `loader.log`: "address library: 103293 IDs for this build, from ...\daolib\addresses\daoaddresses-4eb19d53.bin", "eluvian.dll: loaded", "turnbased.dll: loaded"; DAOLib's two example plugins, still of API 1, "refused to load", each saying so in its own log.
- The smoke test at once: 55 of 55. The mod's registrations came through the C ABI as before (one inspect kind, three tools), and its kind answered. `inspect kind=character` and `kind=abilities` as on the build before (attack 62, 12 abilities of 12).
- No warning or error in `eluvian.log` or the mod's log.
- Learned from these logs: a line's second bracket is a thread's id, not the game's (the loader's lines and a plugin's first share one; the frame thread's differ). The bridge's `why` had taken it for the pid and would have found nothing; it now goes by the logs' time.

A tester's two games on the same build that evening (computer-use-agent, 19:37 to 19:48, the folder under its claim): launch into a save, `health`, `inspect` (cursor, creatures, geometry, a plugin's kind), `screen` (point and target), `screenshot source=game`, `order` (move, ability by the command path), `stage kill`, a plugin's tool and `game_quit` all did what was asked, and a real fight driven with the real mouse went as the morning's had.

`inspect kind=tactics`, 2026-10-01, 20:06 and 20:10 (the scripts' own commands, GetNumTactics and the rest; no new native call): Lyna 3 slots, preset 2 ("Damage Dealer" in the table, "Scrapper" to the player), behaviour DEFAULT, slot 1 enabled: Self, "Being attacked by a melee attack", Use ability, Dirty Fighting; slots 2 and 3 empty. Tamlen 3 slots, no preset: slot 1 Self, Any, Activate mode, Rapid Shot; slot 2 Enemy, "Target using melee attack", Use ability, Pinning Shot. Slots count from 1. The smoke test with its check: 56 of 56. `weaponStyle` "none" with a bow in hand is the game's own answer: its styles are single, dual and two-handed, and none of them is a bow.

`inspect kind=plot`, a container's contents and the follower's record, 2026-10-01, 20:33 (the scripts' own commands again):

- `plot` "bed000pt_main": name "The Lost Mysteries of the Ancients", 30 flags, 5 set (BED_MAIN_CAVES_ENTERED_FIRST_TIME, BED_MAIN_MAIN_JOURNAL, BED_MAIN_EVENT_TUT_EQUIPMENT, and two numbered from 256, which the plot's script computes: the game answered them set with no script asked for). "bed100pt_tamlen": "The Mysterious Cave", four set. "gen00pt_backgrounds": GEN_BACK_ELF_DALISH alone. One flag by name and by number, a plot by its GUID, and the refusals for no such plot or flag. No set flag had journal text (`GetPlotSummary` answered nothing for them).
- `inventory` with a container's id answers what it holds without opening it: the half-buried chest a helmet, the piles of bones a poultice and a dagger, a cocoon six copper. For a container `holds` and `money` are left out.
- `character`'s `follower`: state active, approval 0, locked false, aiEnabled true, for the hero and for Tamlen (the game counts the hero a follower).
- The smoke test: 58 of 58.

`inspect kind=journal` and `kind=exits`, 2026-10-02, 00:40 to 00:48 (from lib-agent's reading: the journal's window is bound to the data tree, and the scripts keep an exit's destination as local strings). The paths read first through `inspect kind=data` on the build in the game, one call at a time, before any code over them:

- The journal: four quests under way, the marked one first (Sulcher's Pass, "Travel to Sulcher's Pass"), then The Lost Mysteries of the Ancients, Humans Near the Camp (within it, and done, though it stays among those under way) and The Mysterious Cave ("Investigate the cave"). Completed and Updated are text, "0" or "1": their number was 0 for both. The entry's objective is marked `<emp>` in the text, and its lines broken with `\r\n`. Journal.CompletedPlots held none.
- The exits: the two "Forest Clearing" placeables lead to bed100ar_forest_clearing, waypoint bed100wp_from_ruins. Every other placeable of the ruins holds "none" there, the template's default, which is no exit. No trigger leads out of the ruins.
- The smoke test: 60 of 60.

2026-10-02, 00:56 to 00:57, a mistake to learn from: the claim file had just been renamed to the shared `game.claim.json`, and a tester's long-running bridge still wrote the old name, so this session's bridge saw no claim, deployed into the GOG folder and launched the GOG game twice while the player's own Steam game was starting (00:56:55). Both GOG games exited at once, writing only their pid-named logs; the Steam game started then was gone by 00:58, and whether the launches had to do with it is not known. Since: the bridge and the deploy read both names, and `game_launch` starts no game while another install's runs. The test save (`game action=save`, refusals for the quicksave and a slot in use) is in the build deployed at 00:56:57 and not yet seen in play.

The test save, 2026-10-02, 01:20 to 01:28, on a game of this session's own after the tester's "closed" (the user's word that tests may make saves): the quicksave and Lyna's manual slot 1 were refused without `overwrite`, as they should be. A manual save to the game's "next free slot" (-1) was declined in explore mode, at once and 10 seconds after the load; one to a free slot named (2) began and was written (`eluvian.game.saved`, 23 s after the ask). So Eluvian finds the free slot itself: a save with no `number` went to slot 3 and was written. The `name` passed did not reach the save: both read with an empty name on the load list (the player's own reads "Forest Clearing"). Two test saves are in Lyna's manual slots 2 and 3, of the ruins. The smoke test: 61 of 61.

`travel` to another area and a named test save, 2026-10-02, 01:52 to 01:53 (DAOLib `c14374c`, after the tester's "closed", under a claim): from Lyna's ruins autosave, `travel {"area": "bed100ar_forest_clearing"}` chose the nearer of the two "Forest Clearing" exits (0x1f5, 4.5 m), walked, used it, and answered "through to Forest Clearing" once the scripts' area-loaded event came: three steps, the game in explore mode, no message box, no save written by the crossing. The clearing's one exit, "Cave Entrance", leads back to the ruins. There `game action=save` with `name` "Eluvian test forest" went to the lowest free manual slot (4), and the load list shows it by that name: lib-agent's fix for the name works. The smoke test: 61 of 61.

`screen`'s `coveredBy`, 2026-10-02, 02:00: at three pixels of a game just started, `screen` named no window over the game and Windows, asked from outside at the same moment (WindowFromPoint), named the game: they agree where nothing covers it. With the game closed, the Claude desktop app's window was over the right of the desktop (x from 984), as the tester had met it; the covered case was not staged, which would mean moving the user's windows. The smoke test: 61 of 61.

For many fights in one launch, 2026-10-02, 02:30 to 02:44 (in turn-based-agent's gap, under a claim):

- `inspect kind=resources`: the first call read 10,652 archives in 15 s, the next answered at once. 26 creature templates hold "ogre".
- `stage spawn` from outside the area, in the ruins: ogre_default (Ogre), hurlock_emissary_off (Hurlock Emissary), bandit_human_archer (Bandit), genlock_alpha_sbow_mix (Genlock Alpha) and spider_giant came up; hurlock_epic_archer, which is an add-in's (Addins\dao_prc_gib), did not. `resources` gives each template's archive: the core's and the campaign's (modules\single player) are loaded in the campaign, an add-in's only where it runs.
- `stage reset`: a wolf killed (in group 2, so no fight) was raised by `ids`, full health; a second, killed in the same fight, was taken away by the party's reset, which takes only the creatures seen to die since the last reset. The first try took every dead creature in the area, twelve of the player's earlier fights' bodies with them (in a test game, saved nowhere); it goes by the record since. Tamlen could not be killed in the ruins (`stage kill` while the spawns' fight had paused the game; after it no death came), so a party member's raising is not yet seen.
- `stage equip`: Spellweaver on Tamlen was made but not put on (its requirements), and said so; a plain longsword (gen_im_wep_mel_lsw_lsw) was put in his main hand.
- `order action=ability` with `itemTag` gen_im_qck_health_101: the poultice found on Lyna and the order given; a tag she does not carry is refused, naming it.
- The smoke test: 61 of 61.

The run's events on disk, 2026-10-02, 03:01: the game kept every event of its run in `eluvian.events.jsonl` beside the logs; after it closed, the bridge's `game_events` read 41 from it, among them the save's load and a staged wolf's death, and with `topic` only those. The next game started the file again. The smoke test: 61 of 61.

2026-10-02, 08:03: the GOG game crashed about 4 s after starting, three times, before any save: EXCEPTION_ACCESS_VIOLATION reading 0x34 in the game's renderer start-up (Graphics::MaterialTechniqueTemp under Renderer::MoviePlayerInternal's constructor), both plugins loaded first. Windows had the monitor off line: the display was asleep, as when screenshots came out black (2026-10-01). Launches through the night with the display on had worked. `game_launch` now answers a game that died starting with its crash log's exception and first frames, and for a crash in the renderer's start-up says to wake the display; seen on the third.

Correction, 2026-10-02, 09:33: the renderer's start-up crash went on with the display on (the user's word; Windows not locked, LogonUI not running). The game's own log (Logs\DragonAge_1.log) has every start from 08:03 stopping after "Initialize - Graphics", the last to pass it at 04:04, and no file of the game's changed between the two (the loader and the plugins changed after the first crash). Windows' report of the monitor as "off line" is no guide: it says so with the monitors on. The cause is not found; the bridge's hint no longer names the display.

After the GOG repair, 2026-10-02, 10:19 to 10:20 (DAOLib `491abad`, under a claim): a launch into Lyna's save worked again; the smoke test 63 of 63.

`pointer`, first sight: held at Tamlen's aim pixel, the game's cursor ray followed it (CursorHits from 0 to 4 there), but CursorObject stayed none, as did CursorGround for a ground point: `overInterface` (dao::gui::MouseOverInterface) was true throughout, the real mouse being on the second monitor, outside the game's window, and the window behind GOG Galaxy's. So the pick follows the pointer only while the real mouse rests over the game's world; asked of lib-agent whether the hover test can follow the pointer too. The release and the hold's own end (1 s) worked. `screen`'s `coveredBy` seen for the first time on a covered pixel: " GOG Galaxy 2.0.73 ".

`pointer` on DAOLib `1a580d2`, 2026-10-02, 10:30: with the real mouse on the other monitor (desktop 2888, 1072) and not moved, the game's pick held Tamlen at his pixel within 0.3 s, and a ground point gave CursorGround. turn-based-agent had seen it work at 10:22 with the real mouse over the world: CursorObject held each spider and the mod's path, plate, cost caption and the cost on the experience bar drew, in the game's own screenshots. A hurlock_emissary_off spawned and read a frame later knew 4 spells (HIDDEN_WIZARD 4023, Drain Life 15002, Weakness 11106, Death Magic 17002); `inspect kind=creature`'s `abilities` is the quickbar's and was empty for it. The smoke test: 63 of 63.

A read straight after `game_launch` answers its save loaded, 2026-10-02, 11:06: two launches, `inspect kind=creature` of the hero 0.03 s after each answer, then every 0.3 s: whole each time (mode explore, the area and the controlled creature known). The one read without a position on the night of 2026-10-01/02 did not come again.

`game action=forceLoad` and `game_launch`'s `load.forceLoad`, 2026-10-02, 13:25 (the user removed the mods from Documents\BioWare\Dragon Age\AddIns for vanilla tests, and allows Force Load, through turn-based-agent): Lyna's AutoSave_2 loaded with no dialog, so nothing was forced; its add-ins are the official ones in the game's folder. The force path is tested with stand-ins only: the bridge presses Force Load only when asked and only at the content dialog, and marks the load `forceLoaded`; the game tool then refuses any save until another load, and a press of a "Force Load" button by `press` marks it too. A save asked for right after this load was begun and not written: the game closed within the 15 s a save takes. The smoke test: 63 of 63.

The loot panel, the screens and the walk overhead, 2026-10-02 23:48 to 00:05 (DAOLib `0dd22bb`, the library as `10e0bcc`; in turn-based-agent's window, under a claim, on its playthrough's saves, none written by the test). #1, #2, #4:

- `inspect kind=gui` (DAOLib's `ShownScreens`): exploring, nine screens shown (Tooltips, PopupLayer, Notifications, NavBar, Quickbar, ArmyControl, Minimap, Portraits, FloatyLayer) and `fullScreen` null; PopupLayer reads shown with no box up. With a loot panel up, mode gui and `fullScreen` Inventory; on the level-up screen, mode chargen and `fullScreen` LevelUp. turn-based-agent saw `fullScreen` Store at a merchant's wares. No call hung.
- `loot` on GH3 05's skeleton remains (0x54b) and the emptied cocoon (0x1ec): the click's use started no command at all, and `loot` answered `empty` after 3 s. A use by command with `objectAction` open_inventory opened the panel on both, holding nothing: empty remains open no panel to a click. `game action=press button=Close` on that panel sent its three messages, all taken, and the game was back in explore, unpaused, the panel and the full-screen GUI gone.
- `travel` to the cocoon 3.4 m overhead: arrived "as near as the walk goes" after three moves, where it had made 22 until its steps ran out.
- `travel` stopped by the bereskarn's fight answered 27.22 m, the same as a geometry read straight after (#3; the stop by a fight not yet seen in turn-based-agent's play).
- The mirror room's door, opened by a walk to the sarcophagus, made the game autosave (Lyna's AutoSave_4) and load, and the walk's next move was refused and thrown up (#7).
- In turn-based-agent's play after the deploy (GH3 12 on): `loot` on a pile of bones walked 21 m, took Studded Leather Armor and closed the panel; the first such call after a load waited out its minute on a move the game cleared (#8). The overhead cocoon as above.
- The level-up screen on GH3 06 (#5), never saved: `GUI.GoToLevelUp 130 0` opened it for Lyna; `GUILevelUp.AutoLevelUp 130` spent her 3 attribute points (Strength), her skill point (Improved Coercion) and her talent point (Dual-Weapon Finesse); `GUILevelUp.CharGenEnd 130 1 1` and `GUI.HideFullScreenGUI` closed it with all of it kept, and closing without the `CharGenEnd` kept it too.
- The smoke test: 63 of 63 on Lyna's manual 4 (the forest). Her AutoSave_2, which the smoke test's usage named, holds the playthrough's ruins since 23:18, where a fight and a party member out of view fail three checks: a test save of its own is the one to name.

The distance after a stop, the level-up and a shop, 2026-10-03 00:40 to 01:15 (DAOLib `10e0bcc`, in turn-based-agent's window, under a claim, its saves loaded, none written). #3, #5, #9:

- `travel` stopped by a fight, in turn-based-agent's play in the Korcari Wilds: nine wolves stopped a walk at 52.35345 m, and a geometry read from the walker straight after gave 52.35345; a second, by a hurlock, at 33.28. Measure from the walker: the turn-based mod selects whoever's turn it is, so the default creature had changed.
- `levelup {"id": "0x82", "auto": true}` on GH3 18: Lyna's two banked levels spent in one round (Strength +6, Dual-Weapon Finesse, Riposte, Improved Coercion), the game back in explore. The followers had no points: they level themselves.
- Ilen's wares on GH3 13, the Store screen up after his first reply: `Store.ID` 6654 (the store object, whose stock `inspect kind=inventory` lists: 26 items), `Store.Name`, `Store.MarkUp` 100, `Store.MarkDown` 25, `Party.Money.Total` 78. `Store.SellItem 130 999 1` sold the Greenstone (value 400) for 100 copper, and `Store.BuyItem 130 1651 1` bought a Lesser Health Poultice (value 100) for 100, the stack in the store from 3 to 2. `GUI.HideFullScreenGUI` closed the Store.
- `travel` to Ilen, a creature, answered "the walk ended COMMAND_FAILED_INVALID_PATH" with 0.45 m left.
- The smoke test: 63 of 63.

The store, the engine's ability check, a long walk and a conversation run on, 2026-10-03 02:45 to 03:25 (DAOLib `7bab46e`, lib-agent's try-5-6-7, with turn-based-agent's loader `7100dae` and its address files; in its window, under a claim, its saves loaded, none written). #7, #8, #9, #11, #14, #17, #19:

- inspect abilities (#11) on GH3 28's Tower Guard, a bow and no shield, at full stamina (115): Shield Bash and Shield Block `ready` false, `why` "No shield equipped"; the shots and Taunt ready, `cost` 23.02 to 46.04 (the table's 20 to 40 times 1.151, his fatigue). With his stamina set to 12 by the scripts' SetCreatureProperty(id, 8, 12.0, 3), every one `ready` false, "Insufficient stamina". IsAbilityAvailable had said yes to all.
- A long walk (#19): from the Tower of Ishal's courtyard to the Quartermaster, 229.5 m straight, the walk found at 276.4 m in one search and walked in one move.
- `store` (#9) at the Quartermaster (GH3 24), after his reply "Let me see what you have.": inspect kind=gui's `store` with MarkUp 115 and MarkDown 25 (a Lesser Injury Kit at 1897, 1650 × 1.15) and 21927 copper; two Elfroot bought at 57 each (21813), the Feral Wolf Charm sold for 4875 (26688) and buyable back.
- `dialogue ... until: "choice"` (#17): first, a continue with his replies already up waited out its budget, and his talk ending into the Store answered a refusal. Both fixed in the window: a continue ran on to his three replies, and choosing the wares answered `ended`, mode gui, with his line.
- The smoke test: 63 of 63, on the build before the dialogue fix.

Brief reads and the paperdoll, 2026-10-03 (in turn-based-agent's window, under a claim, its saves loaded, none written). #16, #18:

- `inspect kind=surroundings` at the forest clearing: the party, the creatures, the usable placeables and the exits within 30 m, a line each. The first build wrote "locked (pick 1)" for an unlocked cave entrance, which has a pick level and no lock; the line now says "pick N" and the state says whether it is shut.
- `equip` on Lyna: the Feral Wolf Charm onto her neck (the Pearl back to the pack) and off again; Blood Dragon Plate refused, `before` equal to `after`, with the note about requirements. In turn-based-agent's play after: Havard's Aegis refused for Alistair's strength, then Band of Fire on his ring and the warpaint on the mabari once each was selected by the binding `SelectPartyMemberIndexMessage <place> 0`.

The world map, control and a walk to a creature, 2026-10-03, the morning (DAOLib `dac5e99`; in turn-based-agent's window, under a claim, its manual 58 loaded, none written; the build `deploy-1003c`). #12, #22, #24:

- `inspect kind=worldmap` with the map down, at Dane's Refuge: 58 pins by index paths, each with its name, tag, area and state; `at` lot100ar_lothering (the map's own place for the party, not the refuge); active Lothering, Party Camp and Flemeth's Hut, Denerim, Redcliffe and the Circle Tower greyed.
- `order {"action": "control", "id": Alistair}`: `taken` true, the selection following him; back to Lyna the same way; the refuge's exit refused as not of the party.
- `travel` to the cook, a creature 12 m off behind the kitchen door: the door used, then the move ended COMMAND_FAILED_INVALID_PATH 0.49 m from him, and the answer `arrived` "there" (#12), where it had answered "the walk ended COMMAND_FAILED_INVALID_PATH".
- Not tried there: travel across the map (`to`) and an autosave waited out (#15), which change the area and so write the game's autosave into the playthrough's slot.

A playthrough driven by MCP alone, Lothering to the Circle Tower and the Fade, 2026-10-03 afternoon (turn-based-agent's, DAOTurnBased#3; builds deploy-1003d to 1003i, each deployed into its gaps). What each build showed, as turn-based-agent reported it:

- The Options screen on its Controls tab, at the bottom of the key bindings, the user's picture taken with no mouse (#28): `input {"message": "GUI.ToggleFullScreenGUI Options"}`, then `input {"screen": "Options", "call": "SceneManager.PlayScene", "args": ["Controls"]}` and `{"call": "_root.KeyBindings_mc.SetScrollPercentage", "args": [100]}`. `OptionsMenu.OptionTabAction` only returns a tab button's handler; the scene's name is "Options", not the movie's "OptionsMenu".
- The world map: `travel {"to": <tag>}` crossed by the map, a random encounter on the way said as such (#24). From the party camp straight back to the area it was entered from, the hero arrived alone four times of four, where the game's own click on the pin (the computer-use agent's) kept all four: the pin's click marks the map closed by travel first, and with that mark set by hand all four arrived (#30).
- `party` at camp and in the Circle Tower's picker with Wynne: choose, unchoose and accept, accept answering the full four once it waited for the party to settle (#25).
- The Windmill door's use, failing from 1 m, stepped back from and used again (#29); the windmill's door up from the Secret Passage, which the search could not reach, used by click from where the party stood (#27).
- Speakers by name (#23): Owain's lines by Owain and kind=dialogue's owner Owain, Wynne's tagged line by Wynne, every line of a blood mage's conversation `to` the listener PLAYER, Lyna.
- The level-up offers came back empty (#5): the chains are filled at once, but a chain's members go by name and DAOLib's list read gave only numbered ones (lib-agent's member names since, not yet seen in play). Auto's attribute spread is the game's own per class (Sten strength 2, dexterity 1).
- Failures met and filed: a "Return Path" text blocker taken for the way to the map, a walk that ended unseen after a fight (#32); a door the story keeps shut used three times (#33); the Fade's own map not taken as a map (#34).
- Two crashes in turn-based fights (06:18 and 15:10, the same fault at 0x004d53b4 in the game's model loading): both plugins' event-script detours on the stack, not implicated as far as lib-agent reads (DAOLib #3, DAOTurnBased #19).

The playthrough on, Redcliffe to Orzammar, 2026-10-04 (turn-based-agent's, DAOTurnBased#3; builds deploy-1003j to 1003z, each deployed into its gaps under a claim). What it showed, each an issue closed on its sighting:

- The world map and the Fade's (#24, #34): `travel {"to": <tag>}` across the map, its pins read; the Fade's own map taken as a map too.
- Doors and blockers (#33, #40, #44, #45, #46, #47, #50): a door the story keeps shut said at once; a door whose key the party lacks, or one that is not interactive, gone round rather than used; a placeable's state named (`stateName`); an unlocked door no longer taken for shut; a far door walked up to; a locked door's key used twice, unlocking then opening.
- Orders (#35, #48, #51): an area ability aimed at its target's position; `order move` for another party member handing it control first and saying so; an item's ability out of combat by click.
- Reads (#39, #49, #52): kind=gui's `fullScreen` only when shown; kind=triggers with the locals asked for; `script` naming the near commands for a wrong one.
- Travel (#37, #53): in combat mode with no hostile near, walking on; where the path search finds none, short straight steps across a doorway's strip.
- A load in a conversation refused, saying how (#36).
- The level-up screen (#5): `levelup {auto}`, then `offers` (talents, spells and skills, each with its state) and abilities by name.

The level-up screen's own buttons, crafting and the rest, 2026-10-05 (turn-based-agent's playthrough, Orzammar and the Deep Roads to Denerim; builds deploy-1004a to 1004n). Each an issue closed on its sighting:

- Control and equipment events (#57, DAOLib #22): `eluvian.party.control_changed` by order, by the party bar's key and by a hand-over; `eluvian.equipment.equipped`/`unequipped`, an item an equip pushed out told as `displaced`.
- `levelup` by name through the screen's own buttons (#54, #58, #63, #64): first by the selection messages with a point count; then by pressing each ability's icon (its `OnButtonAction`, the scene's own point rule) and closing as the screen's Finish does, after a diagnostic read showed the screen reverting presses it had not been told were finished. Lyna's Punisher and Momentum, Wynne's Mass Rejuvenation, Lyna's Combat Tactics (the skills page has a scroller of its own), and the specializations: Ranger, Arcane Warrior, Reaver by the same steps by hand and Alistair's Champion through the tool.
- `eluvian.gui.message` (#58): every message a screen sends the engine, which showed the screen's own `CharGenEnd ... 1 1`.
- `input` with `screen` (#28): `get` any value with its type, `call`'s answer, a clip's `children`; through nine rounds of reads that found the level-up icons; `inspect kind=controls`, the key bindings as text.
- `craft` (#59): poisons, poultices and lyrium potions made through the crafting screen's own message, the listing true to the pack.
- `equip` refusals (#31): `canUse`, and `requirements` as the inventory tooltip says them ("Requires: 38 strength"), read from the engine's own GetItemRequirements.
- An argument a tool does not have is said (#61): `topics` on `events` answered "did you mean `topic`?".

The 0.1.0 release's zip, 2026-10-10 (#115; built on DAOLib `v0.1.0`, plugin API 2). `build\package\DAOEluvianMCP-0.1.0.zip` of the gate's release build, extracted into the GOG game's folder as someone installing it would, under a claim, over the `eluvian` folder that was there:

- With the loader the game had (turn-based-agent's build of DAOLib `1c64ab4`): `loader.log` said "eluvian.dll: loaded" and "turnbased.dll: loaded", `eluvian.log` "eluvian 0.1.0", and the mod's seven tools registered. The smoke test: 74 of 74 on Lyna's manual 4, no crash or hang log.
- With DAOLib 0.1.0's own loader, `version.dll` and `version.pdb` out of the zip its GitHub release carries (the address files in the game were that zip's already, hash for hash): the same lines, and the smoke test 74 of 74 again. The loader that was there was put back after, the same file.
- The bridge out of the zip, started from `bin_ship\daolib\plugins\eluvian\` with no `--exe`: `game_status` while no game ran, `game_launch` to the main menu (`health` said version 0.1.0 and the owner), `game_quit`.
- The link: `eluvian.dll` 2.5 MB where the incremental link's was 5.6, `eluvian.pdb` 40 MB where a PDB linked into twice was 81.
- Not seen: a crash log naming Eluvian's frames from this PDB, since nothing crashed; the EA and Steam games, where Eluvian has never been run.

`camera`'s tilt and zoom, 2026-10-11 (#117; DAOLib `v0.1.0`), on Lyna's manual 4, the Forest Clearing, a game of this session's own. `SceneCamera::SetPitch` and `CameraController::SetZoomSetting` had been called by nothing before:

- `turn` with `pitchDegrees` 100, then 128: held, the frame after too, and the picture at 128 was the sky through the ruin's arches from below the party. The pivot stayed 1.55 m above the focus as the camera went round it (3 m from it), so at 128 the camera stood about level with the feet and at 150 a metre under them: nothing stops it at the ground.
- The close view's clamp: 170 asked read 150 a frame later, as the answer's note had said; a creature beside the hero faced with `tilt` came to 27 and read 30.
- `face` with `tilt` at a point 100 m north and 38 degrees up, from a camera then at 150: yaw -90 and pitch 128.0, so the pivot's height is rightly read back from where the camera stands. A creature's point is its feet: Tamlen at arm's length was looked down on.
- `zoom` to -1: `zoom` there within 0.6 s (six reads 100 ms apart), `tactical` true, pitch 45. Back to 1 the same.
- The tactical view keeps its own tilt: 70 asked there read 45 a frame later, and the answer now says so.
- The party in the picture's corner: each arrival in the tactical view had the focus 9 to 10 m from the hero, forward and left of the view, and the hero at about pixel (1260, 890) of 1600x1000, whatever `follow` did to the offset. The real mouse was off the game's window: the game pans by a cursor at the screen's edge and then only keeps the party in view (DAOLib's `dao/Addresses.h`, ReadInput and KeepInView). With `pointer` holding the cursor at (800, 500), `follow` put the hero at pixel (800, 551) and the picture showed the party at its middle. `follow` now clears the focus's offset, which the tactical view otherwise keeps.
- The smoke test: 76 of 76, with a tilt and a zoom in it.

The same day, after shaders-agent had used it on Lyna's manual 111 at Redcliffe (#117): `face` with `tilt` at a point 100 m along the sun's direction answered pitch 127 and put the sun's disc in the picture, which settled the sun's size; the tactical view by `pointer`, `zoom` -1 and `follow` had the party mid-picture five runs of five. It also met zooms that came out where they were not asked, read here on Lyna's manual 4:

- **The game has two views and nothing between.** From the close view a zoom setting of 0.49, 0.4 or 0.2 read -1 and `tactical` a frame later, and 0.5 held (zoom 0.5, pitch 75, the camera 7.2 m back); from the tactical view 0.5 and 0.7 were the close view at those. So a setting between only flips the view. `zoom` now takes -1 or 0.5 to 1 and refuses the rest, saying so.
- `behind`: with the camera turned to a yaw of 40, `behind` with no target read 180 and the camera west of a hero whose facing is -90, where the save has it; `behind` Tamlen, facing -90 too, the same.
- **A pan of the tactical view does not hold, and there is none.** `SetFocusOffset` with a point 8 m east, then one 40 m north: the offset read back in the same call and as nothing in the next read, 60 ms on, eight tries of eight, the cursor held mid-picture or not, and the hero stayed at pixel (800, 551). What takes it back was not found: DAOLib's own pan writes the offset each frame, and the turn-based mod's copy of it was loaded beside. The action was taken out before the merge.
- The smoke test: 77 of 77.

An argument a tool does not have refused, 2026-10-16 (#147; DAOLib master `5258828`), the release build from the build tree's plugins folder, in instances of this session's own (`eluvian-smoke`, `eluvian-smoke4`), beside the user's `elgara` instance. The smoke test on Lyna2's manual 38 found the one call that had been passing an argument its tool lacks, its own: `log` with `log` for `file`, refused as "`log` is not an argument; it takes `file`, `since`, `grep`, `limit`. Nothing was done". Its six other failures there were the save's: that campaign (Witch Hunt) has neither the dagger template nor the cutscene the smoke test uses, and the hero was out of the camera's view. Fixed, on Lyna's manual 4, the save it is written for: 79 of 79. turn-based-agent read its tools' handlers against their schemas and found them clean; photo cam refuses unknown arguments itself.

A minimised window, 2026-10-16 (#148), the release build from the build tree's plugins folder, in an instance of this session's own (`eluvian-smoke4`, windowed, 1920 by 1200), Lyna's manual 4, the window minimised and restored by `ShowWindow` from a script, no mouse or keyboard. Before, elgara-agent's fullscreen game had minimised itself on losing the display, and `screenshot` and `window` said "the game has no window yet", the search for the window going by its client area, which a minimised window has as 0 by 0. Minimised: `window` answered `minimised` true, the client at (-32000, -32000) and 0 by 0, and the note; `screenshot` refused, saying the window is minimised and to use source "game"; `screen` gave its pixel's hits and no `desktop` place. Source "game" took a whole picture, the game drawing on (498 frames, `running`): a windowed game minimised draws, so the note says only a fullscreen one may not. Restored, `window` read the client at (0, 12) again. A second copy started beside elgara-agent's fullscreen game stopped at the splash twice that morning, with no frame; alone, it loaded each time.

One measure for every angle, 2026-10-15 (#143; DAOLib master `956056d`), on Lyna2's auto 4, the Dragonbone Wastes, loaded only, the game run with the build tree's plugins folder. The readings above are in the measures before it: a yaw from the focus to the camera, and a pitch from straight above.

- **The game's facing:** SetFacing 0 on the hero, and GetOrientation read (0, -1, 0); a facing of -51.4 read (0.78, -0.62); walking east along a path dipping south, GetFacing read -71 on the way; walked south, 0 at the end. So 0 looks along -y, -90 along +x, and it grows clockwise seen from above. Photo cam's fly camera (another session's plugin) has 0 along +y: the two agree only at -90, which is why one reading of each had seemed to match.
- `creature` read `facingDegrees` -51.41; `camera`'s `behind` then read `facingDegrees` -51.41, and the camera stood north-west of her, looking south-east as she does. `turn` to 180 and pitch 20 put the camera 2.8 m south of her and below the pivot: looking north and up. `turn` to her facing at pitch -10: the picture was over her back, the way she looks.
- `pitchDegrees` 80 answered that the close view keeps -60 to 60 and shows 60; 100 was refused.
- With photo cam's build in that folder too, `photocam_toggle` on started at `facingDegrees` -51.41, the same number, and `camera` refused, saying the game is in flycam mode and to use that plugin's tools; off, `camera` read again.

A second copy beside the first, 2026-10-11 (#116; DAOLib master `36ce2d8`, whose loader was in the GOG game's `bin_ship` for this run only, the 0.1.0 loader put back after, the same file). Under a claim, both copies this session's own, on Lyna's manual 4:

- The game as ever, started through the bridge, served on 8930 with the game folder's plugins (the Eluvian of #117 and the turn-based mod).
- `--instance eluvian-test --call game_launch` with `seed: "save"`, `plugins` a build tree's `daolib\plugins` and that save in `load`, beside it: at its main menu and in the Forest Clearing on port 8931, `health` saying `instance` "eluvian-test" and no plugin's tools (the build tree holds Eluvian alone). Its `loader.log`: "the game's mutex is dragonage-eluvian-test and its Documents folder ...\AppData\Local\DAOLib\instances\eluvian-test", "DAOLIB_PLUGINS is set: plugins are loaded from ... and not from the game's folder", "eluvian.dll: loaded", "1 plugin(s)". The save loaded with no word about content.
- The profile the bridge seeded: `Settings`, `addins`, `Offers`, `packages` and `Characters\Lyna\Saves\Slot_4`, 2.1 MB in 31 files, with the loader's `seeded` mark; the loader copied nothing more. `game_saves` of the instance: 1 save, where the default profile has 530.
- In the instance's own `daolib` folder: `eluvian.json`, `eluvian.events.jsonl` and `eluvian.log` under their plain names, on port 8931, and the bridge's `eluvian.launch.json`.
- Each bridge saw its own: the default one pids [the first] and `copies` [eluvian-test's], the instance's the other way round, and `state` answered from each (the Forest Clearing, Lyna and Tamlen, in both). After the instance's `game_quit` the first was still up and listed no copy.
- The smoke test in the instance, the first game up beside it (`smoke.py --instance eluvian-test --plugins ...`): 77 of 77. The second start seeded nothing, the folder being there.
- One bridge switched while it ran, over stdio: `game_instance {"name": "Eluvian-Test"}` answered `instance` "eluvian-test" and its folder, with `tools/list_changed` sent. Two calls sent at once showed an answer put together across the change, which now reads the copy once.
- As the second copy started, the first drew no frame for 5.6 s and left a hang log, its window not in front, as lib-agent had seen three starts of eight.
- Not seen: an instance started by one session beside another session's game, both in use; the loader's own whole-profile seed (lib-agent's play has it); an instance in the EA or Steam game.

A crash in the game's own screenshot, 2026-10-11 11:56 (#120; elgara-agent's game, `crash-2026-10-11-11-56-27.log`): `screenshot {"source": "game"}` sends the screenshot key's message, and the engine, handing its own file name to the render thread, copied 0x400 bytes from a buffer that ended 0x2A0 short of an unmapped page (the fault a `rep movsd` at 0x008E24FA reading 0x4FCA6000, called from 0x00A23D44). Nothing Eluvian sends is in that string. One crash in the `game` screenshots of ten days; the tool's description said so until the fix, which is DAOLib's.

The fix, the same day (DAOLib #78, master `fd43f73`; the loader installed in the GOG game, built of the same code): in an instance of this session's own, its plugins from a build tree of Eluvian on that master, 25 `screenshot {"source": "game"}` one after another, each answered with a file of more than 10 KB, and no crash log. The loader's log: "fixes: a saved file's name reaches the renderer in a buffer of the size the renderer copies", and at the first shot "a file's name of 126 characters was handed to the renderer in a buffer of 1024 bytes". The crash itself was not made and then prevented: it hung on where a name lay in memory. The instance's start froze the other session's game once, as before (its hang log of 12:59:49).

Two sessions' games side by side, read and not driven, 2026-10-11 about 05:00 (#119): shaders-agent's game as ever, started by hand, on 8930, and turn-based-agent's instance `lib`, started by hand with its own plugins, on 8940. A bridge from before #116 had taken the instance for the main game and refused shaders-agent's `game_launch` ("a game is already running"). The bridge of #119, asked from a shell: for the game as ever `pids` [the main game's], `eluvian.port` 8930 and `copies` [`lib` with its pid]; with `--instance lib`, `pids` [the instance's] and port 8940; with `--instance eluvian-test`, not running. The two cases #119 is for were not there to see, and are the tests': an instance with no Eluvian in it, and an instance on 8930 with no main game up.

A game on a second port, its events read and the game not driven, 2026-10-12 04:07 (#121): kadan-agent's two instances, started by its own script through the bridge with a staged copy of the game folder's Eluvian, which was older than #116 and so wrote a second port's names in an instance. The guest, on 8931, kept `eluvian.<pid>.events.jsonl` beside an `eluvian.events.jsonl` of 01:49. Asked from a shell with `--instance` and the guest's name, `--call game_events`: main's bridge answered the 592 events of the run of 01:49 with `running: true`, the ones kadan-agent had reported; this branch's answered 970 of the running game, `file` its own pid's, the pid one of the two games up. Not seen: the last run chosen by its time with no game up, and the game as ever beside an instance holding 8930; both are the tests'.

The folder's owner has the plain names, 2026-10-12 09:18 to 09:25 (#122; DAOLib master `a50ae77`, the game folder's loader of `d3ed625`), every copy at its main menu and none loading a save:

- Confounded first, 09:18: the game as ever with this branch's plugins, beside kadan-agent's two instances on 8930 and 8931, took 8932 and wrote `eluvian.<pid>.json`, `eluvian.<pid>.events.jsonl`, `eluvian.<pid>.log` and the loader's own `loader.<pid>.log` in Documents. The instances ran an Eluvian from before #116, whose DAOLib kept one owner for every copy of the game: the host held it, and every DLL of the game beside it was told it was a second game. `game_events` for it read its own pid's file, 2 events and `running: true`: #121's case of a game that is not an instance, not seen until then.
- Clean, 09:23, no other game up: `eluvian-test` started first with this branch's plugins took 8930; the game as ever beside it, the same plugins, took 8931. In Documents `eluvian.json` (its pid, port 8931, `instance` empty), `eluvian.events.jsonl`, `eluvian.log` and `loader.log` under their plain names, and nothing new with a pid; in the instance's folder the same four, port 8930. `game_events` of each read its own `eluvian.events.jsonl`; `game_status` of the game as ever said its pid, port 8931 and `copies` [`eluvian-test`]. The second copy's loader log: "processors: moved from 0x3 to 0xc" (DAOLib #89).
- Deployed then into the GOG game (every copy closed, kadan-agent's held for it): the game as ever with the folder's own three plugins (this Eluvian, the turn-based mod, the shaders mod) reached its menu on 8930, the plain names, 7 tools of a plugin registered.
- Not seen here: a second game started in a folder where a first runs, which must take its pid's names. lib-agent saw `dao::OwnsLogs` answer false there the same day (DAOLib #91).

A mode ordered with no target, 2026-10-13 (#124), in an instance of this session's own with this branch's plugins, Lyna's manual 4 in the Forest Clearing, explore mode, two daggers staged into her hands so Dual Striking (11, `targetType` 1) was `ready`: `order {"action": "ability", "ability": 11}` four times running, each answered `via` command, `given` true and `target` Lyna herself, and `IsModalAbilityActive` after each read 1, 0, 1, 0. The commands ended as a mode's do: turned on, the UseAbility ends COMMAND_FAILED_COMMAND_CLEARED with the mode on; turned off, a PlayAnimation ends in success. Before, by kadan-agent in two instances the same day: some twenty such orders over five modes (Rock Armor, Momentum, Dual Striking, Threaten, Berserk) turned none on and each one that was on off, and with `target` the creature itself all went on. Not seen: a self ability that is not a mode, and an item's, which is left as it was.

The loader under its new name, 2026-10-13 (#125; DAOLib master `3dad3b8`): the build makes `dinput8.dll` and its pdb beside the plugins. `tools\deploy.ps1` run with `-Game` on five made-up game folders: one with no loader got `dinput8.dll`; one with only `version.dll` kept it and got none; the same with `-Loader` got `dinput8.dll` and lost `version.dll` and `version.pdb`; one with both kept `dinput8.dll` and lost the old pair; one with `dinput8.dll` alone kept it. Each got the plugin's folder and the address files. In the game: an instance of this session's own, its plugins from this build, reached its menu under the GOG folder's loader, still `version.dll` that day ("eluvian.dll: loaded", "1 plugin(s)"). The game with `dinput8.dll` as its loader, the same day at 12:15: lib-agent put it into the GOG folder with every copy closed (DAOLib `c86dc74`, `version.dll` gone), this build's Eluvian was deployed after it (the script kept the loader and brought no `version.dll` back), and an instance started with the folder's own plugins reached its menu on 8930: "elgarashaders.dll: loaded", "eluvian.dll: loaded", "turnbased.dll: loaded", "3 plugin(s)", 7 tools of a plugin registered, the plain file names.

Attributes raised in a game with no paperdoll, 2026-10-13 (#126), in an instance of this session's own, Lyna's manual 4 in the Forest Clearing, given three attribute points by `SetCreatureProperty` (property 34): `input` with "Paperdoll.CharGenAssignAttribute 130 2 0" answered `sent` false, and `data` found no `Paperdoll.PaperCreature`. With the GOG folder's Eluvian of that day, without the fix, `levelup {"attributes": {"dexterity": 3}, "showMs": 0}` answered `levelled` false, "the game took no \"Paperdoll.CharGenAssignAttribute 130 2 3\"", and `character` after it had dexterity's base at 21 for 18 and no attribute point: kadan-agent's sighting of the day before, made again. With this branch's plugins, the same call from the same save: `levelled` true, "levelled: every point spent", `attributes` dexterity 3, the screen closed. Not seen: a raise the game refuses after another was taken, which is the tests'.

A plugin's option ticked through the screen's own function, 2026-10-13 16:27 (#127), in an instance of this session's own with the GOG folder's plugins, the Options screen opened by `input` with "GUI.ToggleFullScreenGUI Options": `movie` with `call` "ExternalCommands.SendMessage" and "ClientOptions.SetClientOptionMessage DAOLibDAOLibFastLoad 0", then the same with 1, each answered `ran` true, and the loader's log said "fast load: turned off in the options screen" and then "turned on", `daolib.ini` written `On=0` and `On=1`. The option's name is "DAOLib" and the name the plugin gave (the loader's is DAOLibFastLoad): with DAOLibFastLoad alone `ran` was true twice and nothing was set. "GUI.HideFullScreenGUI" sent the same way closed the screen. Not looked at: the box drawn ticked.

A movement key held with no key pressed, 2026-10-13 (#123; DAOLib master `802664e`, `dao::input::Hold`), in an instance of this session's own with this branch's plugins, Lyna's manual 4 in the Forest Clearing, explore mode, the window not the foreground one (`window` and the tool's own `foreground` false throughout):

- `steer` with each `direction` and `ms` 1500: `held` the direction with 1499 ms left, `controlled` Lyna, and `eluvian.command.started` from its `cursor` a Drive command, player issued, for her each time. She moved 7.34 forward, 7.12 backward, 1.77 left and 4.57 right (the sideways ones cut short by what stood there), and a second after each hold's end stood where she had stopped, to the hundredth.
- Backward and right held together for 20 s, `held` both, then `release: true` half a second on: `released` the two, 6.03 moved, and nothing more after.
- Refused, each saying what is wanted: no `direction`; `direction` "up"; `ms` 600000; and with the Options screen up, "the game takes movement keys only in explore or combat mode".
- Not seen: a hold in a fight (lib-agent's play has it), one standing when a conversation starts or an area loads, and two games each steered at once, which is what it was asked for (kadan-agent).

A new game and its hero built through the creation screen, 2026-10-18 (#153; lib-agent's reading in DAOLib's dev/CHARGEN.md, asked for at the user's request), in instance eluvian-test, by MCP alone:

- By hand first, with `movie` and `input` (the issue's comment has every call): the start menu's `StartMenu.NewGame` opens the campaign chooser (newgame.gfx), whose `NewGame.ms_nGameSelection` 0 and `NewGame.PlayOutro` load Origins into `game_intro.cut`; `skip_cutscene` brings up creation (mode chargen, screen CharGen, hero 130). The movie's own `CharGenStart` carried creature 0, and the hero stayed at 0 in every attribute until it was sent again with 130. A human male mage built by the screen's messages went to the Harrowing Chamber, Irving's conversation. A `GUI.HideFullScreenGUI` with the movie's exit state unset closed the screen with no `CharGenEnd`, and the game crashed under DragonAgeGUISystem (0x0072bb11); with `CharGen.CreatureID`, `nExitPoint` and `bUserExit` set first, the same hide ended creation.
- With this branch's plugins (a staged folder, `game_launch`'s `plugins`): `game_new` stopped at the chooser the first time, its `PlayOutro` called 17 ms after the screen was shown doing nothing (the same call 3 s on loaded the campaign), so it is called again while the chooser stays up; then `game_new` reached the creation screen in 9 s, the intro skipped. `chargen` `offers` listed the five backgrounds from the game's table with the races and classes each takes. A female city elf rogue with `finish: false`: the screen kept up, the origin's default name Kallian, the row 2303 and `bec110ar_players_house`; `offers` then gave 1 skill and 2 talent points and the learnable ones. The same with voice sultry, name Lyra, dexterity 3, cunning 2, Stealing, Below the Belt and Stealth: every point spent, `exit` full, the screen ended, the alienage's opening with Shianni. A human noble warrior with no points given: the movie's background box closed (`closedBox` its text; the layer gives its clip as a string, which the first build missed, so the box stayed over Lyra's opening until closed by hand), `exit` quick play, Aedan with Strength 18 and nothing left to spend, Castle Cousland's opening conversation, no box left.
- `gui` does not report the movie's background box (`popup` null): DAOLib's `CurrentPopup` does not see a box a movie shows by `GUI.ShowDialogBox`. It does since DAOLib #173 (below).
- Not seen: Awakening's start (its chooser's row 1 is sent, and it may offer to import a hero), the head and its sliders (the screen's appearance page is not driven: the hero keeps the race's default head), a dwarf, and the difficulty, which writes the profile's options.

A box a movie shows read and pressed, 2026-10-18 (DAOLib #173, lib-agent's: `CurrentPopup` reads the popup movie's `PopupLayer.ms_mcPopup`, and `PressPopupButton` closes a box by `PopupLayer.HideAllPopups` and sends the button's text, as a click does), in instance eluvian-test with plugins built against its branch (`cd36dfa`, the same code as master's `f626da2`), by MCP alone:

- The creation screen's background box, which the screen shows as it opens: `gui` gave its text and one button, Close, sending "GUI.InvokeCallback CharGen _root.DialogBoxCallback0". `press` 0: `popup` null, `ms_mcPopup` null, the screen usable in the screenshot.
- Then the scripts' `ShowPopup(379897, 1)` on the same screen, and twice more in the Fade after the Harrowing's opening: each "Shield Pummel", Yes and No; `press` No closed each, the mode went back from gui, and each gave one `eluvian.script.popup_result` with 1 and 2 (the box's id and the button). So the layer was not left refusing boxes, and no game crashed.
- The game's, not the change's: two `ShowPopup`s during the Harrowing's opening conversation showed nothing then, and one box came up the frame the Fade was entered; a `ShowPopup` with that box up added none.
- `chargen` now closes the background box by `gui` and `press` (its workaround, the popup movie's clip read and hidden, dropped), and refuses with a box asking a choice up. With master's DAOLib: a male city elf rogue by Quick Play, `closedBox` the box's text, and the alienage's opening cutscene with no box over it.

A game started behind the window in front, 2026-10-18 (#154): the bridge starts the game with `STARTF_USESHOWWINDOW` and `SW_SHOWNOACTIVATE`, the first showing of its window not activating it. Earlier that morning a copy's main menu, then Options, had been opened by keys nobody sent it, and the user's own copy closed twice as other sessions' games started and ended (#149); the user: "Probably another session is starting their own game and taking foreground". This branch's bridge started instance eluvian-test with no other game running, the window in front read every quarter second from the launch to 10 s after `game_launch` answered, no mouse or keyboard: Paragon's window (paragon.exe) was in front before and throughout, the game never was. `window` gave the client at (160, 111), 1600 by 1000, `minimised` false and `foreground` false; `health` `running`, 763 frames. No launch by the old bridge was made to compare, that being a game brought in front of the user's window.

A copy named on the call, 2026-10-19 (#159): one bridge over stdio, started for the game as ever, with no game running, driving instance eluvian-test by each call's `instance` alone. `tools/list` gave 82 tools, `instance` on 81, all but `game_instance`. `game_status` with it said eluvian-test, not running; `game_launch` with it started that copy (behind, `window` `foreground` false) and answered its health. `game_instance` before and after said the game as ever, listing eluvian-test among the copies running. `health` with no `instance` said the game is not running, and with it gave eluvian-test's. `game_quit` with none said nothing runs, and with it ended eluvian-test's. The launch's `tools/list_changed` was sent by that copy's bridge, which no longer sends any: the client lists the bridge's own copy's tools.

An area without a save there, 2026-10-19 (#157, `warp`), in instance eluvian-test with this branch's build staged, from Lyna's manual 4 in the Dalish origin's Forest Clearing, by MCP alone:
- `waypoints: true` for the Tower of Ishal's top floor (pre440ar_tower_level_4, the single player module's) listed six with tags: pre440wp_from_tower3, three pre440wp_archer, pre440wp_light_beacon, pre440wp_darkspawn_jump. The first call took seconds: the areas are read from the ten designerareas.erf, where the first build's read of `resources`' index of every archive outlasted the bridge's 90 s.
- "tower_level_4" was refused, naming cir230ar_tower_level_4 and pre440ar_tower_level_4; the waypoint "no_such_waypoint" was refused with the area's four tags. Nothing was sent either time. Called through `script` before `warp` was built, DoAreaTransition took that waypoint for the Forest Clearing and put Lyna at (0, 0, 0).
- To pre440wp_from_tower3: arrived in 0.8 s, the hero 0.0 m from the waypoint, the mode cutscene (the area's opening, then its fight with the ogre). A warp during the cutscene was refused; after skip_cutscene, from the fight, the warp back to the clearing with no waypoint arrived in 0.6 s, in explore.
- An add-in's area: str100ar_hut_exterior's file, in dao_prc_str's enciphered archive, is not read, so its waypoints are not known. Without one, DoAreaTransition answered 0 from the Dalish origin and the party stayed where it was: warp now says the area is dao_prc_str's and the module running does not load it.
A frame for drawing again elsewhere, 2026-10-19 (#156, for DAOParagonEngine's parity work; DAOLib #204, lib-agent's `CurrentProjection`, built from its branch at `f2c2317`), in instance eluvian-test with this branch's Eluvian and the game folder's photo cam staged, windowed 1600 by 1000, Lyna's manual 4:
- `camera`'s read answered `projection`: in explore a vertical field of view of 60, aspect 1.6, near 0.15, far 4000, 1600 by 1000; with photo cam flying (flycam mode, where `read` now answers the projection alone) near 0.01, and after `photocam_pose` with a field of view of 40, 40; after photo cam was off, near 0.25, which the game's fly camera mode leaves behind (DAOPhotoCam #1). The aspect times the height was the width each time.
- The matrix: rows (1.0825, 0, 0, 0), (0, 1.7321, 0, 0), (0, 0, -1.00004, -0.15), (0, 0, -1, 0): right-handed, depth 0 to 1, for column vectors, the transpose of D3DXMatrixPerspectiveFovRH, where DAOLib's header said the left-handed one (told to lib-agent).
- `screenshot` at a size: from the window, `width` 800 gave 800 by 500, `height` 720 gave 1152 by 720, 800 by 500 itself; 1280 by 720 was refused, naming 1280 by 800 and 1152 by 720, and with `crop` gave the middle 1600 by 900 scaled to 1280 by 720; `width` 15 was refused. From the game, `width` 800 gave 800 by 500, and 1000 by 1000 with `crop` the middle 1000 by 1000. Each answered `frame` 1600 by 1000.
- With photo cam flying, the game's own picture at 1280 by 720 with `crop` was the frame alone: no HUD, no panel. In explore it has the HUD, as the game's screenshot key gives it.

Rooms turning seen, 2026-10-20 (#164, `rooms`; for DAOParagonEngine #108 through lib-agent-2), in instance eluvian-test with this branch's build staged, `warp` to the Elven Ruins (bed110ar_elven_ruins) from Lyna's manual 4, by MCP alone:
- At arrival 1574, the party's, in view; 1575 to 1580 never; seven rooms.
- The door between 1574 and 1579 (bed110ip_door_south, story-locked, set shut by script), the party standing in 1574: opened, 1579 never to in view within 1.5 s; shut, in view to before; opened, before to in view.
- Walking through in steps of at most 3 m: on entering 1579, 1578 and 1580 in view, unentered; in 1578, 1574 to before; a member in 1577, 1577 in view, and a step on 1576 in view, unentered; 1579 to before once nobody was in 1578, and back in view from 1576 and 1577. 1575 stayed never.
- `camera follow` on a creature in 1575 (never seen): the game's and the window's pictures pure (0, 0, 0) over the middle, under the interface.
