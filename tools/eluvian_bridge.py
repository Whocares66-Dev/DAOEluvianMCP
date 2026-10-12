"""Eluvian's stdio bridge: an MCP server an agent keeps across game restarts.

Eluvian serves MCP from inside DAOrigins.exe, so a direct connection dies
with the game, and a client that started before the game marks it failed
until someone reconnects by hand. This bridge is the stable end: the agent's
client starts it over stdio, and it forwards each tool call to the game's
REST API (POST /api/tool/<name>) while the game runs.

It adds what only a process outside the game can do:
  game_status   is the game running, and what Eluvian's `health` says
  game_launch   start DAOrigins.exe (never a second of one copy), wait until it serves
  game_quit     close the game
  game_instance which copy of the game this bridge drives: the game as ever, or a named
                instance beside it with a profile of its own (#116)

The game's tools are listed from its GET /api/tools, and kept in
eluvian.tools.json beside its logs, so they are listed even while the game is
down; calling one then says how to start it.

    claude mcp add eluvian -- python C:\\project\\DAOEluvianMCP\\tools\\eluvian_bridge.py

A build copies this file beside eluvian.dll, and a release's zip has it there, in the
game's bin_ship\\daolib\\plugins\\eluvian\\: started from there it takes that game as
its own, and from here the GOG install's unless --exe names another.

Standard library only. tools/test_bridge.py tests it against a stand-in for
the game.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import shutil
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
# The project's, CMakeLists.txt's: test_bridge.py holds the two together.
VERSION = "0.1.0"


def default_game(script: Path) -> Path:
    """The game a bridge started with no --exe is for. A release has the bridge in the plugin's
    folder, bin_ship\\daolib\\plugins\\eluvian\\, so there it is that install's game, whichever
    store's it is (#115); from a source tree it is the GOG install's."""
    folder = script.resolve().parent
    if (folder.parent.name.lower(), folder.parent.parent.name.lower()) == (
        "plugins",
        "daolib",
    ):
        beside = folder.parents[2] / DEFAULT_GAME.name
        if beside.exists():
            return beside
    return DEFAULT_GAME


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


def known_folder(guid: str, fallback: Path) -> Path:
    """One of Windows' known folders, by its id's bytes; `fallback` where there is no Windows."""
    if sys.platform == "win32":
        folder = ctypes.c_char_p(bytes.fromhex(guid.replace(" ", "")))
        path = ctypes.c_wchar_p()
        if (
            ctypes.windll.shell32.SHGetKnownFolderPath(
                folder, 0, None, ctypes.byref(path)
            )
            == 0
        ):
            found = Path(path.value)
            ctypes.windll.ole32.CoTaskMemFree(path)
            return found
    return fallback


def documents_folder() -> Path:
    """The user's Documents, where the game writes, wherever it really is (OneDrive here)."""
    return known_folder(
        "D0 9A D3 FD 8F 23 AF 46 AD B4 6C 85 48 03 69 C7", Path.home() / "Documents"
    )


# Every event of the last run, a JSON object a line (core/EventLog.h).
EVENTS_FILE = "eluvian.events.jsonl"
# A game's that found the first port taken, named by its pid as its eluvian.<pid>.json is.
OWN_EVENTS_FILE = re.compile(r"eluvian\.\d+\.events\.jsonl")


def written(file: Path) -> float:
    """When a file was last written; 0 for one that is not there."""
    try:
        return file.stat().st_mtime
    except OSError:
        return 0.0


# The pid of a copy this bridge started, written beside its logs at once: Eluvian's own
# eluvian.json comes seconds later, and until then nothing else says whose the process is.
LAUNCH_FILE = "eluvian.launch.json"

# A second copy of the game beside the first (#116). DAOLib's loader makes a copy started with
# DAOLIB_INSTANCE=<name> an instance (DAOLib #67; its dev/BUILDS.md, "a second copy beside the
# first", is the contract this stands on): the name is 1 to 32 letters, digits, '-' or '_' and
# is taken in lower case; the game's start-up mutex is "dragonage-<name>" where the game as
# ever has "dragonage", which is the whole of "one game at a time"; and its profile, with the
# logs, is %LOCALAPPDATA%\DAOLib\instances\<name>\BioWare\Dragon Age where the game as ever
# has Documents\BioWare\Dragon Age. The bridge runs outside the game, so nothing is redirected
# for it: it works the folder out from the name.
INSTANCE_NAME = re.compile(r"[A-Za-z0-9_-]{1,32}")


