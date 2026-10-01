"""Eluvian's stdio bridge: an MCP server an agent keeps across game restarts.

Eluvian serves MCP from inside DAOrigins.exe, so a direct connection dies
with the game, and a client that started before the game marks it failed
until someone reconnects by hand. This bridge is the stable end: the agent's
client starts it over stdio, and it forwards each tool call to the game's
REST API (POST /api/tool/<name>) while the game runs.

It adds what only a process outside the game can do:
  game_status   is the game running, and what Eluvian's `health` says
  game_launch   start DAOrigins.exe (never a second one), wait until it serves
  game_quit     close the game

The game's tools are listed from its GET /api/tools, and kept in
eluvian.tools.json beside its logs, so they are listed even while the game is
down; calling one then says how to start it.

    claude mcp add eluvian -- python C:\\project\\DAOEluvianMCP\\tools\\eluvian_bridge.py

Standard library only. tools/test_bridge.py tests it against a stand-in for
the game.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from pathlib import Path

PROTOCOL_VERSIONS = ("2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05")
DEFAULT_PORT = 8930
DEFAULT_GAME = Path(
    r"C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins\bin_ship\DAOrigins.exe"
)
VERSION = "0.1.0"

INSTRUCTIONS = (
    "This bridge keeps Eluvian, the test bench inside Dragon Age: Origins, connected while the game "
    "restarts. game_status says whether the game runs; game_launch starts it and waits until Eluvian "
    "serves. The other tools are the game's own (health, inspect, order, wait_for, ...): see their "
    'descriptions. Creatures are ids like "0x7d9".'
)

NOT_RUNNING = (
    "the game is not running, or Eluvian is not loaded in it: game_status says which, and "
    "game_launch starts it"
)


def documents_folder() -> Path:
    """The user's Documents, where the game writes, wherever it really is (OneDrive here)."""
    if sys.platform == "win32":
        folder_documents = ctypes.c_char_p(
            bytes.fromhex(
                "D0 9A D3 FD 8F 23 AF 46 AD B4 6C 85 48 03 69 C7".replace(" ", "")
            )
        )
        path = ctypes.c_wchar_p()
        if (
            ctypes.windll.shell32.SHGetKnownFolderPath(
                folder_documents, 0, None, ctypes.byref(path)
            )
            == 0
        ):
            found = Path(path.value)
            ctypes.windll.ole32.CoTaskMemFree(path)
            return found
    return Path.home() / "Documents"


def log_folder() -> Path:
    return documents_folder() / "BioWare" / "Dragon Age" / "daolib"


class Unreachable(Exception):
    """Nothing answered at the game's port."""


class StillWaiting(Exception):
    """The game took the request and had not answered when the bridge stopped waiting: the call
    may be running there yet, which is not the game being down."""


# How long the bridge waits for a tool's answer. A scenario may run its ten minutes, and a
# wait_for its own timeout; anything else answers within the frame queue's few seconds.
SCENARIO_SECONDS = 10 * 60
MARGIN_SECONDS = 30


def call_budget(name: str, arguments: object) -> float:
    """Seconds to wait for a tool's answer: longer than the tool itself may take, so the bridge
    never gives up on a call the game is still running (a review found a two-minute scenario
    given 60 s, then reported as the game not running while its later steps went on)."""
    if name == "scenario":
        return SCENARIO_SECONDS + MARGIN_SECONDS
    wait_ms = arguments.get("timeoutMs", 0) if isinstance(arguments, dict) else 0
    waited = wait_ms / 1000 if isinstance(wait_ms, (int, float)) and wait_ms > 0 else 0
    return 60 + waited


