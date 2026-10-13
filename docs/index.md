---
title: Eluvian
permalink: /
nav_order: 1
---

# Eluvian

A test bench inside Dragon Age: Origins for AI agents and scripts. Eluvian is a [DAOLib](https://github.com/Whocares66-Dev/DAOLib) plugin that serves the running game over [MCP](https://modelcontextprotocol.io) and plain HTTP, so an agent can read the game, give it orders and wait for what happens, with no screen at all.

- [Getting started]({{ '/getting-started/' | relative_url }}): from nothing to a first test.
- [Tools]({{ '/tools/' | relative_url }}): every tool and its arguments.
- [Events]({{ '/events/' | relative_url }}): what an agent can wait for.
- [C interface]({{ '/c-interface/' | relative_url }}): how another plugin adds tools and events of its own.

[Ghilana]({{ '/ghilana/' | relative_url }}) is a second MCP server from the same repository. It reads the game's files from disk, so it works whether or not the game is running.

Each release is a zip to extract into the game's folder, on [GitHub](https://github.com/Whocares66-Dev/DAOEluvianMCP/releases), with what it brought in the [changelog](https://github.com/Whocares66-Dev/DAOEluvianMCP/blob/main/CHANGELOG.md). Building, deploying and the project's layout are in the [README](https://github.com/Whocares66-Dev/DAOEluvianMCP#readme).