def instance_name(name: object) -> str | None:
    """An instance's name as the loader takes it, in lower case; "" for the game as ever
    (no name, or an empty one); None for a name the loader would refuse."""
    if name is None or name == "":
        return ""
    if isinstance(name, str) and INSTANCE_NAME.fullmatch(name):
        return name.lower()
    return None


def instances_root() -> Path:
    """Where the instances' profiles are, a folder each by name."""
    local = known_folder(
        "85 27 B3 F1 BA 6F CF 4F 9D 55 7B 8E 7F 15 70 91",
        Path.home() / "AppData" / "Local",
    )
    return local / "DAOLib" / "instances"


def profile_folder(instance: str, root: Path | None = None) -> Path:
    """A copy's profile, the folder the game keeps Settings, Characters and the rest in: the
    instance's own, or Documents' for the game as ever."""
    if instance:
        base = root if root is not None else instances_root()
        return base / instance / "BioWare" / "Dragon Age"
    return documents_folder() / "BioWare" / "Dragon Age"


def log_folder(instance: str = "", root: Path | None = None) -> Path:
    return profile_folder(instance, root) / "daolib"


def mutex_held(name: str) -> bool:
    """Whether a mutex of that name is there, in this session: the game makes "dragonage", or
    an instance "dragonage-<name>", in WinMain and it goes with the process, so this asks
    whether that copy is running (not in its first moment, before WinMain)."""
    if sys.platform != "win32":
        return False
    kernel32 = ctypes.windll.kernel32
    kernel32.OpenMutexW.restype = ctypes.c_void_p
    handle = kernel32.OpenMutexW(0x00100000, False, name)  # SYNCHRONIZE
    if not handle:
        return False
    kernel32.CloseHandle(ctypes.c_void_p(handle))
    return True


def mutex_of(instance: str) -> str:
    return f"dragonage-{instance}" if instance else "dragonage"


# What a new instance's profile is given of the default one's when the bridge seeds it: the
# settings and key bindings, and the lists that say which add-ins are installed and allowed,
# without which a save of the default profile is answered "requires the following content"
# (lib-agent's play test, 2026-10-11). All of it was under 1 MB here, against 5.9 GB of
# Characters, which the loader copies whole when it seeds an instance itself.
SEED_PARTS = ("Settings", "addins", "Offers", "packages")


def seed_instance(
    instance: str, save: dict | None, default: Path, root: Path
) -> dict | None:
    """Gives a new instance a profile with the settings and one save, in place of the whole
    of the default profile's saves the loader would copy. Only for an instance that has no
    folder yet: one that has is left as it is, and nothing is answered. `save` is as
    game_saves lists it (character, kind, number), or None for no save at all. The file
    `seeded` beside the profile is the loader's own mark that there is nothing left to copy."""
    home = root / instance
    if home.exists():
        return None
    profile = home / "BioWare" / "Dragon Age"
    profile.mkdir(parents=True)
    copied = []
    for part in SEED_PARTS:
        if (default / part).is_dir():
            shutil.copytree(default / part, profile / part)
            copied.append(part)
    if save:
        character = default / "Characters" / str(save.get("character"))
        prefix = {kind: prefix for prefix, kind in SAVE_KINDS}.get(save.get("kind"))
        slot = character / "Saves" / f"{prefix}{save.get('number')}"
        if prefix and slot.is_dir():
            target = profile / "Characters" / character.name
            (target / "Saves").mkdir(parents=True)
            # The character's own files, its portrait and its sheet, beside the one save.
            for file in character.iterdir():
                if file.is_file():
                    shutil.copy2(file, target / file.name)
            shutil.copytree(slot, target / "Saves" / slot.name)
            copied.append(f"Characters\\{character.name}\\Saves\\{slot.name}")
        else:
            copied.append(f"no save {save} in {default}")
    (home / "seeded").write_text("seeded\n", encoding="utf-8")
    return {"profile": str(profile), "copied": copied}


