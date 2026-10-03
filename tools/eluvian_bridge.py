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


# Who holds the game and its folder: one file beside the logs that every agent's scripts read
# (README.md, "The claim").
CLAIM_FILE = "game.claim.json"
# Every event of the last run, a JSON object a line (core/EventLog.h).
EVENTS_FILE = "eluvian.events.jsonl"
# Its name until 2026-10-02. A bridge started before then writes this one, and a claim there
# went unseen by one started after, which launched over a tester's run (2026-10-02): both
# are read until no bridge with the old name runs.
OLD_CLAIM_FILE = "eluvian.claim.json"


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
    if name == "travel":
        budget_ms = (
            arguments.get("budgetMs", 60000) if isinstance(arguments, dict) else 60000
        )
        budget = (
            budget_ms / 1000
            if isinstance(budget_ms, (int, float)) and budget_ms > 0
            else 60
        )
        return budget + MARGIN_SECONDS
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
        "name": "game_events",
        "description": (
            "The last game's events, read from the file it keeps beside the logs "
            "(eluvian.events.jsonl, every event of the run, written as it happened): for after the "
            "game has closed or crashed, or for more than the running game's buffer holds. "
            '`since` (a seq; default the first), `topic` (a name, a prefix ending in ".*", or a '
            "list of them), `limit` (default 500). Answers `events` as the events tool gives them, "
            "`next` to read on from, `count` matched, and `running` (whether a game runs now, "
            "whose file it is then). A new game starts the file again."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "since": {"type": "integer"},
                "topic": {
                    "description": 'A topic, a prefix like "turnbased.*", or a list'
                },
                "limit": {"type": "integer", "minimum": 1, "maximum": 20000},
            },
        },
        "annotations": {"readOnlyHint": True},
    },
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
        "name": "game_claim",
        "description": (
            "Say the game and its folder are in use: by `owner` (the name you go by), for `note` "
            "(what you are doing), for `minutes` (default 30, at most 240). While another's claim "
            "stands game_launch refuses, and so does every agent's deploy, with the claim's words; "
            "game_status shows it. A game being closed does not mean its folder is free: a test "
            "may have its own loader and plugins in place between two runs. Claim before setting "
            "the folder up, and game_release when it is back as it was. A claim renewed by its "
            "owner runs from now."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "owner": {"type": "string"},
                "note": {"type": "string"},
                "minutes": {"type": "integer", "minimum": 1, "maximum": 240},
            },
            "required": ["owner"],
        },
    },
    {
        "name": "game_release",
        "description": "End your claim on the game and its folder (`owner`, as you claimed it).",
        "inputSchema": {
            "type": "object",
            "properties": {"owner": {"type": "string"}},
            "required": ["owner"],
        },
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
            "starts where it means to. Pass `owner`, the name you go by: health then says who "
            "started the game, so another agent knows it is not its own. If a game already runs "
            "the answer is `launched` false with whose it is: check that before you act. Deploy "
            "first: the game holds its plugins open while it runs."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "waitSeconds": {"type": "integer", "minimum": 0, "maximum": 600},
                "owner": {
                    "type": "string",
                    "description": "Who starts it: your agent's name, shown by health",
                },
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
                        "forceLoad": {
                            "type": "boolean",
                            "description": 'If the game answers the load with "requires the '
                            "following content\", press Force Load (for the user's vanilla "
                            "tests); the game is then never saved",
                        },
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

    def why_silent(self, since: float | None = None) -> str | None:
        """Why Eluvian does not serve in a game that runs, in the logs' own words, or None
        where they do not say. The loader and every plugin must be of one plugin API: a
        loader of another is refused by Eluvian in the game's first second, and the loader
        loads nothing where the address library has no file for the game's build (DAOLib
        8b027c6 on). Each game's start writes the logs anew, and a line names the thread
        that wrote it, not the game (seen 2026-10-01), so what tells this game's log from an
        older one's is its time: `since` is when the game was started."""

        def lines(name: str) -> list[str]:
            log = self.game.folder / name
            try:
                if since is not None and log.stat().st_mtime < since:
                    return []
                return log.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                return []

        def said(line: str) -> str:
            return line.split("] ", 3)[-1].strip()

        loader = lines("loader.log")
        for line in loader:
            if "plugins are not loaded" in line or "eluvian.dll: LoadLibrary" in line:
                return f"loader.log: {said(line)}"
            if "eluvian.dll: refused" in line:
                for own in lines("eluvian.log"):
                    if "stays off" in own:
                        return f"eluvian.log: {said(own)}"
                return f"loader.log: {said(line)}"
        if any("plugin(s) in" in line for line in loader) and not any(
            "eluvian.dll" in line for line in loader
        ):
            return "loader.log: the loader found no eluvian.dll among its plugins"
        return None

    def game_events(self, arguments: dict) -> dict:
        """The run's events from the file the game writes (core/EventLog.h)."""
        file = self.game.folder / EVENTS_FILE
        if not file.exists():
            return text_result(
                f"game_events: no {EVENTS_FILE} in {self.game.folder}; a game with Eluvian "
                "writes it from its start",
                is_error=True,
            )
        since = arguments.get("since", 0)
        since = since if isinstance(since, int) else 0
        limit = arguments.get("limit", 500)
        limit = min(max(limit if isinstance(limit, int) else 500, 1), 20000)
        wanted = arguments.get("topic")
        topics = (
            [wanted]
            if isinstance(wanted, str)
            else [t for t in wanted if isinstance(t, str)]
            if isinstance(wanted, list)
            else []
        )

        def passes(topic: str) -> bool:
            if not topics:
                return True
            for want in topics:
                if want == "*" or want == topic:
                    return True
                if want.endswith(".*") and topic.startswith(want[:-1]):
                    return True
            return False

        events: list[dict] = []
        count = 0
        last = since
        with file.open(encoding="utf-8", errors="replace") as lines:
            for line in lines:
                try:
                    event = json.loads(line)
                except ValueError:
                    continue  # a line the game was writing when it stopped
                if not isinstance(event, dict) or event.get("seq", 0) < since:
                    continue
                last = max(last, event.get("seq", 0) + 1)
                if not passes(str(event.get("topic", ""))):
                    continue
                count += 1
                if len(events) < limit:
                    events.append(event)
        return text_result(
            {
                "events": events,
                "count": count,
                "next": (events[-1]["seq"] + 1) if len(events) == limit else last,
                "running": bool(self.processes.running()),
                "file": str(file),
            }
        )

    def crash_since(self, since: float) -> dict | None:
        """The crash log DAOLib's loader wrote for a game that died after `since`: its file,
        the exception's line and the first frames, and what is known to cause one like it."""
        logs = [
            log
            for log in self.game.folder.glob("crash-*.log")
            if log.stat().st_mtime >= since
        ]
        if not logs:
            return None
        log = max(logs, key=lambda log: log.stat().st_mtime)
        try:
            lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return {"file": str(log), "what": "a crash log that could not be read"}
        what = next(
            (line.strip() for line in lines if line.startswith("Unhandled exception")),
            "an unhandled exception",
        )
        frames = [
            line.strip()
            for line in lines
            if line.strip().startswith("[") and "] 0x" in line
        ][:4]
        crash = {"file": str(log), "what": what, "frames": frames}
        # The renderer failing as it starts, before any save and before any plugin acts: every
        # start from 08:03 on 2026-10-02, after one at 04:04 had come up with the same files.
        # First taken for the display asleep; it went on with the display on, cause not found.
        if any("Renderer::" in frame or "D3D" in frame for frame in frames):
            crash["hint"] = (
                "a crash in the game's renderer as it starts, before any plugin acts; one has "
                "come at every start since a time with no game file changed, so look outside the "
                "game: the display, the graphics driver, another program holding the GPU"
            )
        return crash

    def game_status(self) -> dict:
        pids = self.processes.running()
        health = self.health()
        status = {
            "running": bool(pids),
            "pids": pids,
            "eluvian": health if health is not None else "not answering",
        }
        if health is None:
            why = self.why_silent() if pids else None
            if why:
                status["why"] = why
        others = self.processes.others()
        if others:
            status["otherInstalls"] = others
        claim = self.claim()
        if claim:
            status["claim"] = claim
        missing = self.install_missing()
        if missing:
            status["install"] = (
                f"its game data is missing: {missing}; repair the install"
            )
        return status

    def install_missing(self) -> str | None:
        """What of the game's data an install laid out as the game's (DAOrigins.exe in bin_ship)
        has lost, or None. On 2026-10-02 a delete that followed directory junctions emptied the
        GOG install's packages, modules and the rest, and every start then died in the renderer,
        which was taken for the display for an hour; an empty folder says it at once."""
        exe = self.processes.exe
        if exe.parent.name.lower() != "bin_ship":
            return None
        game = exe.parent.parent
        lost = [
            name
            for name in ("packages/core/data", "modules/single player/data")
            if not any((game / name).glob("*.erf"))
        ]
        return ", ".join(f"{game / name} holds no archive" for name in lost) or None

    # ---- the claim -------------------------------------------------------
    #
    # A running game says whose it is (health's owner). Between a test's two runs nothing did:
    # a watcher waiting for "no game running" deployed and launched six seconds after a
    # tester's game closed, into a folder set up with another loader, and its start wiped the
    # run's logs (2026-10-01). The claim is a small file beside the logs, game.claim.json:
    # {"owner", "note", "since", "until"}, the times in Unix seconds. The user made it the one
    # every agent's deploy and launch scripts honour (2026-10-02), so it is named for the game,
    # not for Eluvian; what it means is in README.md, "The claim".

    def claim_path(self) -> Path:
        return self.game.folder / CLAIM_FILE

    def claim(self) -> dict | None:
        """The claim that stands now, or None: no file, not readable, or past its time. The file
        of the old name is read too."""
        for path in (self.claim_path(), self.game.folder / OLD_CLAIM_FILE):
            claim = self.read_claim(path)
            if claim:
                return claim
        return None

    @staticmethod
    def read_claim(path: Path) -> dict | None:
        try:
            claim = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        if not isinstance(claim, dict) or not isinstance(
            claim.get("until"), (int, float)
        ):
            return None
        left = claim["until"] - time.time()
        if left <= 0:
            return None
        return {
            "owner": claim.get("owner"),
            "note": claim.get("note", ""),
            "minutesLeft": round(left / 60, 1),
        }

    def held_by_another(self, owner: object) -> dict | None:
        claim = self.claim()
        return claim if claim and claim.get("owner") != owner else None

    def game_claim(self, arguments: dict) -> dict:
        owner = arguments.get("owner")
        if not isinstance(owner, str) or not owner:
            return text_result(
                "game_claim: `owner` is the name you go by", is_error=True
            )
        theirs = self.held_by_another(owner)
        if theirs:
            return text_result(
                f"game_claim: the game is claimed by {theirs['owner']} for another "
                f"{theirs['minutesLeft']} minutes ({theirs['note']}); ask them, or wait",
                is_error=True,
            )
        minutes = arguments.get("minutes", 30)
        minutes = min(max(minutes if isinstance(minutes, (int, float)) else 30, 1), 240)
        note = arguments.get("note")
        record = {
            "owner": owner,
            "note": note if isinstance(note, str) else "",
            "since": time.time(),
            "until": time.time() + minutes * 60,
        }
        # Written whole and then put in place, so a reader never sees half of it; read back,
        # so that of two claiming at once the one whose claim did not stand is told.
        path = self.claim_path()
        partial = path.with_name(f"{path.name}.{os.getpid()}.tmp")
        partial.write_text(json.dumps(record), encoding="utf-8")
        os.replace(partial, path)
        theirs = self.held_by_another(owner)
        if theirs:
            return text_result(
                f"game_claim: {theirs['owner']} claimed the game at the same moment "
                f"({theirs['note']}); ask them, or wait",
                is_error=True,
            )
        return text_result({"claimed": True, **(self.claim() or {})})

    def game_release(self, arguments: dict) -> dict:
        owner = arguments.get("owner")
        theirs = self.held_by_another(owner)
        if theirs:
            return text_result(
                f"game_release: the claim is {theirs['owner']}'s, not yours to end",
                is_error=True,
            )
        had = self.claim() is not None
        for path in (self.claim_path(), self.game.folder / OLD_CLAIM_FILE):
            try:
                path.unlink()
            except OSError:
                pass
        return text_result({"released": had})

    def game_launch(self, arguments: dict) -> dict:
        environment = {}
        if isinstance(arguments.get("logLevel"), str):
            environment["DAOLIB_LOG_LEVEL"] = arguments["logLevel"]
        # Who starts it, for health's `owner`: agents share one game, and whose a running one
        # is was asked again and again, and once got wrong (2026-10-01).
        owner = arguments.get("owner")
        environment["ELUVIAN_OWNER"] = owner if isinstance(owner, str) else ""
        theirs = self.held_by_another(owner)
        if theirs:
            return text_result(
                f"game_launch: NOT launched. The game and its folder are claimed by "
                f"{theirs['owner']} for another {theirs['minutesLeft']} minutes "
                f"({theirs['note']}). No game running does not make the folder free: wait for "
                "their game_release, or ask them.",
                is_error=True,
            )
        # Looking for a game and starting one are a single step, under a lock: requests run
        # on threads of their own, and two launches at once each saw no game and each started
        # one (a review's finding). The lock is the machine's, so two bridges agree too.
        with self.launching:
            pids = self.processes.running()
            if pids:
                status = self.game_status()
                health = status.get("eluvian")
                theirs = health.get("owner") if isinstance(health, dict) else None
                whose = (
                    f"started by {theirs}"
                    if theirs
                    else "started by hand or by an older bridge: likely the player's own"
                )
                if not (isinstance(owner, str) and owner and owner == theirs):
                    status["note"] = (
                        f"NOT launched: a game is already running, {whose}. It is not yours to "
                        "drive or close; read it if you must, and wait for it to end."
                    )
                return text_result({"launched": False, "already": pids, **status})
            # Another install's game is someone's test (DAOLib's work on EA's and Steam's
            # executables), and the two share Documents\\BioWare\\Dragon Age: saves, settings
            # and the logs. Two GOG games started beside a Steam one each exited at once
            # (2026-10-02), and the user wants no second game opened: so none is.
            others = self.processes.others()
            if others:
                return text_result(
                    {
                        "launched": False,
                        "otherInstalls": others,
                        "note": "game_launch: NOT launched. Another install's game is running "
                        f"({others[0].get('exe', 'another path')}); one game runs at a time. "
                        "game_status says whose claim it is under, if any; wait for it to close.",
                    }
                )
            if not self.processes.exe.exists():
                return text_result(
                    f"game_launch: no {self.processes.exe}", is_error=True
                )
            missing = self.install_missing()
            if missing:
                return text_result(
                    f"game_launch: NOT launched. The install has lost its game data: {missing}. "
                    "Repair it (GOG Galaxy: the game's settings, Manage installation, Verify / "
                    "Repair); a game started like this crashes in its renderer's start-up.",
                    is_error=True,
                )
            started = time.time() - 2  # the logs of this start are no older
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
                answer = {
                    "launched": True,
                    "pid": pid,
                    "error": "the game exited while starting; crash-*.log in "
                    f"{self.game.folder} may say why",
                }
                crash = self.crash_since(started)
                if crash:
                    answer["crash"] = crash
                    answer["error"] = (
                        f"the game crashed while starting: {crash['what']}"
                    )
                    if crash.get("hint"):
                        answer["error"] += f" ({crash['hint']})"
                return text_result(answer, is_error=True)
            self.sleep(1)
        self.game_tools()  # lists the game's tools now it serves, and says so if they changed
        self.send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
        answer = {"launched": True, "pid": pid, "health": health}
        if others:
            answer["otherInstalls"] = others
        if health is None:
            why = self.why_silent(started)
            if why:
                answer["error"] = (
                    f"the game runs and Eluvian is not serving in it: {why}"
                )
                return text_result(answer, is_error=True)
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
        forced = False
        force = save.get("forceLoad") is True
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
                loaded = {"save": body.get("save"), "event": event.get("event")}
                if forced:
                    loaded["forceLoaded"] = True
                    loaded["note"] = (
                        "force-loaded without content the save needs: for testing only, never "
                        "saved (the game tool refuses a save until another load)"
                    )
                return loaded
            popup = self.popup()
            if popup is not None and force and not forced and self.force_load():
                # The user allows Force Load for vanilla tests (2026-10-02); the game it loads
                # is never saved, which the game tool itself holds to.
                forced = True
                asked_at = time.monotonic()
                continue
            if popup is not None:
                labels = ", ".join(
                    str(button.get("label")) for button in popup.get("buttons", [])
                )
                return {
                    "error": "the game answered the load with a message box and waits on it: "
                    f'"{popup.get("text", "")}" [{labels}]. No tool presses its buttons; a person '
                    "must, or launch again with load's forceLoad true where the user allows it. Do not "
                    "Force Load and then save: the save would lose that content.",
                    "popup": popup,
                }
            mode = self.mode()
            waited = time.monotonic() - asked_at
            if (
                force
                and not forced
                and mode == before
                and waited >= self.load_patience
                and self.force_load()
            ):
                forced = True
                asked_at = time.monotonic()
                continue
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

    def force_load(self) -> bool:
        """The game's own Force Load, through its game tool; whether the game took it."""
        try:
            status, body = self.game.request(
                "POST", "/api/tool/game", {"action": "forceLoad"}, timeout=30
            )
        except (Unreachable, StillWaiting):
            return False
        return status == 200 and isinstance(body, dict) and body.get("forced") is True

    def popup(self) -> dict | None:
        """The game's message box, if one is up and this build of Eluvian can read it.

        inspect kind=gui has it since #2; an Eluvian from before that answers
        kind=popup, which this asks when kind=gui is refused."""
        for kind in ("gui", "popup"):
            try:
                status, body = self.game.request(
                    "POST", "/api/tool/inspect", {"kind": kind}, timeout=10
                )
            except (Unreachable, StillWaiting):
                return None
            if status == 200 and isinstance(body, dict):
                popup = body.get("popup")
                return popup if isinstance(popup, dict) else None
        return None

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
            if name == "game_events":
                return reply(self.game_events(arguments))
            if name == "game_saves":
                character = arguments.get("character")
                saves = list_saves(
                    self.characters, character if isinstance(character, str) else None
                )
                return reply(text_result({"count": len(saves), "saves": saves}))
            if name == "game_status":
                return reply(text_result(self.game_status()))
            if name == "game_claim":
                return reply(self.game_claim(arguments))
            if name == "game_release":
                return reply(self.game_release(arguments))
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
    parser.add_argument(
        "--call",
        nargs="+",
        metavar=("TOOL", "ARGUMENTS"),
        help="call one tool and print its answer as JSON, then exit: for a script or an agent "
        "with no MCP client, above all game_launch, game_quit, game_saves and game_status, "
        "which answer while no game runs. ARGUMENTS is a JSON object, such as "
        '{"owner": "me"}',
    )
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
    if options.call:
        sys.exit(call_once(bridge, options.call, stdout))
    serve(bridge, sys.stdin)


def call_once(bridge: Bridge, call: list[str], out) -> int:
    """One tool call from the command line: its answer as JSON on `out`; 1 if it is an error."""
    try:
        arguments = json.loads(call[1]) if len(call) > 1 else {}
    except ValueError as error:
        print(
            json.dumps({"isError": True, "error": f"ARGUMENTS is not JSON: {error}"}),
            file=out,
        )
        return 2
    bridge.send = lambda _message: None  # no client to notify
    answer = bridge.handle(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": call[0], "arguments": arguments},
        }
    )
    result = (answer or {}).get("result", (answer or {}).get("error", {}))
    failed = (
        not isinstance(result, dict) or bool(result.get("isError")) or "code" in result
    )
    body = result.get("structuredContent") if isinstance(result, dict) else None
    if body is None and isinstance(result, dict):
        texts = [
            c.get("text") for c in result.get("content", []) if c.get("type") == "text"
        ]
        body = {"text": " ".join(t for t in texts if t)} if texts else result
    print(json.dumps({"isError": failed, "answer": body}, indent=1), file=out)
    return 1 if failed else 0


if __name__ == "__main__":
    main()
