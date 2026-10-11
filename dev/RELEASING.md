# Releasing

A release is a tag, `v<version>`, and on GitHub the zip Eluvian is installed from: `DAOEluvianMCP-<version>.zip`, the plugin's folder laid out as the game's (`tools\package.ps1`). The documentation site builds that version's pages from the tag. The process is DAOLib's (`extern\DAOLib\dev\RELEASING.md`) and DAOAddressLibrary's. Written 2026-10-10, for 0.1.0 (#115).

## The version

`project(DAOEluvianMCP VERSION x.y.z)` in `CMakeLists.txt` is where it is set. `health`, MCP's `serverInfo`, `eluvian.log` and the zip's name take it from there. Two files repeat it, each held to it by a test: `docs\_data\eluvian.json` (`python tools\build_docs.py --update`, ctest's `docs.reference_current`) and `VERSION` in `tools\eluvian_bridge.py` (`test_bridge.py`). So `release.ps1` does not bump, as its siblings' can: the version is set on the work branch. It said 0.2.0 from 2026-09-30 (`17107df`) until #115, with nothing released under that number; the runs of then in `dev/TESTING.md` name it.

What a number says is what an agent's prompts and scripts, and a plugin that registers tools, can count on:

- **patch**: fixes, and answers that say more, with every tool, argument, field and topic there was.
- **minor**: something to call new: a tool, an argument, a topic.
- **major**: something a caller must change for: a tool, argument or field removed or renamed, or `ELUVIAN_INTERFACE_VERSION` changed (`src/api/EluvianAPI.h`), which any change to the C interface's layout does, an addition too, and after which a plugin built for the old one finds no Eluvian until it is built again. Below 1.0 such a change may come in a minor, and its notes say so first.

What loader a release needs is not the version's to say but DAOLib's `kPluginApiVersion`, below.

## DAOLib

A release holds `extern\DAOLib` at one of DAOLib's own release tags, and `release.ps1` refuses at any other commit. A game has a DAOLib release's loader and address files, and nothing newer: a plugin built on a later commit may ask the address library for an ID those files lack, or be of a plugin API that loader refuses. Held at the tag, Eluvian needs "DAOLib x.y.z or later", which the changelog's section and the zip's `VERSION.txt` name. Later holds while `kPluginApiVersion` is the same (`extern\DAOLib\src\dao\Plugin.h`), since a plugin carries its own copy of the library and the address files only gain IDs.

So a release that needs something DAOLib's master has and its last release lacks waits for DAOLib's next release (lib-agent's). And when DAOLib's plugin API changes, the Eluvian release built on it says in its notes that the game needs that DAOLib, and no earlier one.

## What the zip holds, and what it must not

All of it is the plugin's folder, `bin_ship\daolib\plugins\eluvian\`, which is the same in a build tree, in the game after `deploy.ps1` and in the zip:

- `eluvian.dll` and `eluvian.pdb`, the release build's: what the game plays here (#76). The PDB is 40 MB, most of the zip's 10 MB, and is what lets a crash log name Eluvian's frames with file and line. The link is not incremental, with `/OPT:REF` and `/OPT:ICF`, and starts a PDB of its own each time (`CMakeLists.txt`): the linker writes into a PDB that is there and leaves earlier links' records in it, which doubled it in one relink.
- `eluvian_bridge.py`, what an agent's client starts. From a game's plugin folder it takes that game as its own (`default_game`), so no path is written anywhere.
- `README.md`, the repository's own, with `LICENSE.txt` and `VERSION.txt`: the release, the DAOLib it was built on with its plugin API, and the C interface.
- **No loader and no address files.** Those are DAOLib's zip, and a second copy here would put older address files over newer ones.
- **Nothing that says who built it.** A PDB holds the paths of its sources (`C:\project\DAOEluvianMCP`, `C:\vcpkg`, Visual Studio's), and the linker adds the folder of its temporary files, which is under the user's name. `tools\build.ps1` gives a build a temporary folder in its own tree, and `package.ps1` refuses a DLL or PDB that holds the path of the user's folder. A build made any other way (the IDE's, `cmake --build` by hand) is not one to package.
- Not `src/api/EluvianAPI.h`: a plugin's author takes it from the tag.

## The site's versions

Each release is selectable on the documentation site (the user, 2026-10-10: "the tags should produce docs. we want to select release versions from docs"). Three things make it so:

- `.github\workflows\docs.yml` runs on a push of a `v*` tag as on one of `main`.
- `tools\build_docs.py` builds the working tree at the site's root and each `vX.Y.Z` tag, from that tag's own `docs\` and header, into `X.Y.Z\`, with the list of versions the selector shows (`docs\_includes\components\aux_nav.html`).
- The repository's `github-pages` environment lets `v*` tags deploy, beside the branch `main`. Without that rule the tag's run builds the site and its deploy job is refused, as DAOLib's `v0.1.0` was at first. It is a setting on GitHub and not a file here: `gh api -X POST repos/Whocares66-Dev/DAOEluvianMCP/environments/github-pages/deployment-branch-policies -f name='v*' -f type=tag`, by the user or with their word, since it changes what may deploy. `release.ps1` reads the rules and refuses before it tags when the rule is not there.

## Cutting one

1. On a work branch (`gh-<N>-...`): set the version if it changes, with `python tools\build_docs.py --update` and the bridge's `VERSION`, and add `## x.y.z` at the top of `CHANGELOG.md`: about five entries, the most important first, in the words of someone driving a test or writing a plugin. The section is the GitHub release's notes, word for word, so no link in it is relative. Move the submodule to DAOLib's tag if it is not at one.
2. Run "Before a merge to master" (`CLAUDE.md`), then `.\tools\package.ps1`, and read the zip: its files and its `VERSION.txt`.
3. See the release build in the game, from the zip: its `eluvian` folder extracted over the game's, a start, a load of a save, and `python tools\smoke.py` with the bridge it runs being the one in the game's plugin folder at least once (`python "<game>\bin_ship\daolib\plugins\eluvian\eluvian_bridge.py" --call game_status`). `eluvian.log` says the version and `loader.log` "loaded". One run is with the loader of the DAOLib release the zip names, out of that release's own zip from GitHub, and the loader that was in the game is put back after: what a game has here is otherwise whatever build the turn-based mod deployed. With no game of the install running, as any deploy. `dev/TESTING.md` has 0.1.0's.
4. Publishing waits for the user's word: a request to prepare a release is not one to publish it.
5. Squash into `main`, then from the main tree `.\tools\release.ps1 -DryRun` and `.\tools\release.ps1` (`-Jobs 4` while someone plays). It builds the release preset and runs its tests, writes the zip, makes the annotated tag, pushes `main` and the tag, and creates the release with the zip attached, a prerelease below 1.0.
6. Check the release's tag, notes, prerelease flag and one zip, and the zip's hash against `build\package\`. Check that the tag's run of the Documentation workflow passed and the site's version selector has the version. A tag is never moved: a correction is the next patch.

The DLL in the zip is `release.ps1`'s own link, made in the main tree, and the one seen in step 3 was the work branch's: the same sources, two links. DAOLib's 0.1.0 found its two differing only in the link's stamps (`extern\DAOLib\dev\RELEASING.md`).

If the build, the tests or the zip fail, nothing was tagged or pushed. If `gh release create` fails after the push, run it alone on the tag that is there: `gh release create vX.Y.Z --repo Whocares66-Dev/DAOEluvianMCP --verify-tag --title "DAOEluvianMCP X.Y.Z" --notes-file build\release-notes-X.Y.Z.md build\package\DAOEluvianMCP-X.Y.Z.zip` (`--prerelease` below 1.0).