class Unreachable(Exception):
    """Nothing answered at the game's port."""


class StillWaiting(Exception):
    """The game took the request and had not answered when the bridge stopped waiting: the call
    may be running there yet, which is not the game being down."""


# How long the bridge waits for a tool's answer. A scenario may run its ten minutes, and a
# wait_for its own timeout; anything else answers within the frame queue's few seconds.
# How long a call of a tool that declares no budget is waited for.
DEFAULT_WAIT_SECONDS = 60
MARGIN_SECONDS = 30


def budget_of_tool(tool: object) -> dict | None:
    """A listed tool's declared budget: its _meta's eluvian/budget, the argument that bounds a
    call and its default (#108); None for one that declares none."""
    meta = tool.get("_meta") if isinstance(tool, dict) else None
    budget = meta.get("eluvian/budget") if isinstance(meta, dict) else None
    return budget if isinstance(budget, dict) else None


def call_budget(arguments: object, budget: dict | None) -> float:
    """Seconds to wait for a tool's answer: longer than the tool itself may take, so the bridge
    never gives up on a call the game is still running (a review found a two-minute scenario
    given 60 s, then reported as the game not running while its later steps went on). How long
    that is, the tool declares itself: the argument that bounds a call, or its default, plus a
    margin. The bridge names no game tool; it did, and cut a sweep of loot off at 60 s (the
    user, 2026-10-09: "there should be a single source of truth", #108)."""
    if not isinstance(budget, dict):
        return float(DEFAULT_WAIT_SECONDS)
    default = budget.get("defaultMs")
    default_ms = default if isinstance(default, (int, float)) and default > 0 else 60000
    argument = budget.get("argument")
    given = (
        arguments.get(argument)
        if isinstance(arguments, dict) and isinstance(argument, str)
        else None
    )
    ms = given if isinstance(given, (int, float)) and given > 0 else default_ms
    return ms / 1000 + MARGIN_SECONDS


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
        self.found: tuple[float, int | None] | None = None

    def read_runtime(self, name: str) -> dict | None:
        try:
            runtime = json.loads((self.folder / name).read_text(encoding="utf-8"))
            return runtime if isinstance(runtime.get("port"), int) else None
        except (OSError, ValueError, AttributeError):
            return None

    def answers_as(self, port: int, pids: set[int]) -> bool:
        """Whether the game answering at `port` is one of `pids`, by its own health."""
        try:
            with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/api/health", timeout=2
            ) as response:
                return json.loads(response.read() or b"null").get("pid") in pids
        except (OSError, ValueError, AttributeError):
            return False

    def find_port(self) -> int | None:
        """The port of this copy's game, or None where no game of this copy vouches for one.
        eluvian.json's, if the game it names runs; else a running game's own file,
        eluvian.<pid>.json, which a game that found the first port taken writes. A second
        game once wrote its port over the first's and went away (2026-09-30): the file named
        a game that was gone while the first still served, on the default port. So a port
        the file's own game does not stand behind is tried, the file's and the default, and
        taken only if the game answering there says in its health that it is one of this
        copy's processes: an instance started while no other game was up took the default
        port itself, and a bridge for the game as ever would have sent its tools there
        (shaders-agent, 2026-10-11, #119). With no game of this copy running there is none
        to ask, and no port."""
        named = self.read_runtime("eluvian.json")
        if self.running is None:
            # A bridge given no way to tell which games run: the file's word, or the default.
            return named["port"] if named else DEFAULT_PORT
        pids = set(self.running())
        if not pids:
            return None
        if named and named.get("pid") in pids:
            return named["port"]
        for pid in sorted(pids):
            own = self.read_runtime(f"eluvian.{pid}.json")
            if own:
                return own["port"]
        guesses = ([named["port"]] if named else []) + [DEFAULT_PORT]
        for port in dict.fromkeys(guesses):
            if self.answers_as(port, pids):
                return port
        return None

    def events_file(self, pids: set[int], previous: bool = False) -> Path:
        """The file of this copy's game's events (core/EventLog.h), or of the run before it.
        The game find_port chooses is the one whose events these are: eluvian.json's game
        writes eluvian.events.jsonl, and a game that found the first port taken its own
        eluvian.<pid>.events.jsonl. Only the plain name was read, so a second instance's
        game_events answered a run of hours before while its events tool answered the live
        ones (kadan-agent, 2026-10-12, #121; that Eluvian was older than #116, and the game
        as ever beside an instance holding the first port writes the same today). With no
        game of this copy running, the last game's is the one written last."""
        named = self.read_runtime("eluvian.json")
        file = self.folder / EVENTS_FILE
        if not pids:
            runs = [
                run
                for run in self.folder.glob("eluvian.*.events.jsonl")
                if OWN_EVENTS_FILE.fullmatch(run.name)
            ]
            file = max([file, *runs], key=written)
        elif not (named and named.get("pid") in pids):
            own = (self.folder / f"eluvian.{pid}.events.jsonl" for pid in sorted(pids))
            file = next((run for run in own if run.exists()), file)
        # As EventLog::PreviousOf names it.
        return file.with_name(file.stem + ".previous.jsonl") if previous else file

    def port(self) -> int | None:
        if self.fixed_port is not None:
            return self.fixed_port
        now = time.monotonic()
        # No port is asked again sooner: a game just started has one within seconds.
        if self.found is None or now - self.found[0] > (
            5 if self.found[1] is not None else 1
        ):
            self.found = (now, self.find_port())
        return self.found[1]

    def request(
        self, method: str, path: str, body: object = None, timeout: float = 10
    ) -> tuple[int, object]:
        port = self.port()
        if port is None:
            raise Unreachable("no game of this copy has said where it answers")
        url = f"http://127.0.0.1:{port}{path}"
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
    """DAOrigins.exe as Windows sees it: find, start, close. One of these is for one copy of
    the game: the game as ever, or the instance named."""

    def __init__(
        self,
        exe: Path,
        instance: str = "",
        root: Path | None = None,
        held: Callable[[str], bool] = mutex_held,
        logs: Path | None = None,
    ) -> None:
        self.exe = exe
        self.instance = instance
        self.root = root if root is not None else instances_root()
        self.held = held
        # This copy's own folder beside its logs, for what it says of its own process.
        self.logs = logs if logs is not None else log_folder(instance, self.root)

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

    def of_install(self) -> list[int]:
        """This install's games, every copy. A second install of the game has the same file
        name (the user installed EA's beside GOG's, 2026-10-01): a game of another install is
        not Eluvian's to attach to, count as its own, or close."""
        return [pid for pid in self.named() if is_same_file(image_path(pid), self.exe)]

    @staticmethod
    def said(logs: Path, trusted: bool) -> list[int]:
        """The pids a copy's folder names: the one its Eluvian wrote in eluvian.json, and the
        one a bridge wrote as it started it. Eluvian's file is of some run, this one or one
        long over, so it counts only where `trusted` says the copy runs; a bridge's record
        counts for two minutes whatever else, a copy's mutex coming a moment after its
        process."""
        pids = []
        for file in (LAUNCH_FILE, "eluvian.json"):
            try:
                recent = time.time() - (logs / file).stat().st_mtime < 120
                pid = json.loads((logs / file).read_text(encoding="utf-8")).get("pid")
            except (OSError, ValueError, AttributeError):
                continue
            if isinstance(pid, int) and (trusted or (file == LAUNCH_FILE and recent)):
                pids.append(pid)
        return pids

    def attribution(self) -> tuple[list[int], dict[int, str], list[str]]:
        """The install's processes, which of them are instances' by name, and the instances
        that run with no process named. Every copy of one install is the same file, so a
        process does not say whose it is; each instance's own folder does, while the instance
        runs (its mutex is there): what a run that ended left behind names a pid that may be
        another game's by now. An instance started by hand with no Eluvian in it leaves no
        word at all: its mutex says that it runs, and nothing says which process it is."""
        pids = self.of_install()
        found: dict[int, str] = {}
        unnamed: list[str] = []
        try:
            folders = [folder for folder in self.root.iterdir() if folder.is_dir()]
        except OSError:
            return pids, found, unnamed
        for folder in folders:
            name = folder.name.lower()
            runs = self.held(mutex_of(name))
            theirs = self.said(log_folder(name, self.root), runs)
            for pid in theirs:
                found[pid] = name
            if runs and not any(pid in pids for pid in theirs):
                unnamed.append(name)
        return pids, found, unnamed

    def instances(self) -> dict[int, str]:
        """Which pids are instances', by name."""
        return self.attribution()[1]

    def running(self) -> list[int]:
        """This copy's game only: the instance's, or for the game as ever whichever of the
        install's is no instance's. While an instance runs that names no process, one of
        those is its, and which is not known: then the game as ever's are only the ones its
        own folder names, and with none named it has none. That is wrong for the moment a
        game started by hand takes to say its pid, and is the cheaper mistake: the other is
        another's game closed, or a start refused because another's runs (#119)."""
        pids, theirs, unnamed = self.attribution()
        mine = [pid for pid in pids if theirs.get(pid, "") == self.instance]
        if self.instance or not unnamed:
            return mine
        own = self.said(self.logs, self.held(mutex_of("")))
        return [pid for pid in mine if pid in own]

    def copies(self) -> list[dict]:
        """The install's other copies running now: the game as ever (instance "") and the
        instances that are not this one, each with its process; an instance that names no
        process is listed without one."""
        pids, theirs, unnamed = self.attribution()
        others = [
            {"instance": theirs.get(pid, ""), "pid": pid}
            for pid in pids
            if theirs.get(pid, "") != self.instance and (theirs.get(pid) or not unnamed)
        ]
        return others + [
            {"instance": name, "pid": None} for name in unnamed if name != self.instance
        ]

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