class Game:
    """The game's REST API, at the port its Eluvian wrote down beside the logs: eluvian.json,
    or eluvian.<pid>.json for a game that found the first port taken."""

    def __init__(
        self,
        folder: Path,
        port: int | None = None,
        running: Callable[[], list[int]] | None = None,
    ) -> None:
        self.folder = folder
        self.fixed_port = port
        self.running = running  # the pids of the games running, to tell a stale file by
        self.found: tuple[float, int] | None = None

    def read_runtime(self, name: str) -> dict | None:
        try:
            runtime = json.loads((self.folder / name).read_text(encoding="utf-8"))
            return runtime if isinstance(runtime.get("port"), int) else None
        except (OSError, ValueError, AttributeError):
            return None

    def find_port(self) -> int:
        """The port of a game that is running. eluvian.json's, if its game still runs (or
        there is no telling); else a running game's own file; else what eluvian.json says, or
        the default. A second game once wrote its port over the first's and went away
        (2026-09-30): the file named a game that was gone while the first still served."""
        named = self.read_runtime("eluvian.json")
        pids = set(self.running()) if self.running is not None else None
        if named and (pids is None or named.get("pid") in pids):
            return named["port"]
        for pid in sorted(pids or ()):
            own = self.read_runtime(f"eluvian.{pid}.json")
            if own:
                return own["port"]
        if named and pids:
            return DEFAULT_PORT  # a game runs, and it is not the one the file names
        return named["port"] if named else DEFAULT_PORT

    def port(self) -> int:
        if self.fixed_port is not None:
            return self.fixed_port
        now = time.monotonic()
        if self.found is None or now - self.found[0] > 5:
            self.found = (now, self.find_port())
        return self.found[1]

    def request(
        self, method: str, path: str, body: object = None, timeout: float = 10
    ) -> tuple[int, object]:
        url = f"http://127.0.0.1:{self.port()}{path}"
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(url, data=data, method=method)
        if data is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.status, json.loads(response.read() or b"null")
        except urllib.error.HTTPError as error:
            try:
                return error.code, json.loads(error.read() or b"null")
            except ValueError:
                return error.code, {"error": str(error)}
        except TimeoutError as error:
            raise StillWaiting(str(error)) from error
        except urllib.error.URLError as error:
            # urllib wraps a timeout on connecting or sending in a URLError; one on reading
            # the answer comes bare (above). Either way the game may have the request.
            if isinstance(error.reason, TimeoutError):
                raise StillWaiting(str(error)) from error
            raise Unreachable(str(error)) from error
        except ConnectionError as error:
            raise Unreachable(str(error)) from error


def image_path(pid: int) -> str | None:
    """The file a process runs, or None if Windows will not say."""
    if sys.platform != "win32":
        return None
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.OpenProcess(
        0x1000, False, pid
    )  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return None
    try:
        buffer = ctypes.create_unicode_buffer(32768)
        size = ctypes.c_ulong(len(buffer))
        if kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return buffer.value
        return None
    finally:
        kernel32.CloseHandle(handle)


def is_same_file(path: str | None, exe: Path) -> bool:
    return path is not None and os.path.normcase(
        os.path.normpath(path)
    ) == os.path.normcase(os.path.normpath(str(exe)))


class Processes:
    """DAOrigins.exe as Windows sees it: find, start, close."""

    def __init__(self, exe: Path) -> None:
        self.exe = exe

    def named(self) -> list[int]:
        """Every process with the game's file name, whichever install it is of."""
        out = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {self.exe.name}", "/FO", "CSV", "/NH"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout
        pids = []
        for line in out.splitlines():
            fields = [field.strip('"') for field in line.split('","')]
            if len(fields) > 1 and fields[1].isdigit():
                pids.append(int(fields[1]))
        return pids

    def running(self) -> list[int]:
        """This install's game only. A second install of the game has the same file name (the
        user installed EA's beside GOG's, 2026-10-01): a game of another install is not
        Eluvian's to attach to, count as its own, or close."""
        return [pid for pid in self.named() if is_same_file(image_path(pid), self.exe)]

    def others(self) -> list[dict]:
        """Games running from another install, or whose file could not be read."""
        found = []
        for pid in self.named():
            path = image_path(pid)
            if not is_same_file(path, self.exe):
                found.append({"pid": pid, "exe": path or "unknown"})
        return found

    def start(self, environment: dict[str, str]) -> int:
        flags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(
            subprocess, "CREATE_NEW_PROCESS_GROUP", 0
        )
        process = subprocess.Popen(
            [str(self.exe)],
            cwd=str(self.exe.parent),
            env={**os.environ, **environment},
            creationflags=flags,
            close_fds=True,
        )
        return process.pid

    def close(self, pid: int, force: bool) -> None:
        command = ["taskkill", "/PID", str(pid)] + (["/F"] if force else [])
        subprocess.run(command, capture_output=True, check=False)


