---
title: Ghilana
permalink: /ghilana/
nav_order: 1
---

# Ghilana

Ghilana lets AI agents and scripts read Dragon Age: Origins' files straight from disk. The game doesn't need to be running. It is an [MCP](https://modelcontextprotocol.io) server, `ghilana.exe`, and it lives alongside Eluvian: Eluvian talks to the running game, and Ghilana reads the files on disk.

It reads archives, data tables, talk tables, templates, conversations and textures. It covers every place the game loads them from: the core game, the campaign, add-ins, the override folders and DAOLib plugins. When several copies of a file exist, Ghilana tells you which one the game actually uses. It works this out with the same rules the game uses, which were read from the game's executable and checked in play.

- [Getting started]({{ '/ghilana/getting-started/' | relative_url }}): set it up and make your first call.
- [Tools]({{ '/ghilana/tools/' | relative_url }}): every tool and its arguments.

Ghilana never writes to the game's folders. Add-in archives are encrypted with a key that only the game has. Ghilana can list what's inside them, but it can't read the contents. To read those, ask the running game through Eluvian.

The design and plans are in [dev/GHILANA.md](https://github.com/Whocares66-Dev/DAOEluvianMCP/blob/main/dev/GHILANA.md). The file formats are parsed by [DAOLib](https://github.com/Whocares66-Dev/DAOLib)'s core. DAOLib's [dev/FILES.md](https://github.com/Whocares66-Dev/DAOLib/blob/master/dev/FILES.md) documents the order the game loads files in.