# The most of each kind the game keeps: literals in its save handler, which
# starts no save beyond them (DAOLib's dao::saves::MostSlot, read by lib-agent,
# 2026-10-05). A character with 500 manual saves can make no more (#79).
MOST_SAVES = {"manual": 500, "auto": 4, "quick": 1}


def save_counts(saves: list[dict]) -> dict:
    """Each character's saves of each kind against the most the game keeps."""
    counts: dict = {}
    for save in saves:
        kinds = counts.setdefault(
            save["character"],
            {kind: {"count": 0, "most": most} for kind, most in MOST_SAVES.items()},
        )
        kinds[save["kind"]]["count"] += 1
    for kinds in counts.values():
        for entry in kinds.values():
            entry["full"] = entry["count"] >= entry["most"]
    return counts


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
            "`next` to read on from, `count` matched, `running` (whether a game runs now, "
            "whose file it is then) and `file`, the one read: a game that found the first port "
            "taken keeps eluvian.<pid>.events.jsonl, and with no game running the file written "
            "last is read. A new game starts the file again, keeping the run before's: "
            "`previous: true` reads that one (a crash's, after a launch)."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "since": {"type": "integer"},
                "previous": {"type": "boolean"},
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
            "the save's name (the area's, as the game names it), when it was saved, and its folder; "
            "`kinds`, each character's count of each kind against the most the game keeps "
            "(500 manual, 4 auto, 1 quick: past it, `full`, a save is refused). "
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
            "inside it. Answers while the game is down. Where several copies of the game run, this "
            "is of the one this bridge drives (`instance`, see game_instance), and `copies` lists "
            "the others."
        ),
        "inputSchema": {"type": "object"},
        "annotations": {"readOnlyHint": True},
    },
    {
        "name": "game_launch",
        "description": (
            "Start DAOrigins.exe, never a second of the same copy, and wait until Eluvian serves "
            "at the main menu; answers its health. The copy is the one this bridge drives: the "
            "game as ever, or the instance game_instance chose, which runs beside it with a "
            "profile of its own. "
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
                "plugins": {
                    "type": "string",
                    "description": "A folder to load the plugins from in place of the game's "
                    "bin_ship\\daolib\\plugins, as it stands: a build tree's daolib\\plugins, "
                    "so a test deploys nothing into the game. It must hold eluvian\\eluvian.dll",
                },
                "seed": {
                    "type": "string",
                    "enum": ["profile", "save"],
                    "description": "For an instance started for the first time. profile (the "
                    "default): the loader copies the whole default profile, every save of every "
                    "character, which takes as long as they are large. save: only the settings "
                    "and the one save in `load`, for a copy made for a test",
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
        "name": "game_instance",
        "description": (
            "Which copy of the game this bridge drives, and a way to drive another. The game runs "
            "one at a time, except as a named instance: a second copy beside the first, with its "
            "own saves, settings, logs and port, so two tests need not share a game. With `name` "
            "(1 to 32 letters, digits, - or _; lower case is the same name), every tool of this "
            "bridge from then on is that instance's: game_launch starts it, game_status, "
            "game_saves and game_quit are of it, and the game's own tools go to it. With an "
            "empty `name` it is the game as ever again. With no `name` it only answers which "
            "copy it is, the folder of its logs and the other copies running. A new instance's "
            "first start copies the default profile (game_launch's `seed`). Every copy runs "
            "from the one game folder and holds its plugins open, so a deploy still needs all "
            "of them closed. The first copy may freeze for some seconds while a second starts."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {
                    "type": "string",
                    "description": "The instance to drive from now on; empty for the game as ever",
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
        copies: Callable[[str], tuple[Game, Processes, Path, Path]] | None = None,
        default: Path | None = None,
    ) -> None:
        self.game = game
        self.processes = processes
        self.cache = cache
        # Which copy of the game this is the bridge of (#116): the game as ever, "", or an
        # instance by name. `copies` makes the four things above for another copy, for
        # game_instance; a bridge given none stays with the copy it has.
        self.instance: str = getattr(processes, "instance", "")
        self.copies = copies
        # The profile of the game as ever, whichever copy this drives: what a new instance
        # is seeded from (game_launch's `seed`).
        self.default = default if default is not None else game.folder.parent
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
        # Each listed game tool's declared budget, None where it declares none (#108).
        self.budgets: dict[str, dict | None] = {}
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
                self.note_budgets(tools)
                try:
                    self.cache.write_text(json.dumps(tools), encoding="utf-8")
                except OSError:
                    pass  # a cache not written is a list not kept, nothing worse
                return tools
        except (Unreachable, StillWaiting):
            pass
        try:
            kept = json.loads(self.cache.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return []
        self.note_budgets(kept)
        return kept

    def note_budgets(self, tools: object) -> None:
        """Keeps each listed tool's declared budget, for the wait on its calls."""
        if isinstance(tools, list):
            self.budgets = {
                tool["name"]: budget_of_tool(tool)
                for tool in tools
                if isinstance(tool, dict) and isinstance(tool.get("name"), str)
            }

    def budget_of(self, name: str) -> dict | None:
        """A game tool's declared budget, from the list last read; read again for one not in it."""
        if name not in self.budgets:
            self.game_tools()
        return self.budgets.get(name)

    def note_revision(self, revision: object) -> None:
        """Tells the client to list again when the game's tools changed (a plugin registered)."""
        with self.lock:
            changed = self.revision is not None and revision != self.revision
            self.revision = revision if isinstance(revision, int) else self.revision
        if changed:
            self.send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})

    def call_game(self, name: str, arguments: dict) -> dict:
        timeout = call_budget(arguments, self.budget_of(name))
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
            # A loader of DAOLib #26 on loads a plugin only from a folder of its
            # own, and says so of one left loose in the plugins folder.
            if (
                "plugins are not loaded" in line
                or "eluvian.dll: LoadLibrary" in line
                or "eluvian.dll: not loaded" in line
            ):
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
        previous = arguments.get("previous") is True
        file = self.game.events_file(set(self.processes.running()), previous)
        if not file.exists():
            return text_result(
                f"game_events: no {file.name} in {self.game.folder}; "
                + (
                    "a game with Eluvian keeps the run before's there when it starts"
                    if previous
                    else "a game with Eluvian writes it from its start"
                ),
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

    def exit_since(self, since: float) -> dict | None:
        """The log DAOLib's loader writes when the game ends itself before its first frame
        (exit-<time>.log, beside the crash logs; DAOLib #44, 2026-10-09), the newest written after
        `since`: the call that ended it, the call stack past frame 0 (the logger's own hook), and
        the first return addresses on the stack, which named ___tmainCRTStartup where the call
        stack was short (lib-agent, 2026-10-09). The build and system lines say nothing of why."""
        logs = [
            log
            for log in self.game.folder.glob("exit-*.log")
            if log.stat().st_mtime >= since
        ]
        if not logs:
            return None
        log = max(logs, key=lambda log: log.stat().st_mtime)
        try:
            lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return {"file": str(log), "what": "an exit log that could not be read"}

        def section(title: str) -> list[str]:
            frames: list[str] = []
            inside = False
            for line in lines:
                if line.startswith(title):
                    inside = True
                elif inside and line.strip().startswith("["):
                    frames.append(line.strip())
                elif inside:
                    break
            return frames

        what = next(
            (line.strip() for line in lines if line.startswith("The game called")),
            "the game ended itself before its first frame",
        )
        frames = [
            f for f in section("PROBABLE CALL STACK:") if not f.startswith("[ 0]")
        ]
        return {
            "file": str(log),
            "what": what,
            "frames": frames[:8],
            "returns": section("RETURN ADDRESSES ON THE STACK:")[:4],
        }

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
        if self.instance:
            status["instance"] = self.instance
        copies = self.processes.copies()
        if copies:
            status["copies"] = copies
        others = self.processes.others()
        if others:
            status["otherInstalls"] = others
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

    # ---- the copy this bridge drives --------------------------------------------

    def game_instance(self, arguments: dict) -> dict:
        """Says which copy of the game this bridge drives, or changes it (#116)."""
        if "name" in arguments:
            name = instance_name(arguments.get("name"))
            if name is None:
                return text_result(
                    "game_instance: `name` is 1 to 32 letters, digits, - or _ (the loader takes "
                    "no other), or empty for the game as ever",
                    is_error=True,
                )
            if name != self.instance:
                if self.copies is None:
                    return text_result(
                        "game_instance: this bridge was started for one copy of the game and "
                        "cannot change it",
                        is_error=True,
                    )
                game, processes, cache, characters = self.copies(name)
                with self.lock:
                    self.game, self.processes = game, processes
                    self.cache, self.characters = cache, characters
                    self.instance = name
                    self.revision = None
                    self.budgets = {}
                # Another copy may have other plugins, and so other tools.
                self.send(
                    {"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}
                )
        # Of one copy, taken together: requests run on threads of their own, and an answer
        # read piece by piece beside a change named the game as ever and listed its own
        # process among the others (seen 2026-10-11, two calls sent at once).
        with self.lock:
            instance, game, processes = self.instance, self.game, self.processes
        return text_result(
            {
                "instance": instance or None,
                "folder": str(game.folder),
                "running": bool(processes.running()),
                "copies": processes.copies(),
            }
        )

    def game_launch(self, arguments: dict) -> dict:
        environment = {}
        if isinstance(arguments.get("logLevel"), str):
            environment["DAOLIB_LOG_LEVEL"] = arguments["logLevel"]
        # Who starts it, for health's `owner`: agents share one game, and whose a running one
        # is was asked again and again, and once got wrong (2026-10-01).
        owner = arguments.get("owner")
        environment["ELUVIAN_OWNER"] = owner if isinstance(owner, str) else ""
        if self.instance:
            environment["DAOLIB_INSTANCE"] = self.instance
        # The plugins of a build tree in place of the game folder's (DAOLib #67's
        # DAOLIB_PLUGINS): a folder that is not there loads nothing, and one without Eluvian
        # starts a game this bridge cannot reach, so both are said before the start.
        plugins = arguments.get("plugins")
        if plugins is not None:
            folder = Path(plugins) if isinstance(plugins, str) else None
            if folder is None or not (folder / "eluvian" / "eluvian.dll").is_file():
                return text_result(
                    f"game_launch: NOT launched. `plugins` is a folder that holds "
                    f"eluvian\\eluvian.dll, such as a build tree's daolib\\plugins; {plugins!r} "
                    "does not",
                    is_error=True,
                )
            environment["DAOLIB_PLUGINS"] = str(folder.resolve())
        seed = arguments.get("seed", "profile")
        if seed not in ("profile", "save"):
            return text_result("game_launch: `seed` is profile or save", is_error=True)
        if seed == "save" and not self.instance:
            return text_result(
                "game_launch: `seed` is for an instance (game_instance): the game as ever has "
                "the profile it has",
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
            # An instance shares neither with anyone: its mutex and its profile are its own.
            others = self.processes.others()
            if others and not self.instance:
                return text_result(
                    {
                        "launched": False,
                        "otherInstalls": others,
                        "note": "game_launch: NOT launched. Another install's game is running "
                        f"({others[0].get('exe', 'another path')}); one game runs at a time. "
                        "Wait for it to close.",
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
            seeded = None
            if self.instance and seed == "save":
                load = arguments.get("load")
                seeded = seed_instance(
                    self.instance,
                    load if isinstance(load, dict) else None,
                    self.default,
                    self.game.folder.parents[3],
                )
            started = time.time() - 2  # the logs of this start are no older
            pid = self.processes.start(environment)
            # Whose the new process is, until its Eluvian says so itself (Processes.said):
            # for an instance, and for the game as ever beside an instance that names none.
            try:
                self.game.folder.mkdir(parents=True, exist_ok=True)
                (self.game.folder / LAUNCH_FILE).write_text(
                    json.dumps({"pid": pid, "instance": self.instance}),
                    encoding="utf-8",
                )
            except OSError:
                pass
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
                ended = self.exit_since(started)
                if ended:
                    answer["exit"] = ended
                    answer["error"] = (
                        f"the game ended itself while starting: {ended['what']} "
                        f"({ended['file']})"
                    )
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
        if self.instance:
            answer["instance"] = self.instance
        if seeded:
            answer["seeded"] = seeded
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
                    "POST", "/api/tool/saves", {"action": "load", **wanted}, timeout=30
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
                "POST", "/api/tool/saves", {"action": "force_load"}, timeout=30
            )
        except (Unreachable, StillWaiting):
            return False
        return status == 200 and isinstance(body, dict) and body.get("forced") is True

    def popup(self) -> dict | None:
        """The game's message box, if one is up, as gui reads it."""
        try:
            status, body = self.game.request("POST", "/api/tool/gui", {}, timeout=10)
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
                return reply(
                    text_result(
                        {
                            "count": len(saves),
                            "kinds": save_counts(saves),
                            "saves": saves,
                        }
                    )
                )
            if name == "game_status":
                return reply(text_result(self.game_status()))
            if name == "game_instance":
                return reply(self.game_instance(arguments))
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
    parser.add_argument(
        "--exe",
        type=Path,
        default=default_game(Path(__file__)),
        help="DAOrigins.exe; default: the game whose plugin folder this file is in, else "
        "the GOG install's",
    )
    parser.add_argument(
        "--instance",
        default=os.environ.get("ELUVIAN_INSTANCE", ""),
        help="the copy of the game this bridge drives from its start: an instance's name "
        "(DAOLib's DAOLIB_INSTANCE), or nothing for the game as ever. Default: "
        "ELUVIAN_INSTANCE. The game_instance tool changes it while the bridge runs",
    )
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

    instance = instance_name(options.instance)
    if instance is None:
        parser.error(
            f"--instance {options.instance!r}: 1 to 32 letters, digits, - or _"
        )

    def copy(name: str) -> tuple[Game, Processes, Path, Path]:
        """What a bridge has of one copy of the game: where it answers, its processes, the
        list of its tools kept beside its logs, and its saves."""
        folder = log_folder(name)
        processes = Processes(options.exe, name)
        # --port names the port of the copy the bridge was started for, not of every copy.
        port = options.port if name == instance else None
        return (
            Game(folder, port, processes.running),
            processes,
            folder / "eluvian.tools.json",
            folder.parent / "Characters",
        )

    game, processes, cache, characters = copy(instance)
    bridge = Bridge(
        game,
        processes,
        cache,
        send,
        characters=characters,
        copies=copy,
        default=profile_folder(""),
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