SAVE_KINDS = (("Slot_", "manual"), ("AutoSave_", "auto"), ("QuickSave_", "quick"))


def list_saves(characters: Path, character: str | None = None) -> list[dict]:
    """The game's saves, newest first, from their folders:
    Characters\<character>\Saves\<Slot_|AutoSave_|QuickSave_><n>\<name>.das (dev/SAVES.md in
    DAOLib). The name is the save's, which the game makes the area's (CastleCousland)."""
    saves = []
    if not characters.is_dir():
        return saves
    for folder in sorted(characters.iterdir()):
        if not folder.is_dir() or (
            character and folder.name.lower() != character.lower()
        ):
            continue
        root = folder / "Saves"
        if not root.is_dir():
            continue
        for slot in root.iterdir():
            kind = next(
                (
                    (k, prefix)
                    for prefix, k in SAVE_KINDS
                    if slot.name.startswith(prefix)
                ),
                None,
            )
            if kind is None or not slot.name[len(kind[1]) :].isdigit():
                continue
            das = next(iter(sorted(slot.glob("*.das"))), None)
            if das is None:
                continue
            saved = das.stat().st_mtime
            saves.append(
                {
                    "character": folder.name,
                    "kind": kind[0],
                    "number": int(slot.name[len(kind[1]) :]),
                    "name": das.stem,
                    "saved": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(saved)),
                    "folder": str(slot),
                    "_mtime": saved,
                }
            )
    saves.sort(key=lambda save: save["_mtime"], reverse=True)
    for save in saves:
        del save["_mtime"]
    return saves


def text_result(value: object, is_error: bool = False) -> dict:
    text = value if isinstance(value, str) else json.dumps(value)
    result: dict = {"content": [{"type": "text", "text": text}], "isError": is_error}
    if isinstance(value, dict) and not is_error:
        result["structuredContent"] = value
    return result


BRIDGE_TOOLS = [
    {
        "name": "game_saves",
        "description": (
            "The game's saves on disk, newest first: character, kind (manual, auto, quick), number, "
            "the save's name (the area's, as the game names it), when it was saved, and its folder. "
            "Answers while the game is down, to choose what a test starts from."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "character": {"type": "string", "description": "Only this character's"}
            },
        },
        "annotations": {"readOnlyHint": True},
    },
    {
        "name": "game_status",
        "description": (
            "Whether Dragon Age: Origins is running (its process ids) and what Eluvian's health says "
            "inside it. Answers while the game is down."
        ),
        "inputSchema": {"type": "object"},
        "annotations": {"readOnlyHint": True},
    },
    {
        "name": "game_launch",
        "description": (
            "Start DAOrigins.exe, never a second one (a second game's logs and port get in the "
            "first's way), and wait until Eluvian serves at the main menu; answers its health. "
            "With `load` (a save as game_saves lists them: character, kind, number), it then "
            "loads that save without the menus and waits until the world is there, so a test "
            "starts where it means to. Deploy first: the game holds its plugins open while it runs."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "waitSeconds": {"type": "integer", "minimum": 0, "maximum": 600},
                "logLevel": {
                    "type": "string",
                    "enum": ["trace", "debug", "info", "warn", "error"],
                },
                "load": {
                    "type": "object",
                    "description": "The save to start in",
                    "properties": {
                        "character": {"type": "string"},
                        "kind": {"type": "string", "enum": ["manual", "auto", "quick"]},
                        "number": {"type": "integer"},
                    },
                },
            },
        },
    },
    {
        "name": "game_quit",
        "description": (
            "Close the game: asked to close as its window's close button asks, or with force=true "
            "ended at once. Unsaved progress is lost either way."
        ),
        "inputSchema": {"type": "object", "properties": {"force": {"type": "boolean"}}},
    },
]


