---
title: Getting started
permalink: /getting-started/
nav_order: 2
---

# Getting started

Eluvian runs inside Dragon Age: Origins and lets an AI agent, or a script, read the game and give it orders. Five steps take you from nothing to a first test.

## 1. Install

Build Eluvian and copy it into the game, with the game closed:

```powershell
.\tools\build.ps1 -Preset release -Test
.\tools\deploy.ps1
```

The [README](https://github.com/Whocares66-Dev/DAOEluvianMCP#building) lists what the build needs.

## 2. Connect your agent

The bridge is an MCP server that stays connected while the game starts and stops:

```sh
claude mcp add eluvian -- python C:\project\DAOEluvianMCP\tools\eluvian_bridge.py
```

## 3. Start the game

```json
game_launch {"owner": "me", "load": {"character": "Lyna", "kind": "manual", "number": 1}}
```

This starts the game, loads the save and waits until Eluvian answers. `game_status` tells you whether the game is running, and `game_saves` lists the saves.

## 4. Look around

| Call | Returns |
|---|---|
| `state` | The game's mode, the area, the party, and any message box on screen |
| `creatures {"side": "hostile"}` | The creatures around, nearest first, each with its id |
| `creature {"id": "0x7d9"}` | One creature in full: health, effects, abilities, what it is doing |

Everything is addressed by id, such as `"0x7d9"`, never by name.

## 5. Act, then wait

Orders return a `cursor`. Wait for what happens next from that cursor, instead of sleeping:

```json
order {"action": "attack", "target": "0x7d9"}
wait_for {"topic": "eluvian.command.ended", "since": 1234, "match": {"id": "0x82"}}
```

`wait_for` returns the event once it happens, or says why nothing came (for example, the game is paused).

## Next

- [Tools]({{ '/tools/' | relative_url }}): every tool and its arguments.
- [Events]({{ '/events/' | relative_url }}): what you can wait for.
