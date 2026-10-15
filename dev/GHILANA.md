# Ghilana

Ghilana is the MCP server of the tools that need no game: Dragon Age: Origins' files and formats read, searched and compared on disk, and later written into a mod. It is to the game's data what houseCARL (`C:\modding\houseCARL`) is to a Skyrim load order, and DAOLib's core is its Mutagen (`C:\modding\Mutagen`). Eluvian serves the running game; ghilana serves the game's files, with or without the game running. Issue #128 here, and DAOLib's #106 for the library under it.

This was the plan of 2026-10-12, written for the agent who builds it; the first version was built the same night on branch `gh-128-ghilana` ("Where it stands" says what is done and what is next). Its tools are `ghilana.exe --describe`'s list.

## What was decided, and why

- **A library like Mutagen, powering a server like houseCARL** (the user, 2026-10-12: "the goal is to have something like elder scrolls' mutagen ... which powers housecarl").
- **The library is DAOLib's C++ core, not a Python package.** Core already reads most formats (tested, mutation-tested), ElgaraShaders' offline baker is built on it, and plugins in the game get the same readers. A second implementation would make permanent what exists now: six ERF readers in four repos (DAOLib #106 lists them). DAOLib's Python readers (`tools/gff4.py`, `gda.py`, `tlk.py`) stay only until nothing calls them.
- **No separate repository.** The library is `src/dao/core` in DAOLib, which every DAO repo already has as `extern/DAOLib`; ghilana lives in this repo (the user, 2026-10-12: "a separate mcp server, but one that lives in the eluvian repo"). If the library one day has users outside these repos, or DAOLib's game-tested releases hold back file-only changes, core can be moved out with its history, since it depends on nothing else.
- **Its own MCP server, not tools behind the bridge.**
  - Ghilana needs no DAOLib in the game: a modder or an agent reads files, conflicts and tables with nothing installed there, as houseCARL needs only an MO2 folder.
  - MCP clients run several servers and namespace their tools (`mcp__ghilana__table`, `mcp__eluvian__creatures`), so the server says whether a tool needs the game. Nothing is merged or routed.
  - An agent loads the tools of the server it uses, not both lists.
- **64-bit** (the user, 2026-10-12: "make daolib core 64-bit buildable and avoid the potential problems of 32-bit for ghilana"). An index of every resource and typed caches have no place under a 2 or 4 GB ceiling, and none in the game's own process.
- **Writing goes to a new mod, never into the game's files**, as houseCARL writes a new plugin by default.

## Where it stands (2026-10-13)

- **DAOLib**, master `2be6e02`, which `extern/DAOLib` is held at (`install::Index::SortFiles`, which orders a table's files by source and then by name, for `table`): #106 steps 1 and 2 (`daolib_core`, 64-bit; `Erf.h` with ERF V2.2's directory and `erf::Archive`), the readers first written in `src/ghilana` and moved there at the user's word (`Gff3.h`, `GdaFile.h`, `M2da.h`, `AddIns.h`, `Install.h`), and **the game's order**, read by lib-agent in the exe and seen in play (`dev/FILES.md`): `install::Index` gives each resource's copies in the order the game looks for it, the one it takes first, each with its priority, why the game does not have it where it does not (another campaign's, a level's own, another language's or texture quality's, a disabled add-in's, a folder it does not read), and whether its place rests on what was not seen (an AddIns.xml with an add-in disabled or with no folder). The tie order of add-ins of equal `Priority` is reproduced (`AddInOrder.h`: MSVC 2005's `std::sort` over AddIns.xml from its last item). **The M2DA merge**, read by lib-agent in the exe and seen in play (`dev/FILES.md`, Merged tables): a table takes every file its worksheet's prefix starts, in the order the game lists its files, and of an ID's rows keeps the first file's; m2da_base's IDs 10001 to 10999 are single tables of one file.
- **Here, branch `gh-128-ghilana`**: `ghilana.exe` with the first version's ten tools, `--call`, `--describe`, `--campaign` and `--textures` (what the game is taken to be doing, which decides some of the copies it has), the `ghilana` and `ghilana-release` presets; `core/Stdio.h`, MCP's stdio transport. Every answer gives copies in the game's order; `conflicts` names the copy the game takes and, for a source, how many it loses; a resource read by its name is the game's copy; `table` gives a table as the game has it: each file's game copy, in the game's order, of an ID's rows the first file's, each row naming the files whose rows it overrides (`_over`), `dropped` showing those, `_unsure` where an enciphered file comes before (on the GOG install achievements row 25 is Awakening's 445854 over the core patch's -1, and aip_archer 12 rows, as the game had them in play; Projectiles is 67 of the game's 76, the rest in four enciphered add-in files, said); `query` reads the game's copy of each resource unless asked for all. Seen on this machine's GOG install over stdio. Its guess of which copy matters is gone.
- **A campaign's packages** (#129, DAOLib #113, master `7ad565b`): every `packages\` folder but core is a source, `package:<name>`; the loaded campaign's `.cif` (`CampaignCIFPackagesList`) says which load, ahead of the core, as the engine adds them. Awakening's names `core_ep1`, as do the Awakening-side add-ins; the main campaign's names none. `environment`'s `game_state.packages` gives them. On the GOG install with `--campaign DAO_PRC_EP_1`, `pn_hlf_s01b.mao` is core_ep1's (747 bytes, 70006) ahead of core's two (788); with "Single Player", core's, core_ep1's listed as another campaign's (2026-10-13). Not seen in a running Awakening game: no save of it on this machine.
- **A creature of a mod's own** (#141, DAOLib #123's `GdaFile::Write`, `gff3::Editor` and `model::Renamed`): `table_rows` writes a table of new rows for one the game merges (`apr_base` and every `apr_*`), each copied from a row of the game's with cells changed, and refuses an ID the merged table already has (the game's, its DLC's, a mod's); `template_edit` writes a template under a new name with fields changed by path (`Appearance Data/Appearance_Type`); `model_copy` writes a model's `.mmh`, `.msh` and `.phy`, and its farther versions, under a new name, optionally with another material, and its textures under the new name too: a creature's body is drawn with textures named after its model, whatever its material names (a copy whose textures kept the old name cast its full shadow and drew no body). Seen in the game 2026-10-14: `red_ogre.utc` on a new appearance row 7701 whose model is the ogre's copied as `cn_bdy_rdoa` with a red diffuse drew red beside a stock ogre, unchanged, and fought it. Its name is still the ogre's: a name of its own needs a talk table, which ships in a `.dazip` (#140).
- **A head's morphs guarded** (#138, DAOLib #121's `Morph.h`): `model_import` reads every `.mor` the game has (1,651, about a second and a half after the index) for those whose parts name the model, and refuses a chunk given back with another vertex count than a morph's mesh node places, since a morph places every vertex by index; the answer's `morphs` names the files and nodes, and says a position edit there is replaced on a morphed head. Seen over the game's files 2026-10-14: `hm_uhm_basa_0` is named by 622 morphs, all placing `FaceM1`'s 2,282 vertices; it comes back unchanged straight from `model_export`, and is refused through Blender, whose import alone splits it to 2,342 at its UV seams. Editing a morphed head in Blender needs the split vertices welded back by their original index (#139).
- **Textures, a round trip through an image editor** (#135, DAOLib #120's DDS writer over DirectXTex, the core's first dependency, so `vcpkg.json` has `directxtex`): `texture_export` writes a texture's largest picture as a PNG into a folder of the caller's, a normal map (a name ending in `n`, as the game's are, or `normal`) with x, y, z in red, green and blue as an editor shows one (`dds::Normals`; the game keeps x in alpha and y in green, its shader reading `.wy`). `texture_import` writes a PNG back as the game's DDS in the original's format (DXT1, DXT5, A4R4G4B4, 32 bits) with every halving down to a pixel, a normal map always in the game's layout, and the game's smaller copies for its farther models (`_0dl2`, `_0dl3`) from the picture halved; a side that is no power of two is refused. Seen in the game 2026-10-14: the ogre's `cn_bdy_ogra_0d` tinted red in the PNG and its `_0n` round-tripped untouched drew red with its bumps and lighting as stock, clean from the tactical view too, no crash.
- **Models out, the first half of a round trip** (#132, DAOLib #116 on its branch `gh-116-model-reader`, `b6598c0`): `model` says what a model is made of (its hierarchy, `.mmh`, and the mesh file it names, `.msh`: nodes by kind, bones, each mesh node's chunk with its material, counts, vertex elements, bounds, skinning), and `model_export` writes it as glTF 2.0 (`Gltf.h`) for Blender: the node tree, every vertex element glTF has a place for, the bones as one skin. Z up is made glTF's Y up; a chunk's blend indices, which count in its mesh node's bones, are made the skin's joints; the file's names and kinds go into `extras` (`dao`), which Blender keeps as custom properties, for the way back (DAOLib #117's writer). There is no bind pose in the `.mmh`; the nodes' own transforms are taken for it (lib-agent's reading), and a joint's inverse bind matrix takes the mesh node's place in, as a mesh's vertices are in its node's space. The ogre's body (`cn_bdy_ogra_0.mmh`, 133 nodes, 99 bones, 9,652 vertices) passes the Khronos validator with no error, and Blender 5.2 imports it whole: its 9,652 vertices kept, its 99 bones, the body in its bind pose through the armature, nothing mangled, its triangles facing their normals with no winding turned (2026-10-14). Seen in the game 2026-10-14: the ogre made larger in Blender, written back and put in Documents' `packages/core/override`, drew textured and skinned in its idle pose beside a stock one shot from the same camera, with no crash. **New bones** (#137, DAOLib #119): a joint of the glTF's skin that is none of the model's bones is a new bone, under the nearest node above it the model has (or a new bone made before it), where the glTF has it at rest relative to that node; it takes the next bone index and exports of its own, and no animation names it, so it moves with its parent. Seen in the game 2026-10-14: two cones over the ogre's head, one weighted to its Head bone and one to a new bone 99 under it, each a new chunk, followed the head idle and in a fight, the body animating as before. A small dark cone 0.45 m above the head at rest hid inside the head and horns in play, since the hunched idle pitches the head far forward: test with something large. The answer's `lods` names the farther versions (`_2`, `_3`), models of their own the game draws at a distance, which stay stock unless edited too. **New meshes** (#136, DAOLib #118's added chunks and mesh nodes): a glTF mesh that is none of the model's chunks, or a second one claiming a chunk (Blender's duplicate keeps the extras), is written as a new chunk after the original's, with its own declaration copied from the chunk it claims (else the first drawn), under a new mesh node like that chunk's: the same parent, place and settings, the glTF's material, and only the bones its own vertices use. Seen in the game 2026-10-14: the ogre cut in two in Blender, one half its own body and the other a duplicate over 26 bones of its own, drew whole, in idle and in a fight. A mesh weighted to more than 67 bones (the skinning shaders' palette) is split: each triangle, in the file's order, goes to the first piece its bones fit, the first piece in the chunk's place and the rest new chunks under mesh nodes like its own (`SplitByBones`); seen in the game 2026-10-14, the ogre's body given a weight of 0.001 on 8 more bones (72) came back as 67 and 22, whole in idle and in a fight. A copy moved 2 m off its bones tears when they turn, as any skin would. `model_import` reads its base where the game would, so a model of its own left in the override folder is the base: take it out first. **The way back** (DAOLib #117's writers, master `123f1eb`): `model_import` reads a .gltf or .glb (`GltfImport.h`), matches each mesh to its chunk by the extras the export wrote (Blender keeps them with `export_extras=True`) or by name, puts each vertex where the glTF has it at rest in its mesh node's space, makes the binormal w * cross(N, T), tangents from the texture coordinates where Blender exported none, and each vertex's joints the model's bones by name (a mesh node's list grown where an edit needs a bone it did not list; no bone added to the model; at most 67 a chunk, the skinning shaders' `float4 bsm_bones[201]`, lib-agent on DAOLib #117). The writers keep everything else of the original files. The ogre's body through glTF and back with no editor: the same file size, its triangles, bones and weights the same bytes, 7,798 half floats off by one step of their rounding (at most 0.0005), the .mmh's one box made again from the mesh as kept. Through Blender 5.2, made 40% wider there: 10,093 vertices where 9,652 were (Blender splits them at seams), and read back through ghilana and Blender again the same bounds as Blender's own export to 0.002 (2026-10-14). Not yet: bones moved in Blender, nodes added, a chunk past 67 bones split, textures, and the model seen in the game.
- **Not yet**: the cache of contents on disk (`query` and `references` read the files each call: 7,235 creature templates in 1.5 s in a release build, after the index); GDA column names beyond DAOLib's `dev/gda_tables.csv`; `schema`, `diff`, `dialogue`, `scripts`, `save`; writers (#106 step 3) and the authoring tools; the release zip and the Claude Code plugin; an Eluvian tool over DAOLib's `dao::resources::SourceOf` to check ghilana's answer against the running game.

A change DAOLib needs is asked of DAOLib's session (lib-agent) or made in DAOLib's repository under its rules (its `CLAUDE.md`), then `extern/DAOLib` moved to it. **A reader of the game's formats goes in DAOLib's core, not in `src/ghilana`** (the user, 2026-10-12).

## Architecture

**The process.** `ghilana.exe`, 64-bit, an MCP server over stdio: newline-delimited JSON-RPC on stdin and stdout, logs to stderr and a file. No port, no HTTP, no bridge.

```
claude mcp add ghilana -- ghilana.exe --game "C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins"
ghilana.exe --call table "{\"table\":\"abi_base\",\"row\":10000}"     # one call from a shell, as the bridge's --call
ghilana.exe --describe                                               # every tool, for the docs site, as eluvian_describe
```

Arguments, each optional where it can be found:

- `--game <folder>`: the install, the folder that holds `bin_ship`. Default: found as in "Finding the game" below.
- `--documents <folder>`: `Documents\BioWare\Dragon Age`. Default: the user's Documents folder as Windows reports it (`SHGetKnownFolderPath`; on this machine under OneDrive).
- `--instance <name>`: a named instance's Documents folder (DAOLib's second copy of the game, `dao::Module::Instance`), which has its own `Settings\AddIns.xml`.
- `--language <code>`: the talk tables' language, `en-us` unless said.
- `--cache <folder>`: where the index is kept. Default `%LOCALAPPDATA%\Eluvian\ghilana`.

**What is reused, what is new.**

- `src/core`: `Mcp` (JSON-RPC, initialize, tools/list, tools/call; transport-free already), `ToolRegistry`, `Tool.h` (`ToolSpec`, `ToolResult`, `ToolError`; a result's `content` can carry an image, as `screenshot` does), `Args`, `Base64`. Every ghilana tool runs on the calling thread (`Thread::Listener`'s meaning; there is no frame thread).
- New in `src/core`: a stdio transport, one JSON-RPC message per line, tested without a process. It reads nothing of DAOLib, so it keeps core's rule.
- New, `src/ghilana/`: the tools, over DAOLib's `daolib_core`. They read files and nothing of any process, so they are tested with ctest like core (the files built in the test by DAOLib's `tests/files_builder.h`, which DAOLib's own tests of the readers use), plus tests over a real install that are skipped where there is none. The rules for this folder: **no `<Windows.h>` but in the one file that finds folders** (`Folders.cpp`: the registry, known folders), never a `dao::` name outside `dao::core`, and **no reader of a game format**: that is DAOLib's core's. What is here is what an answer needs over core: the JSON of a file (`FileJson.h`), the copy a call names and the guess of which matters (`GameFiles.h`), the PNG of a texture (`Png.h`, over libspng from vcpkg: an encoder of ghilana's own, its deflate and filters, went for it at the user's word, 2026-10-12), names for GFF4 fields and GDA columns from DAOLib's CSVs (`Names.h`).
- `src/game`, `src/plugin`, `src/server`, `tools/eluvian_bridge.py`: Eluvian's, untouched.

**The build.** This repo already builds its core alone and 64-bit (`EL_BUILD_PLUGIN` off; the `core-cov` preset is x64). Ghilana is another such tree:

- an option `EL_BUILD_GHILANA`, and a preset `ghilana` (x64, `x64-windows-static-md`, `EL_BUILD_PLUGIN` off), and `ghilana-release`;
- `add_subdirectory(extern/DAOLib)` there gives `daolib_core` alone, since DAOLib in a 64-bit configure is core-only;
- `extern/DAOLib` has to be at a commit with `daolib_core`, so this waits on DAOLib's `gh-106-core-target` being merged (or `-DDAOLIB_DIR=../DAOLib` against a checkout of that branch meanwhile);
- `tools\build.ps1` imports `vcvarsall amd64` for it, as DAOLib's `build.ps1` now does for `core-x64`.

**One rule between the two servers.** A tool that reads the running game is Eluvian's; one that reads files is ghilana's, whether the game runs or not. A question that needs both (ghilana says this copy wins; did the game load it?) is answered by the agent calling both.

## What it builds on

**DAOLib's core** (`extern/DAOLib/src/dao/core`), on master:

| Format | Reader | Missing |
|---|---|---|
| ERF V2.0 (`.erf`, `.rim`) | `Erf.h`: the directory, and `erf::Archive`, files read by name | writing |
| ERF V2.2 (the add-ins', enciphered) | `Erf.h`: the directory (names, sizes, flags) | deciphering (xoreos `src/aurora/erffile.cpp`, `kEncryptionBlowfishDAO`: Blowfish under a numeric password whose MD5 the header holds; where the game gets the password is unread) |
| GFF V4.0, V4.1 | `Gff4.h` | writing (#106 step 3); lists of strings, 64-bit numbers and vectors (said `_unread` in answers) |
| GFF V3.2 (templates: `.utc`, `.uti`, `.utp`, `.utt`, `.are`, `.bic`) | `Gff3.h`  | writing |
| TLK 0.2 | `TalkTable.h` | writing (DAOLib's `tools/tlk.py` writes, in Python) |
| GDA (G2DA), M2DA | `GdaFile.h`, `M2da.h`  | which part's row the game keeps (precedence) |
| AddIns.xml, offers.xml | `AddIns.h`  | |
| An install: sources, copies | `Install.h`  | precedence |
| Conversations (CONV) | `Conversation.h` | |
| Saves (`.das`, `.das.met`) | `SaveMeta.h` (the metadata); `.das` is GFF4 | |
| Rooms, models, lightmaps (`.rml`, `.mmh`, `.msh`) | `Level.h`, `LightProbes.h` | |
| Textures (DDS) | `Dds.h`: a picture of the chain | |
| Flash movies (GFx) | `GfxMovie.h`, `Inflate.h` | |
| Material objects (`.mao`) | none (XML; read as text) | |
| Script sources | in Python (`tools/sqlbak.py`, `toolset_scripts.py`: the toolset's SQL Server backup, `dev/TOOLSET.md`) | a C++ reader, or ghilana reading what the Python extracted |

**DAOLib #106's steps, and the tools that wait on each:**

| Step | What | State | Ghilana's tools that need it |
|---|---|---|---|
| 1 | `daolib_core`, 64-bit | master | all |
| 2 | the duplicate readers moved in | master | `resources`, `resource`, `extract` |
| 4, part | GFF3, GDA, M2DA and its merge rule, AddIns.xml, an install's copies and the game's order of them | master | `table`, `query`, `conflicts`, `environment` |
| 3 | writers, round trip over the install | not started | every authoring tool |
| 4, rest | strrefs across add-ins; the order of files within a source | not started | `check`; `table`'s unsettled rows |
| 5 | typed classes from the install's schemas | not started | `schema`, `set_field` |

## The game on disk

**Finding the game** (the default for `--game`; `game_launch` in the bridge finds the GOG install already):

- GOG: `C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins` on this machine; the registry's GOG key in general.
- Steam: `<library>\steamapps\common\Dragon Age Ultimate Edition` (here `E:\SteamLibrary\...`), found through Steam's `libraryfolders.vdf`.
- EA: `C:\Program Files\EA Games\Dragon Age`.
- All three are 1.05.13263.0 with the same data; only the exe differs (DAOLib's `dev/BUILDS.md`).

**Where resources are**, GOG install, counted 2026-10-02 and 2026-10-12:

- `packages\core\data\*.erf`: the core's archives (`2da.erf` holds 749 data tables, `designercreatures.erf` the templates, `modelmeshdata.erf` is 425 MiB, `shaders.erf`, ...). `packages\core\override\` and `packages\core\data\talktables\*.tlk` (1,501 talk tables, all version 0.2).
- `modules\single player\...`: the campaign's own archives, and nearly 10,000 `.rim` files, a level's rooms and an area's resources among them.
- `Addins\<name>\`: the DLC and promotional add-ins the Ultimate Edition installs (`dao_prc_cp_*`, `dao_prc_drk`, `dao_ep1_promo_*`, `PC_GAMER_TALE`, Awakening...); their `.erf` files are ERF V2.2, 377 of them, enciphered but for Awakening's. `Offers\` beside it.
- In Documents (`C:\Users\tianx\OneDrive\Documents\BioWare\Dragon Age` here): `Settings\AddIns.xml` and `offers.xml` (which add-ins are installed and enabled), `AddIns\` (add-ins a player installed from a `.dazip`), `packages\core\override\` (a player's loose overrides), `Settings\DAModmanager_Override.xml` (the community mod manager's).
- DAOLib's plugins: `bin_ship\daolib\plugins\<name>\override\`, which DAOLib makes a source of the game's resources like `packages\core\override` (DAOLib #43).
- Kinds by count in the install (2026-10-12): 10,009 `.rim`, 9,052 `.fsb` and 9,020 `.fev` (FMOD audio, out of scope), 2,049 `.mao`, 1,501 `.tlk`, 643 `.erf`, 555 `.dds`, 216 `.arl`.

**Precedence, read.** Which copy of a resource the game uses is the resource manager's in the exe to say. lib-agent read it there and saw it in play (2026-10-12, DAOLib's `dev/FILES.md`): of the copies it has, the game takes the one of the lowest priority, then a loose file before a RIM before an ERF, then the later name. The priorities run from Documents' override (0) through the install's (1), DAOLib plugins' (2), the add-ins (from 9000 down, the order of AddIns.xml's `Priority`, lower first), the campaign (50000s), the core's patches (100000) and its data, to the offers behind everything. `install::Index` gives every copy in that order, and ghilana's answers name the copy the game takes. Still to check: an Eluvian tool over DAOLib's `dao::resources::SourceOf`, to compare ghilana's answer with the running game's for any file.

## The index

`query`, `references` and `conflicts` need every resource of every archive known at once: about 10,000 archives' directories, and for `query` and `references` the fields of the files of the types asked about.

- **Directories** are cheap (a header and 72 or more bytes an entry) and are read on the first call that needs them, then kept in memory. Measured 2026-10-12 on the GOG install: 10,612 archives, 539,436 copies of 235,278 resources, read in 4.2 to 4.8 s by a release build (DAOLib's `install::Index`, sixteen threads, the system's file cache warm), 13.5 s by a debug build. No cache on disk for them: the first call says it built the index and how long it took.
- **Contents** (fields for `query`, links for `references`) are read per type when first asked and kept in the cache folder, each archive's part keyed by its path, size and modification time, so a changed archive is read again and the rest not.
- A call that has to build the index says so in its answer (how long it took, what was read), and a later call is fast. A build that would take long answers with progress, not silence.
- The index is a cache: deleting the folder loses nothing.

## The tools

Each tool is one concept, with a description that says what it answers and when to use it, as Eluvian's are (`dev/DESIGN.md`, Principles). Common conventions:

- **A copy** of a resource is named by where it is: `{"source": "core" | "campaign" | "addin:<name>" | "override:<folder>" | "plugin:<name>", "archive": "<path relative to the game or Documents folder>", "name": "<file.ext>"}`. A tool given a bare name takes the copy that wins, and says which copy that was.
- **Unreadable is said, never skipped.** A resource in an enciphered archive is listed with `"readable": false` and the reason, and the answer points to Eluvian's in-game read.
- **Large answers are paged** (`limit`, `offset`, the total), and a table's columns can be chosen.
- **Text** is in the `--language` unless the call says another.

### First version

| Tool | What it answers | Evidence it is needed |
|---|---|---|
| `environment` | the install found and its build, the Documents folder, the instance, the language; the add-ins installed and enabled, the override folders and what each holds; DAOLib's and Eluvian's versions if installed; what ghilana cannot read (the enciphered add-ins) | every other answer depends on it; houseCARL's `load_order_status` |
| `resources` | resources by name or part of one, by type, with every copy | Eluvian's testers guessed "ogre", "hurlock" and "bandit" for `stage_spawn` and found none (`src/core/Erf.h`); moves here from the game |
| `resource` | one resource read: GFF4 and GFF3 as JSON with field names, a GDA as rows with column names, a talk table's string, `.mao` as its XML; the winning copy or a named one; a depth and item limit for big files | DAOLib's `tools/gff4.py --dump`, used across the repos |
| `conflicts` | for a resource: every copy, and the winner once precedence is read; or every resource more than one source has, filtered by source (one mod's conflicts) | houseCARL's conflict tree |
| `table` | a data table merged as the game merges M2DA files: rows by id or column value, each row with the source it comes from and what it overrides | DAOLib's `tools/gda.py`; DAOTurnBased's `reticles.py` read GDA again by hand |
| `strings` | string references to text, and text to string references (search, in a language), across the core's and the add-ins' talk tables | DAOLib's `tools/tlk.py`; finding a name's strref by its text has no tool |
| `extract` | resources written out to a folder, by name or pattern | DAOElgaraShaders' and DAOTurnBased's scripts all begin by extracting |
| `texture` | a DDS as a PNG in the answer, for the agent to look at: one mip level, one channel, or the alpha as grey | ElgaraShaders' material review sheets, DAOTurnBased's reticles; the agent can read images |
| `texture_export`, `texture_import` | a texture out as a PNG for an image editor and back as the game's DDS, its halvings and its farther copies made (#135) | a mod's retexture without the toolset |
| `query` | every resource of a type whose fields match, across the install: `{"type": "utc", "where": {"Appearance_Type": 80}}`; group-by and counts; the fields to answer with | DAOTurnBased's `portraits.py`, which read every creature template's `Appearance_Type` by hand; houseCARL's most used tool (`cross_plugin_query`) |
| `references` | what uses X (a resource, a string reference, a table row, a script) and what X uses | DAOTurnBased's `reticles.py` followed a visual effect's row to its models, their materials and their textures by hand; Mutagen's LinkCache |

### Then: understanding

| Tool | What it answers | Evidence |
|---|---|---|
| `schema` | a file type's fields: label, name, type, list or struct, from the scan of the install (#106 step 5) | houseCARL's `mutagen-reference` skill |
| `diff` | two copies of a resource field by field (a mod's against vanilla, two mods') | houseCARL's `diff_record` |
| `dialogue` | a conversation as a tree: speakers, lines, replies, their text, conditions and the scripts they run; dangling links and lines no path reaches | houseCARL's `validate_dialogue`; DAOLib's `Conversation.h` reads it |
| `scripts` | the toolset's `.nss` sources: one read, all 2,101 searched | DAOLib and DAOTurnBased read `rules_core.nss` and its kin constantly; DAOLib's `tools/toolset_scripts.py` extracts them to `toolset\extract\scripts\` |
| `save` | a save read without the game: the party, their inventories, plot flags, the area | a test's end state checked without a load; DAOLib's `SaveMeta.h`, `dev/SAVES.md` |

### Then: authoring (needs #106 step 3)

All of it into a mod folder of the user's naming, laid out as an override folder, or packed as a `.dazip`. Nothing in the game's own folders is written.

| Tool | What it does |
|---|---|
| `mod_create` | a new mod folder, and later `mod_package`: the folder packed as a `.dazip` with its manifest |
| `set_field` | a copy of a resource with fields changed, written into the mod (DAOTurnBased's `reticles.py` renamed strings inside GFF4 files in place, for want of a writer) |
| `table_rows` | rows added to or overriding a data table in the mod's own M2DA table, their ids checked against every table and add-in that has the same table |
| `strings_add` | string references added in the mod's own talk table |
| `check` | what is wrong in a mod or the whole install: M2DA ids that collide, references to resources nothing has, string references no talk table has, an override another copy hides; houseCARL's `check_errors` |

### Later: assets

`model` and `model_export` are done (#132, above); and `model_import` the way back (#132, above). `level` (a level's rooms, their models, lightmaps and probes, out of `Level.h`), `material` (a `.mao`'s shader and textures, with ElgaraShaders' classification), `movie` (a GFx movie's ExternalInterface calls and fscommands, out of `GfxMovie.h`; DAOLib's `dev/GUI.md`).

### Not ghilana's

- Anything of the running game: Eluvian's. Eluvian gains one tool for ghilana's sake, a resource read through the engine's resource manager, as ElgaraShaders' `src/ResourceDump.cpp` does behind a file it watches: for the enciphered add-ins until DAOLib deciphers them, and to check ghilana's winners.
- Audio: FMOD's `.fsb` and `.fev`.
- Compiling scripts, until it is known whether the toolset has a compiler that runs from a command line (houseCARL compiles Papyrus; open question below).

## Testing

- **ctest**: `src/ghilana`'s tools over files built in the test (a GFF4 or GDA or ERF made in memory, as DAOLib's `tests/gff4_builder.h` makes one), and the stdio transport over strings.
- **Over the install**, skipped where there is none: a query whose answer is known (the appearance rows DAOTurnBased's `portraits.py` lists for its templates), a string reference's text, a table row against `tools/gda.py`'s answer.
- **Against the game**: a resource's winner from `conflicts` against Eluvian's in-game read of it, for resources with several copies (an add-in's override of a core table, a plugin's override). Written down in `dev/TESTING.md` as the other tools' checks are.
- **Mutants**: the readers', the merge's and the order's are DAOLib's (on its master, each caught); the stdio transport's are this repo's core suite's (4); `src/ghilana`'s are `tools/mutants.py --suite ghilana`'s (20: the copy the game takes, a table's order, shadowed copies and dropped rows, what `conflicts` and `query` count, the folders `extract` writes to, the JSON and text; each caught, 2026-10-13), built with the ghilana preset against the tree's own `extern/DAOLib`.

## Docs and packaging

- `dev/DESIGN.md` says which server each tool is in and why; `dev/PLAYBOOK.md` how an agent uses ghilana; `README.md` how to add it. The docs site lists both servers, ghilana's from `ghilana --describe` as Eluvian's are from `eluvian_describe`.
- **The docs site** has ghilana as a section of its own after Eluvian's (a Just the Docs collection, `docs/_ghilana`): its front page and guide by hand, its tools' pages made by `tools/build_docs.py` from `ghilana.exe --describe` (`docs/_data/ghilana.json`, kept current by ctest's `docs.reference_current`).
- The release zip holds `ghilana.exe` (and its PDB), outside the game folder's layout: it is not the game's.
- **Later, a Claude Code plugin in this repo**, as houseCARL is one (`C:\modding\houseCARL\.claude-plugin\marketplace.json`, `housecarl\.claude-plugin\plugin.json`, `housecarl\.mcp.json`):
  - a marketplace at the repo's root, and a plugin that registers both servers, with the game folder asked through `userConfig`;
  - skills: the game's files and where each kind is, M2DA authoring, conversations and talk tables, the toolset's scripts, Eluvian's playbook, writing a DAOLib plugin;
  - `ghilana.exe` is a build output and a marketplace installs from git, so the plugin's server command is a small launcher that fetches the release's exe for the plugin's version into `${CLAUDE_PLUGIN_DATA}` and checks its hash;
  - the in-game half (DAOLib's loader, Eluvian's DLL) still goes into `bin_ship`, which for GOG is under Program Files.

## Order of work

1. Done: **DAOLib's `gh-106-core-target` merged** (its merge gates are DAOLib's `CLAUDE.md`'s), `extern/DAOLib` moved to it.
2. Done: **The skeleton**: the `ghilana` preset and target, the stdio transport, `--call`, `--describe`, and `environment`.
3. Done: **DAOLib #106 step 2** (the archive readers in core; this repo's `src/core/Erf.cpp` gone), then `resources`, `resource`, `extract`, `texture`.
4. Done: **`strings` and `table`** (GDA in C++, the M2DA merge), and the index; then `query` and `references`. This is the first version.
5. Done: **Precedence read and checked** (#106 step 4, lib-agent), then `conflicts` calls winners.
6. `schema`, `diff`, `dialogue`, `scripts`, `save`.
7. **Writers** (#106 step 3), then the authoring tools and `check`.
8. Assets; the Claude Code plugin.

## Open questions

- **Precedence**: the order of the sources, and whether an add-in's own order (`AddIns.xml`'s priority?) matters. Read in the exe, checked in play.
- **The add-ins' password**: where the game gets it, and whether ghilana can decipher ERF V2.2 on its own.
- **The index's cost**: every directory, 4 to 5 s warm in release (above); a type's contents, 7,235 templates in about 1.5 s. A cold disk not measured. Whether contents want a cache on disk is open.
- **A script compiler**: whether the toolset ships one that runs from a command line.
- **GFF V3.2**: how many file types use it besides creature templates.
- **`.dazip` installs**: whether ghilana reads what DAUpdater installed (`Documents\...\AddIns\`) as the game does, manifests and all.

## References

- Issues: this repo's #128, DAOLib's #106.
- houseCARL: `C:\modding\houseCARL\housecarl\README.md` (its tools and skills), `skills\`.
- Mutagen: `C:\modding\Mutagen` (`docs\`, `Mutagen.Bethesda.Core`, the `*.Generator` projects).
- xoreos: `C:\modding\xoreos\src\aurora` (`erffile.cpp`, `gff3file.cpp`, `gff4file.cpp`, `gdafile.cpp`, `talktable_gff.cpp`, `gff4fields.h`).
- DAOLib: `dev/SAVES.md`, `dev/TOOLSET.md`, `dev/GUI.md`, `dev/BUILDS.md`; `tools/gff4.py`, `gda.py`, `tlk.py`, `toolset_scripts.py`.
- The scripts ghilana replaces: DAOTurnBased `tools/portraits.py`, `tools/reticles.py`; DAOElgaraShaders `tools/material_table.py`, `tint_level.py`, `check_relit.py`, `src/LevelFiles.h`, `src/ResourceDump.cpp`.