class LaunchLock:
    """One launch at a time: among this bridge's request threads (a lock), and among the
    bridges on the machine (a named mutex, on Windows), so "never a second game" holds
    whoever asks."""

    NAME = "Local\\EluvianBridgeLaunch"
    WAIT_MS = 30_000

    def __init__(self) -> None:
        self.local = threading.Lock()
        self.handle: int | None = None

    def __enter__(self) -> LaunchLock:
        self.local.acquire()
        if sys.platform == "win32":
            kernel32 = ctypes.windll.kernel32
            kernel32.CreateMutexW.restype = ctypes.c_void_p
            self.handle = kernel32.CreateMutexW(None, False, self.NAME)
            if self.handle:
                # Abandoned by a bridge that died holding it counts as taken: ours now.
                kernel32.WaitForSingleObject(ctypes.c_void_p(self.handle), self.WAIT_MS)
        return self

    def __exit__(self, *exc: object) -> None:
        if self.handle:
            kernel32 = ctypes.windll.kernel32
            kernel32.ReleaseMutex(ctypes.c_void_p(self.handle))
            kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None
        self.local.release()


class Bridge:
    def __init__(
        self,
        game: Game,
        processes: Processes,
        cache: Path,
        send: Callable[[dict], None],
        sleep: Callable[[float], None] = time.sleep,
        characters: Path | None = None,
    ) -> None:
        self.game = game
        self.processes = processes
        self.cache = cache
        self.send = send
        self.sleep = sleep
        # How long a load may leave the game's mode unmoved before it counts as declined: from
        # the menu the mode changed within seconds, and the whole load took 14 s.
        self.load_patience = 20.0
        # The saves' root, beside the logs' folder: Documents\BioWare\Dragon Age\Characters.
        self.characters = (
            characters if characters is not None else game.folder.parent / "Characters"
        )
        self.revision: int | None = None
        self.lock = threading.Lock()
        self.launching = LaunchLock()

    # ---- the game's tools -------------------------------------------------

    def game_tools(self) -> list[dict]:
        """The game's tools: asked of the game if it is up, else the last list kept."""
        try:
            status, body = self.game.request("GET", "/api/tools", timeout=3)
            if status == 200 and isinstance(body, dict):
                tools = body.get("tools", [])
                self.note_revision(body.get("revision"))
                try:
                    self.cache.write_text(json.dumps(tools), encoding="utf-8")
                except OSError:
                    pass  # a cache not written is a list not kept, nothing worse
                return tools
        except (Unreachable, StillWaiting):
            pass
        try:
            return json.loads(self.cache.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return []

    def note_revision(self, revision: object) -> None:
        """Tells the client to list again when the game's tools changed (a plugin registered)."""
        with self.lock:
            changed = self.revision is not None and revision != self.revision
            self.revision = revision if isinstance(revision, int) else self.revision
        if changed:
            self.send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})

    def call_game(self, name: str, arguments: dict) -> dict:
        timeout = call_budget(name, arguments)
        try:
            status, body = self.game.request(
                "POST", f"/api/tool/{name}", arguments, timeout=timeout
            )
        except Unreachable:
            return text_result(f"{name}: {NOT_RUNNING}", is_error=True)
        except StillWaiting:
            return text_result(
                f"{name}: the bridge stopped waiting after {timeout:.0f} s, but the game took the "
                "call and may still be running it, and whatever comes after it. Do not repeat it "
                "blindly: `health` and `events` say what has happened.",
                is_error=True,
            )
        if status != 200:
            message = body.get("error") if isinstance(body, dict) else None
            return text_result(message or f"{name}: HTTP {status}", is_error=True)
        if isinstance(body, dict) and set(body) == {"data", "content"}:
            result = text_result(body["data"])
            result["content"].extend(body["content"])
            return result
        return text_result(body)

    # ---- the bridge's own tools -------------------------------------------

    def health(self) -> object:
        try:
            status, body = self.game.request("GET", "/api/health", timeout=3)
            return body if status == 200 else {"error": f"HTTP {status}"}
        except (Unreachable, StillWaiting):
            return None

    def game_status(self) -> dict:
        pids = self.processes.running()
        health = self.health()
        status = {
            "running": bool(pids),
            "pids": pids,
            "eluvian": health if health is not None else "not answering",
        }
        others = self.processes.others()
        if others:
            status["otherInstalls"] = others
        return status

    def game_launch(self, arguments: dict) -> dict:
        environment = {}
        if isinstance(arguments.get("logLevel"), str):
            environment["DAOLIB_LOG_LEVEL"] = arguments["logLevel"]
        # Looking for a game and starting one are a single step, under a lock: requests run
        # on threads of their own, and two launches at once each saw no game and each started
        # one (a review's finding). The lock is the machine's, so two bridges agree too.
        with self.launching:
            pids = self.processes.running()
            if pids:
                return text_result(
                    {"launched": False, "already": pids, **self.game_status()}
                )
            others = self.processes.others()
            if others:
                # Two installs share Documents\BioWare\Dragon Age: saves, settings, logs.
                return text_result(
                    "game_launch: a game from another install is running "
                    f"({others[0]['exe']}, pid {others[0]['pid']}), not {self.processes.exe}. "
                    "It is not this bridge's to close; close it yourself, then launch.",
                    is_error=True,
                )
            if not self.processes.exe.exists():
                return text_result(
                    f"game_launch: no {self.processes.exe}", is_error=True
                )
            pid = self.processes.start(environment)
        wait = arguments.get("waitSeconds", 180)
        deadline = time.monotonic() + (wait if isinstance(wait, (int, float)) else 180)
        health = None
        while time.monotonic() < deadline:
            health = self.health()
            # The game started here is the one that must answer, not another's listener.
            if (
                isinstance(health, dict)
                and health.get("state") == "running"
                and health.get("mode") != "none"
                and health.get("pid", pid) == pid
            ):
                break
            if not self.processes.running():
                return text_result(
                    {
                        "launched": True,
                        "pid": pid,
                        "error": "the game exited while starting; crash-*.log in "
                        f"{self.game.folder} may say why",
                    },
                    is_error=True,
                )
            self.sleep(1)
        self.game_tools()  # lists the game's tools now it serves, and says so if they changed
        self.send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
        answer = {"launched": True, "pid": pid, "health": health}
        save = arguments.get("load")
        if isinstance(save, dict):
            loaded = self.load_save(save, deadline)
            answer["loaded"] = loaded
            answer["health"] = self.health()
            if "error" in loaded:
                return text_result(json.dumps(answer), is_error=True)
        return text_result(answer)

    def load_save(self, save: dict, deadline: float) -> dict:
        """Asks the game for a save, as its `game` tool's load does, and waits for the world:
        from the main menu a load took 14 s in play (2026-10-01). The menu may not take a
        load in its first moments, so a refusal is asked again until the deadline."""
        wanted = {
            key: save[key] for key in ("character", "kind", "number") if key in save
        }
        refusal = "the game did not answer"
        while True:
            try:
                status, body = self.game.request(
                    "POST", "/api/tool/game", {"action": "load", **wanted}, timeout=30
                )
            except (Unreachable, StillWaiting) as error:
                status, body = 0, {"error": str(error)}
            if status == 200 and isinstance(body, dict) and body.get("asked"):
                break
            refusal = body.get("error", refusal) if isinstance(body, dict) else refusal
            if time.monotonic() >= deadline:
                return {"error": f"the load was not taken: {refusal}"}
            self.sleep(2)
        # The game answers "asked" and may still not load: a save that needs content no longer
        # registered stops at the game's own dialog (Force Load / Cancel), with no word to a
        # plugin (seen 2026-10-01, after another install rewrote AddIns.xml). So the wait is
        # in pieces, and a mode that has not moved in `load_patience` seconds is that.
        before = self.mode()
        asked_at = time.monotonic()
        while True:
            left = deadline - time.monotonic()
            piece_ms = int(max(1.0, min(left, 5)) * 1000)
            try:
                status, event = self.game.request(
                    "POST",
                    "/api/tool/wait_for",
                    {
                        "topic": "eluvian.game.loaded",
                        "since": body.get("cursor", 0),
                        "timeoutMs": piece_ms,
                    },
                    timeout=piece_ms / 1000 + MARGIN_SECONDS,
                )
            except (Unreachable, StillWaiting) as error:
                return {"error": f"asked, but the wait for the load failed: {error}"}
            if status == 200 and isinstance(event, dict) and event.get("found"):
                # The event comes while the mode still reads loading (seen 2026-10-01); the
                # world is there, and tools answer, a moment after.
                while self.mode() == "loading" and time.monotonic() < deadline:
                    self.sleep(0.5)
                return {"save": body.get("save"), "event": event.get("event")}
            mode = self.mode()
            waited = time.monotonic() - asked_at
            if mode == before and mode != "loading" and waited >= self.load_patience:
                return {
                    "error": f"the game was asked for the save and did nothing in {waited:.0f} s "
                    f'(mode still {mode}): it is likely showing a dialog, such as "This saved '
                    'game cannot be loaded. It requires the following content" when the save '
                    "needs add-ins that are not registered. `screenshot` shows it. Do not "
                    "Force Load and then save: the save would lose that content."
                }
            if time.monotonic() >= deadline:
                return {
                    "error": "the game was asked and no eluvian.game.loaded came in time "
                    f"(mode {mode}); it may still be loading: health says"
                }

    def mode(self) -> str | None:
        health = self.health()
        return health.get("mode") if isinstance(health, dict) else None

    def game_quit(self, arguments: dict) -> dict:
        pids = self.processes.running()
        if not pids:
            return text_result({"quit": False, "reason": "the game is not running"})
        force = bool(arguments.get("force", False))
        for pid in pids:
            self.processes.close(pid, force)
        for _ in range(30):
            if not self.processes.running():
                return text_result({"quit": True, "pids": pids})
            self.sleep(1)
        return text_result(
            {
                "quit": False,
                "pids": pids,
                "reason": "still running after 30 s; force=true ends it",
            },
            is_error=True,
        )

    # ---- JSON-RPC ---------------------------------------------------------

    def handle(self, message: object) -> dict | None:
        if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
            return {
                "jsonrpc": "2.0",
                "id": None,
                "error": {"code": -32600, "message": "Invalid Request"},
            }
        method = message.get("method")
        if "id" not in message or not isinstance(method, str):
            return None  # a notification, or a response to us
        request_id = message["id"]

        def reply(result: dict) -> dict:
            return {"jsonrpc": "2.0", "id": request_id, "result": result}

        def invalid(what: str) -> dict:
            return {
                "jsonrpc": "2.0",
                "id": request_id,
                "error": {"code": -32602, "message": f"Invalid params: {what}"},
            }

        # Checked before anything reads them: params that were a list took the request's
        # thread down with an AttributeError, and its id never got an answer (a review).
        params = message.get("params")
        if params is None:
            params = {}
        if not isinstance(params, dict):
            return invalid("`params` must be an object")

        if method == "initialize":
            asked = params.get("protocolVersion")
            return reply(
                {
                    "protocolVersion": asked
                    if asked in PROTOCOL_VERSIONS
                    else PROTOCOL_VERSIONS[0],
                    "capabilities": {"tools": {"listChanged": True}},
                    "serverInfo": {"name": "eluvian-bridge", "version": VERSION},
                    "instructions": INSTRUCTIONS,
                }
            )
        if method == "ping":
            return reply({})
        if method == "tools/list":
            names = {tool["name"] for tool in BRIDGE_TOOLS}
            game = [tool for tool in self.game_tools() if tool.get("name") not in names]
            return reply({"tools": BRIDGE_TOOLS + game})
        if method == "tools/call":
            name = params.get("name")
            arguments = params.get("arguments")
            if arguments is None:
                arguments = {}
            if not isinstance(name, str):
                return invalid("tools/call needs a tool `name`")
            if not isinstance(arguments, dict):
                return invalid("`arguments` must be an object")
            if name == "game_saves":
                character = arguments.get("character")
                saves = list_saves(
                    self.characters, character if isinstance(character, str) else None
                )
                return reply(text_result({"count": len(saves), "saves": saves}))
            if name == "game_status":
                return reply(text_result(self.game_status()))
            if name == "game_launch":
                return reply(self.game_launch(arguments))
            if name == "game_quit":
                return reply(self.game_quit(arguments))
            return reply(self.call_game(name, arguments))
        return {
            "jsonrpc": "2.0",
            "id": request_id,
            "error": {"code": -32601, "message": f"Method not found: {method}"},
        }


