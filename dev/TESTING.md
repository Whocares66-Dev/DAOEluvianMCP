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
