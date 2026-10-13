---
title: Getting started
permalink: /ghilana/getting-started/
nav_order: 2
---

# Getting started

Ghilana reads Dragon Age: Origins' files from disk and answers questions about them from an AI agent or a script. The game doesn't need to be running. It takes four steps to get your first answer.

## 1. Build it

Ghilana is a 64-bit program, `ghilana.exe`. There's no release that includes it yet, so build it from a clone of the repository (see the [README](https://github.com/Whocares66-Dev/DAOEluvianMCP#building)):

```powershell
git submodule update --init --recursive
.\tools\build.ps1 -Preset ghilana-release
```

The program ends up at `build\ghilana-release\ghilana.exe`.

## 2. Connect your agent

```sh
claude mcp add ghilana -- "C:\path\to\ghilana.exe"
```

Ghilana finds the game for you. It checks for a GOG install first, then Steam, then EA, and it finds your Documents folder through Windows. You can override any of this with arguments. Some arguments also describe what the game is doing, which changes which copies of a file it uses:

| Argument | What it sets |
|---|---|
| `--game DIR` | The install folder (the one that contains `bin_ship`) |
| `--documents DIR` | `Documents\BioWare\Dragon Age`, if it isn't in the usual place |
| `--instance NAME` | The Documents folder of a named instance (a second copy of the game run by DAOLib) |
| `--campaign UID` | The loaded campaign: `Single Player` (Origins, the default), `DAO_PRC_EP_1` (Awakening), or `none` (the main menu) |
| `--language CODE` | The language for talk tables. The default is `en-us` |
| `--textures high\|medium` | The game's texture quality |

## 3. See what's installed

```json
environment {}
```

This shows the install, the game's build, every place files are loaded from and what each one contains, the add-ins enabled in AddIns.xml, and anything that can't be read. The first call reads the index of every archive (about ten thousand of them), which takes around five seconds. After that, calls are fast.

## 4. Ask about a file

| Call | What you get |
|---|---|
| `resources {"match": "ogre", "type": "utc"}` | Creature templates with "ogre" in the name, with every copy of each |
| `resource {"name": "abi_base.gda"}` | The contents of one file: a table's rows, or a template's fields |
| `conflicts {"name": "abi_base.gda"}` | Which copy the game uses, and every copy in the order the game checks them |
| `table {"table": 1, "id": 10000}` | One row of the abilities table, merged the way the game merges its tables |
| `strings {"id": 379970}` | The game's text for a string ID. Use `{"text": "Flurry"}` to look up an ID from its text |
| `query {"type": "utc", "where": {"Appearance_Type": 47}}` | Every creature template with a given appearance |

Whenever an answer lists copies of a file, they're in the order the game checks them, and the answer says which copy the game uses. To call a tool from a shell, run something like `ghilana.exe --call conflicts '{"name": "abi_base.gda"}'`. It prints the answer.

## Next

- [Tools]({{ '/ghilana/tools/' | relative_url }}): every tool and its arguments.
- [Eluvian]({{ '/' | relative_url }}): the running game, for things the files can't tell you.