def serve(bridge: Bridge, lines, drain_seconds: float = 30) -> None:
    """Reads newline-delimited JSON-RPC from stdin; each request on a thread of its own, so a wait_for
    does not hold up a ping. At the end of the input, the requests still running get up to
    `drain_seconds` to answer: a script that writes its requests and closes stdin still gets its
    answers (found 2026-09-30, when only the first of four came back)."""
    running: list[threading.Thread] = []
    for line in lines:
        text = line.strip()
        if not text:
            continue
        try:
            message = json.loads(text)
        except ValueError:
            bridge.send(
                {
                    "jsonrpc": "2.0",
                    "id": None,
                    "error": {"code": -32700, "message": "Parse error"},
                }
            )
            continue

        def run(message=message) -> None:
            # Whatever goes wrong, a request with an id gets an answer with that id: a
            # client waiting on one that never comes waits for ever.
            try:
                reply = bridge.handle(message)
            except Exception as error:
                request_id = message.get("id") if isinstance(message, dict) else None
                if request_id is None:
                    return
                reply = {
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "error": {
                        "code": -32603,
                        "message": f"the bridge failed on this request: {error!r}",
                    },
                }
            if reply is not None:
                bridge.send(reply)

        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        running = [each for each in running if each.is_alive()] + [thread]
    deadline = time.monotonic() + drain_seconds
    for thread in running:
        thread.join(max(0.0, deadline - time.monotonic()))


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--port", type=int, help="the game's port; default: eluvian.json's, else 8930"
    )
    parser.add_argument("--exe", type=Path, default=DEFAULT_GAME, help="DAOrigins.exe")
    options = parser.parse_args()

    write_lock = threading.Lock()
    stdout = sys.stdout

    def send(message: dict) -> None:
        with write_lock:
            stdout.write(json.dumps(message) + "\n")
            stdout.flush()

    folder = log_folder()
    processes = Processes(options.exe)
    bridge = Bridge(
        Game(folder, options.port, processes.running),
        processes,
        folder / "eluvian.tools.json",
        send,
    )
    serve(bridge, sys.stdin)


if __name__ == "__main__":
    main()
