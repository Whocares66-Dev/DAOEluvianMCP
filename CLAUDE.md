# Eluvian MCP -- project context

A test bench inside Dragon Age: Origins for agents: `eluvian.dll`, a DAOLib plugin serving the running game over MCP (`POST /mcp`) and plain HTTP (`/api/`) on 127.0.0.1. An agent asks the game questions and gives it orders instead of driving it by screenshots and reading logs after. Skyrim's devbench (`C:\modding\devbench`) is the model. `dev/DESIGN.md` is what it does and why, the tools, the events, the C ABI, and what DAOLib gives it. `dev/TESTING.md` is how to check it in the game and what was seen there. `dev/PLAYBOOK.md` is for the agent driving a test through it: the rules, a turn, the mod's names. `README.md` is how to install, connect, build and deploy. `dev/GHILANA.md` is ghilana, a second MCP server built here (`ghilana.exe`, 64-bit, stdio): the tools that need no game, the game's files read on disk over DAOLib's core (#128, DAOLib #106); what it does, where it stands, and what is next. `dev/RELEASING.md` is how a release is cut (`tools\release.ps1`, `CHANGELOG.md`): what a version says, the DAOLib release it is held at, what the zip holds, and the zip seen in the game first.

Markdown here is not hard-wrapped: one line per paragraph or list item.

## The target and the siblings

DAOLib's `CLAUDE.md` has the game, the one exe build every address is from, the toolchain and where logs go. Everything is 32-bit x86; `CMakeLists.txt` refuses a 64-bit configure.

- `extern/DAOLib` is a git submodule (DAOLib's own repository), built with this one as DAOTurnBased builds it; without the submodule CMake falls back to the sibling `..\DAOLib`, and `-DDAOLIB_DIR=../DAOLib` builds against the sibling's working tree meanwhile. A change DAOLib needs is asked of the DAOLib session (lib-agent) or made in DAOLib's own repository with its rules, then the submodule moved to it. A release holds the submodule at one of DAOLib's release tags (0.1.0 at `v0.1.0`), and `tools\release.ps1` refuses at any other commit; between releases it is wherever the work needs, since #116 on DAOLib's master for its second copy of the game (`dao::Module::Instance`), since #120 past `fd43f73`, whose loader keeps the game's own screenshot from reading past its file name, since #122 past `a50ae77` for `dao::OwnsLogs` (the folder's owner has the plain file names), since #125 past `3dad3b8`, whose loader is `dinput8.dll` (its target `loader`) where it was `version.dll`, since #123 past `802664e` for `dao::input::Hold` (a movement key held, under `steer`), since #129 past `7ad565b` for a campaign's packages (`install::Index::Packages`, Awakening's `core_ep1`) and `dds::Describe` (#130), since #132 past `123f1eb` for a model read and written back (`dao/core/Model.h`, DAOLib #116 and #117), since #136 past `6c4c1b0` for new chunks and mesh nodes (DAOLib #118), since #137 past `82ddbb7` for new bones (DAOLib #119), since #135 past `1fbd02d` for a texture written as the game's DDS (DAOLib #120, over DirectXTex, so `vcpkg.json` has `directxtex`), since #141 past `28824f2` for a table's new rows, a template's fields and a model renamed (DAOLib #123), since #140 past `956056d` for a mod packed as an `.erf` or a `.dazip` (DAOLib #122), since #144 past `5258828`, DAOLib's 0.3.0 and #141, whose saves by name no longer leave a copy of the `.das` in `bin_ship` (the game copies a save to the SaveGameMessage's own name, relative to its working folder; its save screen leaves that empty), and whose `dao/Camera.h` is what `dao/CameraMoves.h` was (#126). That is past the v0.3.0 tag, so the next release asks lib-agent for a tag at or after it (lib-agent offered a 0.3.1, 2026-10-15); and since #145 and #146 at `98ecc70` for a game's staged plugins folder (`install::Options::plugins`, DAOLib #143) and the archives' directories kept between ghilana's processes (`Options::cache`, DAOLib #144); and since DAOLib #173 at `f626da2`, whose `dao::gui::CurrentPopup` and `PressPopupButton` see and close a box a movie shows itself (the creation screen's); and since #156 at `0169374`, whose `dao::camera::CurrentProjection` (DAOLib #204) answers the projection a frame is drawn with, and whose core indexes the sound banks among an install's loose files (DAOLib #191); and since #165 past `4f9d82c`, whose `dao::animation::OnPlayed` (DAOLib #217) calls back on a creature's play call, and whose GFF3 field labels are `string_view`s; and since #166 at `2713656`, whose `dao::audio::OnEventStart` (DAOLib #218) calls back on each FMOD event the game starts; and since #167 at `ccf32ed`, whose `dao::clock::Now` (DAOLib #232) reads the world clock a save writes and a load restores; and since #160 at `ed50c8f`, whose `dao::render::Watch` (DAOLib #246) hands a plugin an armed frame's draws, each shader's constants read by `core::ctab`; and since #168 at `4e357a7`, whose `render::Draw` says the material, model, mesh and graphics object a draw is of (DAOLib #247); and since #169 at `0dd151d`, whose `install::TalkTables` and `Strings` (DAOLib #106) are the talk tables the game searches in its order, which ghilana's `strings` reads; and since #170 at `031ee28`, whose `dao::core::tables` (`generated/GdaSchema.h`, DAOLib #106) says what each merged worksheet's columns hold, which ghilana's `references` reads a table by; and since #171 at `af48347`, whose `dao::core::templates` (`generated/Gff3Schema.h`) does the same for a creature's and an item's template fields, the rows they name among them; and since #173 at `6822a9a`, whose `dao/Messages.h` types the abilities' messages (DAOLib #253), PerformAbilityMessage's third word the ability's item, which `order`'s click gives; and since #174 at `d9049e8`, whose `Send(CGameObject *, ...)` reaches a game object's message receiver (at +0x18), so `equip` sends EquipItemMessage and UnequipItemMessage to the creature itself; and since #177 at `b321ffe`, whose `dao/Creatures.h` and `dao/GameModes.h` (DAOLib #255) say a death on its frame, a revival, a made creature ready, an area loaded and the game taking input again; and since #180 at `8730dc8`, whose `dao/LevelUp.h` (DAOLib #259) is a level-up with no screen, its offers, specializations and steps refused by the engine's own tests, which `levelup` takes its points through; and since #181 at `1ef662d`, whose `dao/Loot.h` is the loot panel's container, Take All and Close as its buttons do them, which `gui` and `press` use where they sent the buttons' texts; and since #182 at `e3ec1e1`, whose `dao/PartyPicker.h` is the party picker's entries, a choice and a release as a click does, Remove All and Accept, which `party` works it by where it swept the cursor; and since #184 at `32073ca`, whose `dao/WorldMap.h` (since `f9754a7`) travels to a pin of the world's map as its click does, which `travel` clicks by where it set the map's mark and index and sent the transition itself (the Fade's map and a city's two still its own); and since #185 at `0fa042f`, whose `dao/Store.h` is the open store, its stock and the store's own prices, a buy and a sale refused as the screen refuses, and its close, which `store` trades by where it sent the screen's texts and summed the prices itself; and since #187 at `2e49ef3`, whose `dao::party::SetScreenCharacter` sets the character the full screens act on by their own arrows, control left alone, which `tactics` selects a follower by where it handed over control; and since #186 at `9330358`, whose `dao/Crafting.h` is the party's recipes of a craft as the screen shows them, with their names, and one made as its Create does, with no screen, which `craft` makes by where it drove the screen and handed the maker control; and since #188 at `76e649b`, whose `dao/Tactics.h` is a follower's slots, the screen's offers from the engine's list builders, a slot set as its pickers set it, the presets and the switches, with no screen or selection, which `tactics` sets by where it asked a shown screen's FSCommands and wrote the data tree's row; and since #189 at `c3ced45`, whose `dao/CharGen.h` is the creation screen's offers and choices as it makes them and its end as its Play buttons end it, which `chargen` builds the hero by where it sent the screen's texts and drove its movie; and since #190 at `d87db2e`, whose `dao/Options.h` (DAOLib #272, #273) drives the options screen as a player does, the game's own controls and the plugins', which the new `options` works it by. DAOLib has a submodule of its own, the address library (`extern/DAOLib/extern/DAOAddressLibrary`): `git submodule update --init --recursive`.
- **The loader and every plugin in the game folder are of one plugin API** (`dao::kPluginApiVersion`, 2 since DAOLib `8b027c6`): a loader of another refuses Eluvian, and Eluvian it (`eluvian.log` says so, and the bridge's `game_launch` and `game_status` repeat it as `why`). So a move of DAOLib that changes the API is deployed with the turn-based mod's, at a moment agreed with turn-based-agent. Since DAOLib #26 each plugin lives in a folder of its own, `bin_ship\daolib\plugins\<name>\<name>.dll`, and a loader of #26 loads none left loose: `tools/deploy.ps1` copies `build\<preset>\daolib\plugins\eluvian\` there and removes a loose `eluvian.dll` (#69), and a change of layout, like one of API, is deployed with the turn-based mod's. `tools/deploy.ps1` also copies the address library's files (`build\<preset>\daolib\addresses\*.bin`), without which the loader loads nothing. A log line's second bracket is the thread that wrote it, not the game's pid.
- `C:\project\DAOTurnBased` is the turn-based mod, the first thing Eluvian tests. It registers its own tools and events through `src/api/EluvianAPI.h`; its session is turn-based-agent.

## Layout

```
src/core/     the bench without the game: HTTP parsing and routes, MCP's JSON-RPC, REST, the tool
              registry, the event bus and its file (EventLog), the queue to the frame thread, arguments, logs
              from a cursor, the game's archive indexes (Erf), and the tools that only decide over other
              tools' answers (scenario, travel, loot, levelup, craft).
              No <Windows.h>, no DAOLib, no dao:: type. Tested.
src/server/   the socket loop under core/Http.h: Winsock, 127.0.0.1, a thread per connection. Tested
              over loopback.
src/game/     the tools that read and drive the game, through DAOLib, a file each (Inspect, Script,
              Character, Order, Stage, Screenshot, Screen, Game, Input, Camera, Dialogue, Resources, Gui; Shared.h is
              what they share;
              GameTools.cpp registers them and puts the engine's events on the bus), and health.
              Verified by playing.
src/ghilana/  ghilana.exe (dev/GHILANA.md): its tools over DAOLib's core (dao/core's Install, Erf,
              Gff3, Gff4, GdaFile, M2da, TalkTable, Dds), the JSON its answers give, the PNG of a texture,
              and Folders.cpp, the one file that asks Windows (the installs, the known folders). A
              reader of the game's formats goes in DAOLib's core, not here (the user, 2026-10-12).
              Tested in tests/ghilana, over files DAOLib's tests/files_builder.h makes
src/plugin/   DAOLib_Load, the frame pump, the host side of the C ABI (Eluvian_GetInterface); Tools.cpp
              registers every tool, for the plugin and for eluvian_describe (Describe.cpp), which prints
              them and the topics without the game, for the docs site
src/api/      EluvianAPI.h: the C ABI other plugins include. MIT, unlike the rest (GPLv3).
tests/        Catch2 over src/core; tests/server over src/server
tools/        build.ps1, deploy.ps1, console.ps1, check-powershell.ps1 (DAOTurnBased's, for this tree);
              package.ps1 and release.ps1 (DAOLib's, for this tree: the zip a release carries, and a
              release of it; daolib_release.ps1 is what both ask of the submodule);
              eluvian_bridge.py, the stdio MCP server an agent keeps across game restarts (.mcp.json;
              `--call TOOL [JSON]` for one call from a shell; a build copies it beside eluvian.dll,
              where a release has it),
              test_bridge.py, run by ctest, and smoke.py, every tool once in the game (not
              in ctest: it needs the game and a save); mutants.py (DAOTurnBased's), which
              breaks src/core on purpose, one promise at a time, and wants each caught
docs/         the docs site (Just the Docs): its own short front page and Getting started; tools/
              build_docs.py makes the rest from src/api/EluvianAPI.h and _data/eluvian.json (eluvian_describe's output,
              kept current by ctest's docs.reference_current: `python tools/build_docs.py --update`);
              .github/workflows/docs.yml publishes it from main
dev/          the design
extern/DAOLib the submodule
```

**Eluvian is general, not any one mod's.** It serves the game and the bench; a mod's state, actions and events come from the mod through `src/api/EluvianAPI.h`, under its prefix, documented by the mod. A tool, event or doc line only one mod would want is that mod's to register, and no agent-facing text here names a mod (the turn-based mod was the first user; history in comments is fine). `dev/DESIGN.md`, Principles.

**The one architectural rule: `src/core` never includes `<Windows.h>`, links DAOLib or names a `dao::` type.** What can be decided without the game belongs there, with a test.

**Threads.** The listener's threads never touch game memory. A tool runs on the frame thread (`dao::OnFrame`, the scripts' thread, paused or not) unless declared listener-thread; `core/FrameQueue.h` takes it there and brings the answer back, and abandons a call no frame takes in time. Listener tools (`health`, `events`, `wait_for`, `log`) answer while frames stop. Script commands are not called while an area loads (lib-agent, 2026-09-30).

**Why no HTTP library.** cpp-httplib, the obvious one, refuses 32-bit Windows in vcpkg (2026-09-30); `core/Http.h` is the little HTTP the bench needs, tested.

**The game's speed belongs to one plugin.** Each plugin's copy of DAOLib hooks the frame's timer on its first `dao::clock::SetSpeed`, so two setting a speed multiply, and `Speed()` and `Held()` read back only the caller's own (DAOLib `ed55ab8`). In a turn-based test the mod owns it (`ai_turn_speed`); Eluvian neither sets nor reports it.

**Tool names reach a model**: letters, digits, `_` and `-` only (a plugin's: `turnbased_state`). Event topics may use dots (`turnbased.turn.ended`). A tool is one concept, its own arguments in its own schema: no tool of kinds (#110, `dev/DESIGN.md` Principles).

## Building

```powershell
.\tools\build.ps1 -Preset core -Test            # src/core and its tests only: the fast loop
.\tools\build.ps1 -Preset debug -Test           # eluvian.dll, DAOLib's loader, all tests
.\tools\build.ps1 -Preset debug -Format -Lint   # formatters, then clang-tidy, ruff and PSScriptAnalyzer
.\tools\build.ps1 -Preset release -Test         # what the game plays (RelWithDebInfo)
.\tools\deploy.ps1                              # the release build into the game (closed); -Remove takes it out
.\tools\package.ps1                             # the release build as build\package\DAOEluvianMCP-<version>.zip
.\tools\build.ps1 -Preset ghilana -Test         # ghilana.exe, 64-bit, its tests and the core's; ghilana-release ships
```

The debug preset builds ghilana too (32-bit), so the gate below tests, formats and lints it; the `ghilana` presets are its 64-bit builds, with no plugin.

**The game plays the release build** (#76): optimised, the release runtime, its pdb kept so crash and hang logs name our frames. The debug runtime validates every allocation and free under the process heap's lock, and both hang samples of 2026-10-05 stood there (lib-agent). The mod moves with it, at one restart agreed with turn-based-agent. The gate below stays debug. It is also what is released (#115): its link is not incremental and starts its own PDB, and `build.ps1` keeps a build's temporary files in its tree, since the linker writes their path into the PDB and the user's own is under their name. So what is packaged is built by `build.ps1` and nothing else (`dev/RELEASING.md`).

## Before every commit

```powershell
.\tools\build.ps1 -Preset debug -Test -Format -Lint
```

That is all a commit on a feature branch needs (`-Preset core -Test` is the faster loop while the work is in the core); a build for play (a `deploy-*` branch) adds the `release` build it deploys, `-Preset release -Test`. The rest is the merge's (the user, 2026-10-09: "We should only run them when ready to merge to master. If we find an issue later in dev, we can just fix it on the dev branch. We squash before merging to master anyway"; until then the whole of it ran before every commit and every build for play, and three gated side by side kept the machine loaded for a night).

## Before a merge to master

Once, on the commits that will merge:

```powershell
.\tools\build.ps1 -Preset debug -Test -Format -Lint
.\tools\build.ps1 -Preset debug-asan -Test
.\tools\build.ps1 -Preset debug-analyze
.\tools\build.ps1 -Preset release -Test
```

A change to `src/ghilana` runs its suite's, from a shell `build.ps1 -Preset ghilana` set up (64-bit): `.	oolsuild.ps1 -Preset ghilana; python tools\mutants.py --suite ghilana` (20, a minute). A change to `src/core`'s logic also runs its own mutants, by name (from the shell `tools\build.ps1 -Preset core` ran in: `.\tools\build.ps1 -Preset core; python tools\mutants.py -j 3 "name" ...`): the ones it adds, and the ones whose code it moved or changed. Each must be caught; a new promise gets a mutant, and a survivor the test that catches it. The whole set (`python tools\mutants.py`, about an hour) is only for a change to what every promise stands on: the bench, the event bus, the test stand-ins (the user, 2026-10-10: "Can't you just run the one you added?"). Each mutant breaks one promise with a small edit; the workers each break their own copy of the core under `build\mutants\wN`, at below normal priority, so the working tree is only read (`-j N` sets how many at a time; names filter). The tests run there with every core busy and twenty copies at once, so a test that needs millisecond scheduling, or shares a temporary file's name with its copies, fails there with nothing broken and counts a mutant caught that is not (three such, 2026-10-03): a mutant caught that should not be is run alone (`-j 1` and its name). A mutant's text is the code as the formatter leaves it: write and run mutants after `-Format`, or a later format leaves them matching nothing (four such, 2026-10-03). `-Preset core-cov -Coverage` shows what the tests never run; the mutants, what they run without checking.

Last green 2026-10-12, with the files beside the logs named by the folder's owner (#122) and `game_events` reading the served game's (#121), against DAOLib master `a50ae77` (plugin API 2, Eluvian's C interface 3; `3dad3b8` since #125): 442 ctest entries since #126, the tests, the bridge's and the docs' reference check, each its own run in parallel; clang-tidy, ruff, PSScriptAnalyzer, ASan and /analyze clean; no mutant run for #116 to #125, which changed nothing in `src/core` (#126's 5 were caught 2026-10-13, #117's 6 were caught the same day, the eight ports' 10 the day before, the other 393 last run whole with #109 and #112). `-Preset core-cov -Coverage` showed 95% of `src/core`'s regions and 96% of its lines run by the tests (2026-10-02). Seen in the game through then (`dev/TESTING.md`): reads, orders, staged fights, loading a save from the menu, a whole test driven with no computer use, and since 2026-10-02 a playthrough driven by MCP alone (turn-based-agent's, DAOTurnBased#3), whose needs are this repository's issues. A test on the player's own playthrough loads only, or writes a test save of its own by name; the game's own autosaves are allowed. Work on an issue goes on a branch `gh-<N>-<description>`, in a worktree under `.claude/worktrees` (DAOTurnBased's tools run `tools/eluvian_bridge.py` from this tree, so it stays on main), is seen in play, and is squashed into main.

## Writing files from the shell

The Bash tool turns `\\` into `\` and `\r`, `\n`, `\t` into control characters inside a heredoc (DAOTurnBased's note; it happened here too, 2026-09-30), and Python's text mode writes CRLF on Windows. Write sources through the Edit or Write tools, or from a Python file that writes bytes with LF.

## Style

As DAOLib's: clang-format's stock Microsoft style, lint for correctness only, `/W4 /WX`. Comments say why and what was read, with dates, not what the code does. A file's head says what it is for and how it fits. A tool's description is written for an agent reading it cold: what it answers, what to pass, what to do next.
