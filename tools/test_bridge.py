"""Tests of eluvian_bridge.py against a stand-in for the game's REST API, on loopback."""

from __future__ import annotations

import json
import os
import re
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import eluvian_bridge as bridge_module
from eluvian_bridge import Bridge, Game


class FakeGame(BaseHTTPRequestHandler):
    """Answers as Eluvian does: health, the tool list, and three tools."""

    revision = 1
    popup = False  # whether the game's message box is up
    forced = False  # whether the game was told Force Load
    pid: int | None = None  # the process its health says it is, where a test needs one

    def log_message(self, *args) -> None:  # quiet
        pass

    def answer(self, status: int, body: object) -> None:
        data = json.dumps(body).encode("utf-8")
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except ConnectionError:
            pass  # the client stopped waiting, which one test is about

    def do_GET(self) -> None:
        if self.path == "/api/health":
            health = {"state": "running", "mode": "explore"}
            if FakeGame.pid is not None:
                health["pid"] = FakeGame.pid
            self.answer(200, health)
        elif self.path == "/api/slow":
            time.sleep(1.5)
            self.answer(200, {"late": True})
        elif self.path == "/api/tools":
            tools = [
                {
                    "name": "health",
                    "description": "Alive?",
                    "inputSchema": {"type": "object"},
                }
            ]
            self.answer(200, {"revision": FakeGame.revision, "tools": tools})
        else:
            self.answer(404, {"error": "no route"})

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", 0))
        args = json.loads(self.rfile.read(length) or b"{}")
        if self.path == "/api/tool/echo":
            self.answer(200, {"args": args})
        elif self.path == "/api/tool/saves":
            # A load, as Eluvian's saves tool answers it; no such save for "Nobody".
            if args.get("action") == "force_load":
                FakeGame.forced = True
                FakeGame.popup = False
                self.answer(200, {"forced": True, "cursor": 8})
            elif args.get("character") == "Nobody":
                self.answer(400, {"error": "saves: no such save to load"})
            elif args.get("character") == "Modded":
                # Asked, and then the game's own dialog: no load follows.
                self.answer(200, {"asked": True, "save": args, "cursor": 8})
            else:
                self.answer(200, {"asked": True, "save": args, "cursor": 7})
        elif self.path == "/api/tool/gui":
            box = {
                "text": "This saved game cannot be loaded.",
                "buttons": [{"label": "Force Load"}, {"label": "Cancel"}],
            }
            self.answer(200, {"popup": box if FakeGame.popup else None})
        elif self.path == "/api/tool/wait_for":
            event = {"seq": 9, "topic": args.get("topic"), "payload": {"number": 2}}
            found = args.get("since") == 7 or (
                args.get("since") == 8 and FakeGame.forced
            )
            self.answer(200, {"found": found, "event": event})
        elif self.path == "/api/tool/refuse":
            self.answer(400, {"error": "refuse: not now"})
        elif self.path == "/api/tool/picture":
            self.answer(
                200,
                {
                    "data": {"width": 2},
                    "content": [
                        {"type": "image", "data": "AA", "mimeType": "image/png"}
                    ],
                },
            )
        else:
            self.answer(404, {"error": "no tool"})


class FakeProcesses:
    def __init__(self, exe: Path, pids: list[int], instance: str = "") -> None:
        self.exe = exe
        self.pids = pids
        self.instance = instance  # which copy of the game these are the processes of
        self.started = 0
        self.closed: list[tuple[int, bool]] = []
        self.other: list[dict] = []  # games of another install
        self.beside: list[dict] = []  # the install's other copies

    def running(self) -> list[int]:
        return list(self.pids)

    def copies(self) -> list[dict]:
        return list(self.beside)

    def others(self) -> list[dict]:
        return list(self.other)

    def start(self, environment: dict[str, str]) -> int:
        self.started += 1
        self.environment = environment
        self.pids.append(4242)
        return 4242

    def close(self, pid: int, force: bool) -> None:
        self.closed.append((pid, force))
        self.pids.remove(pid)


def request(request_id: int, method: str, params: dict | None = None) -> dict:
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": method,
        "params": params or {},
    }


class BridgeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), FakeGame)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.folder = Path(tempfile.mkdtemp())
        self.exe = self.folder / "DAOrigins.exe"
        self.exe.write_bytes(b"")
        self.sent: list[dict] = []
        self.processes = FakeProcesses(self.exe, [100])
        self.bridge = Bridge(
            Game(self.folder, self.server.server_address[1]),
            self.processes,
            self.folder / "eluvian.tools.json",
            self.sent.append,
            sleep=lambda _: None,
        )

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()

    def down(self) -> None:
        """The game stops answering."""
        self.server.shutdown()
        self.server.server_close()
        self.bridge.game.fixed_port = 1  # nothing listens there

    def call(self, name: str, arguments: dict | None = None) -> dict:
        reply = self.bridge.handle(
            request(1, "tools/call", {"name": name, "arguments": arguments or {}})
        )
        assert reply is not None
        return reply["result"]

    def test_initialize_answers_a_known_version_and_lists_changes(self) -> None:
        reply = self.bridge.handle(
            request(1, "initialize", {"protocolVersion": "2025-06-18"})
        )
        self.assertEqual(reply["result"]["protocolVersion"], "2025-06-18")
        self.assertTrue(reply["result"]["capabilities"]["tools"]["listChanged"])
        reply = self.bridge.handle(
            request(2, "initialize", {"protocolVersion": "1999-01-01"})
        )
        self.assertEqual(
            reply["result"]["protocolVersion"], bridge_module.PROTOCOL_VERSIONS[0]
        )

    def test_notifications_get_no_reply(self) -> None:
        self.assertIsNone(
            self.bridge.handle(
                {"jsonrpc": "2.0", "method": "notifications/initialized"}
            )
        )

    def test_tools_are_the_bridges_and_the_games(self) -> None:
        reply = self.bridge.handle(request(1, "tools/list"))
        names = [tool["name"] for tool in reply["result"]["tools"]]
        self.assertEqual(
            names,
            [
                "game_events",
                "game_saves",
                "game_status",
                "game_launch",
                "game_instance",
                "game_quit",
                "health",
            ],
        )

    def test_the_games_tools_are_kept_for_while_it_is_down(self) -> None:
        self.bridge.handle(request(1, "tools/list"))
        self.down()
        reply = self.bridge.handle(request(2, "tools/list"))
        self.assertIn("health", [tool["name"] for tool in reply["result"]["tools"]])

    def test_a_call_goes_to_the_game_and_comes_back_as_structured_content(self) -> None:
        result = self.call("echo", {"id": "0x7d9"})
        self.assertFalse(result["isError"])
        self.assertEqual(result["structuredContent"], {"args": {"id": "0x7d9"}})
        self.assertEqual(
            json.loads(result["content"][0]["text"]), {"args": {"id": "0x7d9"}}
        )

    def test_the_games_refusal_is_an_error_result_with_its_sentence(self) -> None:
        result = self.call("refuse")
        self.assertTrue(result["isError"])
        self.assertEqual(result["content"][0]["text"], "refuse: not now")

    def test_extra_content_follows_the_text(self) -> None:
        result = self.call("picture")
        self.assertEqual(
            [item["type"] for item in result["content"]], ["text", "image"]
        )
        self.assertEqual(result["structuredContent"], {"width": 2})

    def test_a_call_while_the_game_is_down_says_how_to_start_it(self) -> None:
        self.down()
        result = self.call("state", {})
        self.assertTrue(result["isError"])
        self.assertIn("game_launch", result["content"][0]["text"])

    def test_status_says_running_and_what_health_says(self) -> None:
        result = self.call("game_status")["structuredContent"]
        self.assertEqual(result["pids"], [100])
        self.assertEqual(result["eluvian"]["mode"], "explore")

    def test_launch_never_starts_a_second_game(self) -> None:
        result = self.call("game_launch")["structuredContent"]
        self.assertFalse(result["launched"])
        self.assertEqual(self.processes.started, 0)

    def test_a_launch_names_its_owner_and_a_second_is_told_whose_the_game_is(
        self,
    ) -> None:
        self.processes.pids.clear()
        self.call("game_launch", {"owner": "tester-a"})
        self.assertEqual(self.processes.environment["ELUVIAN_OWNER"], "tester-a")
        second = self.call("game_launch", {"owner": "tester-b"})
        self.assertFalse(second["structuredContent"]["launched"])
        self.assertIn("NOT launched", second["structuredContent"]["note"])
        self.assertEqual(self.processes.started, 1)

    def test_launch_starts_the_game_and_waits_until_it_serves(self) -> None:
        self.processes.pids.clear()
        result = self.call("game_launch", {"logLevel": "debug"})
        self.assertFalse(result["isError"])
        self.assertEqual(result["structuredContent"]["health"]["state"], "running")
        self.assertEqual(
            self.processes.environment,
            {"DAOLIB_LOG_LEVEL": "debug", "ELUVIAN_OWNER": ""},
        )
        self.assertIn(
            {"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}, self.sent
        )

    def refused_game(self) -> None:
        """A game whose loader is of another plugin API than Eluvian: both logs say so."""
        (self.folder / "loader.log").write_text(
            "[19:20:01.001] [62804] [info] entry point reached; loading plugins\n"
            "[19:20:01.004] [62804] [info] eluvian.dll: refused to load\n"
            "[19:20:01.009] [62804] [info] 1 plugin(s) in C:\\game\\daolib\\plugins\n",
            encoding="utf-8",
        )
        (self.folder / "eluvian.log").write_text(
            "[19:20:01.002] [62804] [info] eluvian 0.2.0\n"
            "[19:20:01.003] [62804] [error] the loader's plugin API is 1, not the 2 "
            "Eluvian was built for; Eluvian stays off\n",
            encoding="utf-8",
        )

    def test_a_launch_eluvian_refused_says_so_in_the_logs_words(self) -> None:
        self.down()
        self.processes.pids.clear()
        start = self.processes.start

        def start_and_log(environment: dict[str, str]) -> int:
            self.refused_game()
            return start(environment)

        self.processes.start = start_and_log
        result = self.call("game_launch", {"waitSeconds": 0})
        self.assertTrue(result["isError"])
        self.assertIn(
            "eluvian.log: the loader's plugin API is 1", result["content"][0]["text"]
        )

    def test_a_launch_is_not_answered_from_an_older_games_logs(self) -> None:
        self.down()
        self.processes.pids.clear()
        self.refused_game()
        long_ago = time.time() - 3600
        for name in ("loader.log", "eluvian.log"):
            os.utime(self.folder / name, (long_ago, long_ago))
        result = self.call("game_launch", {"waitSeconds": 0})
        self.assertFalse(result["isError"])
        self.assertNotIn("error", result["structuredContent"])

    def test_status_says_why_a_running_game_does_not_serve(self) -> None:
        self.down()
        (self.folder / "loader.log").write_text(
            "[19:20:01.002] [62804] [error] the address library has no file for this build "
            "of the game; plugins are not loaded\n",
            encoding="utf-8",
        )
        result = self.call("game_status")["structuredContent"]
        self.assertEqual(result["eluvian"], "not answering")
        self.assertTrue(result["why"].startswith("loader.log: the address library"))

    def test_status_says_when_the_loader_found_no_eluvian(self) -> None:
        self.down()
        (self.folder / "loader.log").write_text(
            "[19:20:01.004] [62804] [info] turnbased.dll: loaded\n"
            "[19:20:01.009] [62804] [info] 1 plugin(s) in C:\\game\\daolib\\plugins\n",
            encoding="utf-8",
        )
        result = self.call("game_status")["structuredContent"]
        self.assertIn("no eluvian.dll", result["why"])

    def test_status_says_when_eluvian_was_left_loose_beside_the_folders(self) -> None:
        self.down()
        (self.folder / "loader.log").write_text(
            "[19:20:01.003] [62804] [warning] eluvian.dll: not loaded; a plugin goes in a "
            "folder of its own, daolib\\plugins\\eluvian\\eluvian.dll\n"
            "[19:20:01.009] [62804] [info] 1 plugin(s) in C:\\game\\daolib\\plugins\n",
            encoding="utf-8",
        )
        result = self.call("game_status")["structuredContent"]
        self.assertIn("a plugin goes in a folder of its own", result["why"])

    def test_launch_into_a_save_asks_for_it_and_waits_for_the_world(self) -> None:
        self.processes.pids.clear()
        save = {"character": "Aedan", "kind": "manual", "number": 2}
        result = self.call("game_launch", {"load": save})
        self.assertFalse(result["isError"])
        loaded = result["structuredContent"]["loaded"]
        self.assertEqual(loaded["save"], {"action": "load", **save})
        self.assertEqual(loaded["event"]["topic"], "eluvian.game.loaded")

    def test_a_save_the_game_does_not_take_is_an_error_that_says_why(self) -> None:
        self.processes.pids.clear()
        result = self.call(
            "game_launch", {"load": {"character": "Nobody"}, "waitSeconds": 0}
        )
        self.assertTrue(result["isError"])
        self.assertIn("no such save to load", result["content"][0]["text"])
        self.assertEqual(self.processes.started, 1)  # the game was started all the same

    def test_a_load_answered_with_a_message_box_gives_its_text_at_once(self) -> None:
        self.processes.pids.clear()
        FakeGame.popup = True
        self.addCleanup(setattr, FakeGame, "popup", False)
        result = self.call(
            "game_launch", {"load": {"character": "Modded"}, "waitSeconds": 300}
        )
        self.assertTrue(result["isError"])
        text = result["content"][0]["text"]
        self.assertIn("This saved game cannot be loaded.", text)
        self.assertIn("Force Load, Cancel", text)

    def test_force_load_is_pressed_only_when_asked_and_the_game_is_marked(self) -> None:
        self.processes.pids.clear()
        FakeGame.popup = True
        self.addCleanup(setattr, FakeGame, "popup", False)
        self.addCleanup(setattr, FakeGame, "forced", False)
        result = self.call(
            "game_launch",
            {"load": {"character": "Modded", "forceLoad": True}, "waitSeconds": 300},
        )
        self.assertFalse(result["isError"])
        loaded = result["structuredContent"]["loaded"]
        self.assertTrue(loaded["forceLoaded"])
        self.assertIn("never saved", loaded["note"])
        self.assertTrue(FakeGame.forced)

    def test_a_load_the_game_stops_at_a_dialog_is_told_not_waited_out(self) -> None:
        self.processes.pids.clear()
        self.bridge.load_patience = 0
        result = self.call(
            "game_launch", {"load": {"character": "Modded"}, "waitSeconds": 300}
        )
        self.assertTrue(result["isError"])
        text = result["content"][0]["text"]
        self.assertIn("showing a dialog", text)
        self.assertIn("Do not Force Load", text)

    def test_another_installs_game_is_neither_joined_nor_closed(self) -> None:
        self.processes.pids.clear()
        self.processes.other.append({"pid": 7, "exe": r"C:\EA\bin_ship\daorigins.exe"})
        status = self.call("game_status")["structuredContent"]
        self.assertFalse(status["running"])
        self.assertEqual(status["otherInstalls"][0]["pid"], 7)
        self.assertFalse(self.call("game_quit")["structuredContent"]["quit"])
        self.assertEqual(self.processes.closed, [])
        # Nor is a game of this install started beside it: one game runs at a time.
        result = self.call("game_launch")["structuredContent"]
        self.assertFalse(result["launched"])
        self.assertIn("one game runs at a time", result["note"])
        self.assertEqual(result["otherInstalls"][0]["pid"], 7)
        self.assertEqual(self.processes.started, 0)

    def test_a_process_is_this_installs_by_its_file_not_its_name(self) -> None:
        exe = Path(r"C:\Games\GOG\bin_ship\DAOrigins.exe")
        self.assertTrue(
            bridge_module.is_same_file(r"c:\games\gog\bin_ship\daorigins.exe", exe)
        )
        self.assertFalse(
            bridge_module.is_same_file(r"C:\Games\EA\bin_ship\daorigins.exe", exe)
        )
        self.assertFalse(bridge_module.is_same_file(None, exe))

    def test_a_bridge_in_a_games_plugin_folder_takes_that_game(self) -> None:
        game = self.folder / "bin_ship"
        plugin = game / "daolib" / "plugins" / "eluvian"
        plugin.mkdir(parents=True)
        script = plugin / "eluvian_bridge.py"
        # A folder laid out as a game's with no game in it, as a build tree's is.
        self.assertEqual(bridge_module.default_game(script), bridge_module.DEFAULT_GAME)
        (game / "DAOrigins.exe").write_bytes(b"")
        self.assertEqual(
            bridge_module.default_game(script), (game / "DAOrigins.exe").resolve()
        )
        # A game three folders up is not this bridge's unless the folders are a plugin's.
        elsewhere = game / "tools" / "of" / "mine"
        elsewhere.mkdir(parents=True)
        self.assertEqual(
            bridge_module.default_game(elsewhere / "eluvian_bridge.py"),
            bridge_module.DEFAULT_GAME,
        )

    def test_the_bridges_version_is_the_projects(self) -> None:
        cmake = Path(__file__).resolve().parent.parent / "CMakeLists.txt"
        version = re.search(
            r"project\(\w+ VERSION (\d+\.\d+\.\d+)", cmake.read_text(encoding="utf-8")
        )
        self.assertIsNotNone(version)
        self.assertEqual(bridge_module.VERSION, version.group(1))

    def test_one_call_from_the_command_line_prints_its_answer(self) -> None:
        import io

        out = io.StringIO()
        code = bridge_module.call_once(self.bridge, ["game_status"], out)
        self.assertEqual(code, 0)
        printed = json.loads(out.getvalue())
        self.assertFalse(printed["isError"])
        self.assertEqual(printed["answer"]["pids"], [100])
        out = io.StringIO()
        self.assertEqual(bridge_module.call_once(self.bridge, ["refuse"], out), 1)
        self.assertIn("not now", json.loads(out.getvalue())["answer"]["text"])
        out = io.StringIO()
        self.assertEqual(
            bridge_module.call_once(self.bridge, ["echo", "{not json"], out), 2
        )

    def test_a_journey_is_waited_for_as_long_as_it_may_take(self) -> None:
        travel = {"argument": "budgetMs", "defaultMs": 60000}
        self.assertGreater(bridge_module.call_budget({"budgetMs": 120000}, travel), 120)
        self.assertGreater(bridge_module.call_budget({}, travel), 60)

    def test_the_last_runs_events_are_read_from_its_file(self) -> None:
        self.down()
        self.processes.pids.clear()
        lines = [
            {"seq": 1, "topic": "eluvian.mode.changed", "payload": {"mode": "combat"}},
            {"seq": 2, "topic": "turnbased.turn.ended", "payload": {"round": 1}},
            {"seq": 3, "topic": "eluvian.creature.died", "payload": {"id": "0x3bf"}},
            {"seq": 4, "topic": "turnbased.turn.ended", "payload": {"round": 2}},
        ]
        (self.folder / "eluvian.events.jsonl").write_text(
            "".join(json.dumps(line) + "\n" for line in lines) + '{"seq": 5, "top',
            encoding="utf-8",
        )
        everything = self.call("game_events")["structuredContent"]
        self.assertEqual([e["seq"] for e in everything["events"]], [1, 2, 3, 4])
        self.assertFalse(everything["running"])
        turns = self.call("game_events", {"topic": "turnbased.*"})["structuredContent"]
        self.assertEqual([e["payload"]["round"] for e in turns["events"]], [1, 2])
        later = self.call("game_events", {"since": 3, "limit": 1})["structuredContent"]
        self.assertEqual([e["seq"] for e in later["events"]], [3])
        self.assertEqual(later["next"], 4)
        self.assertEqual(later["count"], 2)

    def test_no_events_file_says_where_it_looked(self) -> None:
        result = self.call("game_events")
        self.assertTrue(result["isError"])
        self.assertIn("eluvian.events.jsonl", result["content"][0]["text"])

    def test_the_run_befores_events_are_read_with_previous(self) -> None:
        # A launch after a crash keeps the crash's run beside the new one (#74).
        (self.folder / "eluvian.events.jsonl").write_text(
            json.dumps({"seq": 1, "topic": "now"}) + "\n", encoding="utf-8"
        )
        missing = self.call("game_events", {"previous": True})
        self.assertTrue(missing["isError"])
        self.assertIn("eluvian.events.previous.jsonl", missing["content"][0]["text"])
        (self.folder / "eluvian.events.previous.jsonl").write_text(
            json.dumps({"seq": 7, "topic": "before"}) + "\n", encoding="utf-8"
        )
        before = self.call("game_events", {"previous": True})["structuredContent"]
        self.assertEqual([e["topic"] for e in before["events"]], ["before"])
        now = self.call("game_events")["structuredContent"]
        self.assertEqual([e["topic"] for e in now["events"]], ["now"])

    def run_of(self, name: str, topic: str, age: float = 0) -> None:
        """A run's events file, written `age` seconds ago."""
        file = self.folder / name
        file.write_text(json.dumps({"seq": 1, "topic": topic}) + "\n", encoding="utf-8")
        then = time.time() - age
        os.utime(file, (then, then))

    def topics(self, arguments: dict | None = None) -> list[str]:
        read = self.call("game_events", arguments)["structuredContent"]
        return [e["topic"] for e in read["events"]]

    def test_a_game_on_a_second_port_is_read_from_its_own_events_file(self) -> None:
        # A game that found the first port taken keeps eluvian.<pid>.events.jsonl, and the
        # plain file beside it is an older run's: that one was answered, `running: true`
        # (kadan-agent, 2026-10-12, #121).
        self.run_of("eluvian.events.jsonl", "hours before", age=3600)
        self.run_of("eluvian.100.events.jsonl", "live")
        self.run_of("eluvian.200.events.jsonl", "another's")
        (self.folder / "eluvian.json").write_text(
            json.dumps({"pid": 71156, "port": 8930}), encoding="utf-8"
        )
        read = self.call("game_events")["structuredContent"]
        self.assertEqual([e["topic"] for e in read["events"]], ["live"])
        self.assertTrue(read["file"].endswith("eluvian.100.events.jsonl"))
        # Its run before, where the game kept one, is beside it under its name.
        missing = self.call("game_events", {"previous": True})
        self.assertTrue(missing["isError"])
        self.assertIn(
            "eluvian.100.events.previous.jsonl", missing["content"][0]["text"]
        )
        self.run_of("eluvian.100.events.previous.jsonl", "before")
        self.assertEqual(self.topics({"previous": True}), ["before"])
        # The game eluvian.json names is the one served while it runs, as its port is.
        self.processes.pids.append(71156)
        self.assertEqual(self.topics(), ["hours before"])

    def test_with_no_game_running_the_run_written_last_is_read(self) -> None:
        self.down()
        self.processes.pids.clear()
        self.run_of("eluvian.events.jsonl", "the run before last", age=3600)
        self.run_of("eluvian.300.events.jsonl", "older still", age=7200)
        self.run_of("eluvian.200.events.jsonl", "the last run", age=60)
        # Not a run's file, however lately written.
        self.run_of("eluvian.200.events.previous.jsonl", "a run before")
        self.run_of("eluvian.notes.events.jsonl", "not a game's")
        self.assertEqual(self.topics(), ["the last run"])
        self.run_of("eluvian.events.jsonl", "a game since")
        self.assertEqual(self.topics(), ["a game since"])

    def test_a_game_that_crashed_starting_is_answered_with_its_crash_log(self) -> None:
        self.down()
        self.processes.pids.clear()
        start = self.processes.start

        def start_and_crash(environment: dict[str, str]) -> int:
            pid = start(environment)
            (self.folder / "crash-2026-10-02-08-03-24.log").write_text(
                "Dragon Age: Origins, DAOrigins.exe link stamp 0x4eb19d53\n"
                'Unhandled exception "EXCEPTION_ACCESS_VIOLATION" (0xc0000005) at 0x00a53690\n'
                "PROBABLE CALL STACK:\n"
                "\t[ 0] 0x00a53690 DAOrigins.exe Graphics::MaterialTechniqueTemp::vfunction6+0x0\n"
                "\t[ 1] 0x00468ddb DAOrigins.exe Renderer::MoviePlayerInternal::MoviePlayerInternal+0xdb\n",
                encoding="utf-8",
            )
            self.processes.pids.clear()  # and it is gone
            return pid

        self.processes.start = start_and_crash
        result = self.call("game_launch", {"waitSeconds": 5})
        self.assertTrue(result["isError"])
        text = result["content"][0]["text"]
        self.assertIn("EXCEPTION_ACCESS_VIOLATION", text)
        self.assertIn("renderer as it starts", text)

    def test_a_game_that_ended_itself_starting_is_answered_with_its_exit_log(
        self,
    ) -> None:
        self.down()
        self.processes.pids.clear()
        start = self.processes.start

        def start_and_end(environment: dict[str, str]) -> int:
            pid = start(environment)
            # As DAOLib's loader writes one (lib-agent, 2026-10-09): the build, the logger,
            # the call, the system, the call stack whose frame 0 is the logger's own hook,
            # and the return addresses on the stack.
            (self.folder / "exit-2026-10-09-14-02-11.log").write_text(
                "Dragon Age: Origins, DAOrigins.exe link stamp 0x4eb19d53\n"
                "DAOLib exit logger\n"
                "The game called ExitProcess with code 4294967295 (0xffffffff) before its first "
                "frame: it ended itself, with no crash\n"
                "SYSTEM: Windows 10\n"
                "PROBABLE CALL STACK:\n"
                "\t[ 0] 0x10001234 version.dll ExitHook+0x0\n"
                "\t[ 1] 0x008edb57 DAOrigins.exe 0x008edb57\n"
                "\t[ 2] 0x008edd96 DAOrigins.exe 0x008edd96 _exit+0xd\n"
                "\n"
                "RETURN ADDRESSES ON THE STACK:\n"
                "\t[ 0] 0x008e9f2a DAOrigins.exe ___tmainCRTStartup+0x1a3\n",
                encoding="utf-8",
            )
            self.processes.pids.clear()  # and it is gone
            return pid

        self.processes.start = start_and_end
        result = self.call("game_launch", {"waitSeconds": 5})
        self.assertTrue(result["isError"])
        text = result["content"][0]["text"]
        self.assertIn("ended itself while starting", text)
        self.assertIn("exit-2026-10-09-14-02-11.log", text)
        self.assertIn("The game called ExitProcess with code 4294967295", text)
        ended = json.loads(text)["exit"]
        self.assertEqual(
            [frame.split()[1] for frame in ended["frames"]], ["1]", "2]"]
        )  # the logger's own frame 0 left out
        self.assertIn("___tmainCRTStartup", ended["returns"][0])
        self.assertNotIn("Windows 10", text)

    def test_an_install_that_lost_its_data_is_not_launched_and_says_so(self) -> None:
        self.processes.pids.clear()
        game = self.folder / "Dragon Age Origins"
        (game / "bin_ship").mkdir(parents=True)
        (game / "packages" / "core" / "data").mkdir(parents=True)
        (game / "modules" / "single player" / "data").mkdir(parents=True)
        exe = game / "bin_ship" / "DAOrigins.exe"
        exe.write_bytes(b"")
        self.processes.exe = exe
        refused = self.call("game_launch", {"waitSeconds": 0})
        self.assertTrue(refused["isError"])
        self.assertIn("lost its game data", refused["content"][0]["text"])
        self.assertEqual(self.processes.started, 0)
        self.assertIn(
            "missing", self.call("game_status")["structuredContent"]["install"]
        )
        for name in ("packages/core/data/2da.erf", "modules/single player/data/a.erf"):
            (game / name).write_bytes(b"")
        self.assertNotIn("install", self.call("game_status")["structuredContent"])

    def test_quit_closes_each_game(self) -> None:
        result = self.call("game_quit", {"force": True})
        self.assertTrue(result["structuredContent"]["quit"])
        self.assertEqual(self.processes.closed, [(100, True)])
        result = self.call("game_quit")
        self.assertFalse(result["structuredContent"]["quit"])

    def test_a_new_revision_of_the_games_tools_tells_the_client(self) -> None:
        self.bridge.handle(request(1, "tools/list"))
        FakeGame.revision = 2
        try:
            self.bridge.handle(request(2, "tools/list"))
        finally:
            FakeGame.revision = 1
        self.assertIn(
            {"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}, self.sent
        )

    def test_saves_are_listed_from_their_folders_newest_first(self) -> None:
        characters = self.folder / "Characters"
        for character, slot, name, when in (
            ("Aedan", "Slot_2", "CastleCousland", 1_700_000_200),
            ("Aedan", "QuickSave_1", "Ostagar", 1_700_000_300),
            ("Lyna", "AutoSave_3", "Dalish", 1_700_000_100),
        ):
            folder = characters / character / "Saves" / slot
            folder.mkdir(parents=True)
            das = folder / f"{name}.das"
            das.write_bytes(b"")
            os.utime(das, (when, when))
        (characters / "Aedan" / "Saves" / "Notes").mkdir()  # not a slot
        (characters / "Aedan" / "Saves" / "Slot_3").mkdir()  # no save in it
        self.bridge.characters = characters

        saves = self.call("game_saves")["structuredContent"]["saves"]
        self.assertEqual(
            [(s["character"], s["kind"], s["number"], s["name"]) for s in saves],
            [
                ("Aedan", "quick", 1, "Ostagar"),
                ("Aedan", "manual", 2, "CastleCousland"),
                ("Lyna", "auto", 3, "Dalish"),
            ],
        )
        only = self.call("game_saves", {"character": "lyna"})["structuredContent"]
        self.assertEqual(only["count"], 1)
        kinds = self.call("game_saves")["structuredContent"]["kinds"]
        self.assertEqual(
            kinds["Aedan"]["manual"], {"count": 1, "most": 500, "full": False}
        )
        self.assertEqual(kinds["Aedan"]["quick"], {"count": 1, "most": 1, "full": True})
        self.assertEqual(kinds["Lyna"]["auto"]["count"], 1)
        self.bridge.characters = self.folder / "nowhere"
        self.assertEqual(self.call("game_saves")["structuredContent"]["count"], 0)

    def test_input_that_ends_still_gets_every_answer(self) -> None:
        lines = [
            json.dumps(request(1, "ping")),
            json.dumps(
                request(2, "tools/call", {"name": "echo", "arguments": {"n": 2}})
            ),
            json.dumps(request(3, "tools/call", {"name": "game_status"})),
        ]
        bridge_module.serve(self.bridge, lines)
        self.assertEqual(
            sorted(reply["id"] for reply in self.sent if "id" in reply), [1, 2, 3]
        )

    def test_the_bridge_waits_as_long_as_the_tool_may_take(self) -> None:
        scenario = {"argument": "budgetMs", "defaultMs": 600000}
        two_minutes = {"steps": [{"sleepMs": 60000}, {"sleepMs": 60000}]}
        self.assertGreater(bridge_module.call_budget(two_minutes, scenario), 600)
        wait_for = {"argument": "timeoutMs", "defaultMs": 30000}
        self.assertEqual(
            bridge_module.call_budget({"timeoutMs": 120000}, wait_for),
            120 + bridge_module.MARGIN_SECONDS,
        )
        self.assertEqual(bridge_module.call_budget({}, None), 60)
        self.assertEqual(
            bridge_module.call_budget(["not", "an", "object"], wait_for),
            30 + bridge_module.MARGIN_SECONDS,
        )

    def test_a_tools_wait_comes_from_its_own_listing_not_the_bridge(self) -> None:
        # A tool the bridge has never heard of, declaring a five-minute budget in its listing:
        # its wait is the listing's, with no name of it in the bridge (#108).
        self.bridge.game_tools = lambda: (
            self.bridge.note_budgets(
                [
                    {
                        "name": "sweep",
                        "_meta": {
                            "eluvian/budget": {
                                "argument": "budgetMs",
                                "defaultMs": 300000,
                            }
                        },
                    },
                    {"name": "state"},
                ]
            )
            or []
        )
        self.assertEqual(self.bridge.budget_of("sweep")["defaultMs"], 300000)
        self.assertIsNone(self.bridge.budget_of("state"))
        self.assertGreater(
            bridge_module.call_budget({}, self.bridge.budget_of("sweep")), 300
        )

    def test_a_call_still_running_is_not_reported_as_the_game_down(self) -> None:
        def still_waiting(*args, **kwargs):
            raise bridge_module.StillWaiting("timed out")

        self.bridge.game.request = still_waiting
        result = self.call("scenario", {"steps": [{"sleepMs": 1}]})
        self.assertTrue(result["isError"])
        text = result["content"][0]["text"]
        self.assertIn("may still be running", text)
        self.assertNotIn("game_launch", text)

    def test_a_read_that_times_out_is_still_waiting_not_unreachable(self) -> None:
        with self.assertRaises(bridge_module.StillWaiting):
            self.bridge.game.request("GET", "/api/slow", timeout=0.2)

    def test_two_launches_at_once_start_one_game(self) -> None:
        self.processes.pids.clear()
        looked = self.processes.running

        def slow_look() -> list[int]:
            pids = looked()
            time.sleep(
                0.1
            )  # long enough for the other request to look too, were it let
            return pids

        self.processes.running = slow_look
        threads = [
            threading.Thread(target=self.call, args=("game_launch", {"waitSeconds": 0}))
            for _ in range(2)
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(self.processes.started, 1)

    def test_malformed_requests_are_answered_with_their_id(self) -> None:
        reply = self.bridge.handle(
            {"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": ["bad"]}
        )
        self.assertEqual((reply["id"], reply["error"]["code"]), (7, -32602))
        reply = self.bridge.handle(
            request(8, "tools/call", {"name": "game_quit", "arguments": ["bad"]})
        )
        self.assertEqual((reply["id"], reply["error"]["code"]), (8, -32602))
        reply = self.bridge.handle(request(9, "initialize", None))
        self.assertIn("protocolVersion", reply["result"])

    def test_a_request_that_fails_inside_still_gets_an_answer(self) -> None:
        def broken(*args, **kwargs):
            raise RuntimeError("no such exe")

        self.processes.pids.clear()
        self.processes.start = broken
        line = json.dumps(request(11, "tools/call", {"name": "game_launch"}))
        bridge_module.serve(self.bridge, [line, json.dumps(request(12, "ping"))])
        by_id = {reply["id"]: reply for reply in self.sent if "id" in reply}
        self.assertEqual(by_id[11]["error"]["code"], -32603)
        self.assertIn("no such exe", by_id[11]["error"]["message"])
        self.assertEqual(by_id[12]["result"], {})

    def test_the_port_comes_from_eluvian_json(self) -> None:
        (self.folder / "eluvian.json").write_text(
            json.dumps({"port": 8931}), encoding="utf-8"
        )
        self.assertEqual(Game(self.folder).port(), 8931)
        self.assertEqual(
            Game(self.folder / "missing").port(), bridge_module.DEFAULT_PORT
        )

    def test_a_file_left_by_a_game_that_is_gone_does_not_name_the_port(self) -> None:
        def write(name: str, pid: int, port: int) -> None:
            (self.folder / name).write_text(
                json.dumps({"pid": pid, "port": port}), encoding="utf-8"
            )

        # A second game wrote its port over the first's, then went away
        # (2026-09-30): the first, still running, serves where it did. The file's port
        # is the first's only if the game answering there says it is that process.
        there = self.server.server_address[1]
        write("eluvian.json", 71156, there)
        self.addCleanup(setattr, FakeGame, "pid", None)
        FakeGame.pid = 47408
        self.assertEqual(Game(self.folder, running=lambda: [47408]).port(), there)
        # Another copy answering there is not this one's game: no port, and no call sent.
        FakeGame.pid = 555
        stranger = Game(self.folder, running=lambda: [47408])
        self.assertIsNone(stranger.port())
        with self.assertRaises(bridge_module.Unreachable):
            stranger.request("GET", "/api/health")
        # The game the file names is the one running: its port.
        write("eluvian.json", 71156, 8931)
        self.assertEqual(Game(self.folder, running=lambda: [71156]).port(), 8931)
        # A second game keeps its own file, and is found by its pid.
        write("eluvian.json", 100, 8930)
        write("eluvian.200.json", 200, 8931)
        self.assertEqual(Game(self.folder, running=lambda: [200]).port(), 8931)
        self.assertEqual(Game(self.folder, running=lambda: [100, 200]).port(), 8930)
        # No game of this copy runs: no port, whoever may listen on the one last known.
        self.assertIsNone(Game(self.folder, running=list).port())

    # ---- a second copy of the game beside the first (#116) -------------------

    def test_an_instances_name_is_taken_as_the_loader_takes_it(self) -> None:
        self.assertEqual(bridge_module.instance_name("Two"), "two")
        self.assertEqual(bridge_module.instance_name("a-b_9"), "a-b_9")
        self.assertEqual(bridge_module.instance_name(""), "")
        self.assertEqual(bridge_module.instance_name(None), "")
        for refused in ("two games", "a/b", "x" * 33, 7, "\u00e9"):
            self.assertIsNone(bridge_module.instance_name(refused))

    def test_an_instances_logs_are_under_its_own_profile(self) -> None:
        root = self.folder / "instances"
        self.assertEqual(
            bridge_module.log_folder("two", root),
            root / "two" / "BioWare" / "Dragon Age" / "daolib",
        )
        self.assertEqual(bridge_module.mutex_of("two"), "dragonage-two")
        self.assertEqual(bridge_module.mutex_of(""), "dragonage")

    def install_of(
        self, instance: str, pids: list[int], held: set[str]
    ) -> bridge_module.Processes:
        """The real Processes over a made-up install: `pids` are its games, `held` the
        mutexes that are there."""
        processes = bridge_module.Processes(
            self.exe,
            instance,
            self.folder / "instances",
            lambda name: name in held,
            self.folder / "as-ever",
        )
        processes.of_install = lambda: list(pids)
        return processes

    def record(self, instance: str, file: str, pid: int, age: float = 0) -> None:
        """What an instance's folder says of its process: Eluvian's file, or a bridge's."""
        logs = bridge_module.log_folder(instance, self.folder / "instances")
        logs.mkdir(parents=True, exist_ok=True)
        (logs / file).write_text(
            json.dumps({"pid": pid, "port": 8931}), encoding="utf-8"
        )
        if age:
            then = time.time() - age
            os.utime(logs / file, (then, then))

    def test_each_copys_processes_are_told_apart_by_what_its_folder_says(self) -> None:
        self.record("two", "eluvian.json", 200, age=600)
        held = {"dragonage", "dragonage-two"}
        self.assertEqual(self.install_of("", [100, 200], held).running(), [100])
        self.assertEqual(self.install_of("two", [100, 200], held).running(), [200])
        self.assertEqual(self.install_of("three", [100, 200], held).running(), [])
        self.assertEqual(
            self.install_of("", [100, 200], held).copies(),
            [{"instance": "two", "pid": 200}],
        )
        self.assertEqual(
            self.install_of("two", [100, 200], held).copies(),
            [{"instance": "", "pid": 100}],
        )

    def test_a_folder_whose_instance_is_gone_names_nobody(self) -> None:
        # What a run that ended left behind: its pid may be the game as ever's by now.
        self.record("two", "eluvian.json", 100, age=600)
        self.record("two", bridge_module.LAUNCH_FILE, 100, age=600)
        self.assertEqual(self.install_of("", [100], {"dragonage"}).running(), [100])
        self.assertEqual(self.install_of("two", [100], {"dragonage"}).running(), [])

    def test_a_copy_just_started_is_its_instances_before_its_mutex_is_there(
        self,
    ) -> None:
        self.record("two", bridge_module.LAUNCH_FILE, 200)
        self.assertEqual(
            self.install_of("two", [100, 200], {"dragonage"}).running(), [200]
        )
        self.assertEqual(
            self.install_of("", [100, 200], {"dragonage"}).running(), [100]
        )
        # Eluvian's own file is of some run, this one or one before: it says nothing until
        # the instance's mutex is there.
        self.record("three", "eluvian.json", 100)
        self.assertEqual(
            self.install_of("", [100, 200], {"dragonage"}).running(), [100]
        )

    def test_an_instance_that_names_no_process_is_not_taken_for_the_game_as_ever(
        self,
    ) -> None:
        # An instance started by hand with no Eluvian in it: its mutex, and no file.
        (self.folder / "instances" / "lib").mkdir(parents=True)
        held = {"dragonage-lib"}
        self.assertEqual(self.install_of("", [300], held).running(), [])
        self.assertEqual(
            self.install_of("", [300], held).copies(),
            [{"instance": "lib", "pid": None}],
        )
        # The game as ever beside it is the process its own folder names.
        as_ever = self.folder / "as-ever"
        as_ever.mkdir()
        (as_ever / "eluvian.json").write_text(
            json.dumps({"pid": 100, "port": 8930}), encoding="utf-8"
        )
        held = {"dragonage", "dragonage-lib"}
        self.assertEqual(self.install_of("", [100, 300], held).running(), [100])
        # Its file from a run long over says nothing once the game as ever is gone.
        self.assertEqual(self.install_of("", [100], {"dragonage-lib"}).running(), [])
        # One a bridge just started is its own before the mutex is there.
        (as_ever / bridge_module.LAUNCH_FILE).write_text(
            json.dumps({"pid": 400}), encoding="utf-8"
        )
        self.assertEqual(
            self.install_of("", [300, 400], {"dragonage-lib"}).running(), [400]
        )
        # With every instance's process named, the rest are the game as ever's, as before.
        self.record("lib", "eluvian.json", 300, age=600)
        self.assertEqual(
            self.install_of("", [300, 500], {"dragonage-lib"}).running(), [500]
        )

    def test_a_start_leaves_word_of_whose_the_process_is(self) -> None:
        self.processes.pids.clear()
        self.assertFalse(self.call("game_launch")["isError"])
        record = json.loads(
            (self.folder / bridge_module.LAUNCH_FILE).read_text(encoding="utf-8")
        )
        self.assertEqual(record, {"pid": 4242, "instance": ""})

    def default_profile(self) -> Path:
        """A default profile with settings, two characters and four saves."""
        profile = self.folder / "default" / "BioWare" / "Dragon Age"
        for part in ("Settings", "addins"):
            (profile / part).mkdir(parents=True)
            (profile / part / "a.xml").write_text(part, encoding="utf-8")
        for character, slots in (
            ("Lyna", ("Slot_4", "Slot_5", "AutoSave_1")),
            ("Other", ("Slot_1",)),
        ):
            for slot in slots:
                folder = profile / "Characters" / character / "Saves" / slot
                folder.mkdir(parents=True)
                (folder / "save.das").write_text(slot, encoding="utf-8")
            (profile / "Characters" / character / f"{character}.xml").write_text(
                "sheet", encoding="utf-8"
            )
        return profile

    def test_a_seeded_instance_has_the_settings_and_the_one_save(self) -> None:
        default = self.default_profile()
        root = self.folder / "instances"
        seeded = bridge_module.seed_instance(
            "two", {"character": "Lyna", "kind": "manual", "number": 4}, default, root
        )
        profile = root / "two" / "BioWare" / "Dragon Age"
        self.assertEqual(seeded["profile"], str(profile))
        self.assertEqual(
            (profile / "Settings" / "a.xml").read_text(encoding="utf-8"), "Settings"
        )
        self.assertTrue((profile / "addins" / "a.xml").is_file())
        saves = profile / "Characters" / "Lyna" / "Saves"
        self.assertEqual([folder.name for folder in saves.iterdir()], ["Slot_4"])
        self.assertTrue((profile / "Characters" / "Lyna" / "Lyna.xml").is_file())
        self.assertFalse((profile / "Characters" / "Other").exists())
        # The loader's own mark that nothing is left to copy.
        self.assertEqual(
            (root / "two" / "seeded").read_text(encoding="utf-8"), "seeded\n"
        )
        # An instance that has a folder is left as it is.
        (profile / "Settings" / "a.xml").write_text("its own", encoding="utf-8")
        self.assertIsNone(bridge_module.seed_instance("two", None, default, root))
        self.assertEqual(
            (profile / "Settings" / "a.xml").read_text(encoding="utf-8"), "its own"
        )

    def many(self, default: Path | None = None) -> Bridge:
        """A bridge that can change copies. The game as ever is this test's own; an instance
        has a folder of its own under `instances` and no game running."""
        root = self.folder / "instances"
        self.made: dict[str, FakeProcesses] = {}

        def copy(name: str) -> tuple[Game, FakeProcesses, Path, Path]:
            folder = bridge_module.log_folder(name, root) if name else self.folder
            processes = FakeProcesses(self.exe, [] if name else [100], name)
            self.made[name] = processes
            return (
                Game(folder, self.server.server_address[1]),
                processes,
                folder / "eluvian.tools.json",
                folder.parent / "Characters",
            )

        game, processes, cache, characters = copy("")
        self.bridge = Bridge(
            game,
            processes,
            cache,
            self.sent.append,
            sleep=lambda _: None,
            characters=characters,
            copies=copy,
            default=default,
        )
        return self.bridge

    def test_game_instance_says_which_copy_and_changes_it(self) -> None:
        self.many()
        first = self.call("game_instance")["structuredContent"]
        self.assertIsNone(first["instance"])
        self.assertEqual(first["folder"], str(self.folder))
        self.assertTrue(first["running"])
        second = self.call("game_instance", {"name": "Two"})["structuredContent"]
        self.assertEqual(second["instance"], "two")
        self.assertEqual(
            second["folder"],
            str(bridge_module.log_folder("two", self.folder / "instances")),
        )
        self.assertFalse(second["running"])
        self.assertIn(
            {"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}, self.sent
        )
        self.assertEqual(
            self.call("game_status")["structuredContent"]["instance"], "two"
        )
        back = self.call("game_instance", {"name": ""})["structuredContent"]
        self.assertIsNone(back["instance"])
        self.assertNotIn("instance", self.call("game_status")["structuredContent"])

    def test_game_instance_refuses_a_name_the_loader_would(self) -> None:
        self.many()
        self.assertTrue(self.call("game_instance", {"name": "two games"})["isError"])
        self.assertIsNone(self.call("game_instance")["structuredContent"]["instance"])

    def test_a_bridge_made_for_one_copy_keeps_it(self) -> None:
        refused = self.call("game_instance", {"name": "two"})
        self.assertTrue(refused["isError"])
        self.assertIn("one copy", refused["content"][0]["text"])
        # Asked for the copy it has, it only says so.
        self.assertFalse(self.call("game_instance", {"name": ""})["isError"])

    def test_an_instance_is_launched_as_one_beside_the_game_as_ever(self) -> None:
        self.many()
        self.call("game_instance", {"name": "two"})
        # Another install's game stops a start of the game as ever, not of an instance.
        self.made["two"].other.append(
            {"pid": 7, "exe": r"C:\EA\bin_ship\daorigins.exe"}
        )
        result = self.call("game_launch", {"owner": "tester-a"})
        self.assertFalse(result["isError"])
        answer = result["structuredContent"]
        self.assertTrue(answer["launched"])
        self.assertEqual(answer["instance"], "two")
        self.assertEqual(
            self.made["two"].environment,
            {"ELUVIAN_OWNER": "tester-a", "DAOLIB_INSTANCE": "two"},
        )
        self.assertEqual(self.made[""].started, 0)
        record = (
            bridge_module.log_folder("two", self.folder / "instances")
            / bridge_module.LAUNCH_FILE
        )
        self.assertEqual(json.loads(record.read_text(encoding="utf-8"))["pid"], 4242)

    def test_plugins_of_a_folder_are_asked_of_the_loader_and_must_hold_eluvian(
        self,
    ) -> None:
        self.processes.pids.clear()
        plugins = self.folder / "build" / "daolib" / "plugins"
        (plugins / "eluvian").mkdir(parents=True)
        refused = self.call("game_launch", {"plugins": str(plugins)})
        self.assertTrue(refused["isError"])
        self.assertIn("eluvian.dll", refused["content"][0]["text"])
        self.assertEqual(self.processes.started, 0)
        (plugins / "eluvian" / "eluvian.dll").write_bytes(b"")
        self.assertFalse(self.call("game_launch", {"plugins": str(plugins)})["isError"])
        self.assertEqual(
            self.processes.environment["DAOLIB_PLUGINS"], str(plugins.resolve())
        )

    def test_a_seed_of_one_save_is_an_instances(self) -> None:
        self.processes.pids.clear()
        refused = self.call("game_launch", {"seed": "save"})
        self.assertTrue(refused["isError"])
        self.assertIn("instance", refused["content"][0]["text"])
        self.assertTrue(self.call("game_launch", {"seed": "everything"})["isError"])
        self.assertEqual(self.processes.started, 0)

    def test_an_instance_seeded_with_a_save_is_given_it_before_its_start(self) -> None:
        default = self.default_profile()
        self.many(default)
        self.call("game_instance", {"name": "two"})
        save = {"character": "Lyna", "kind": "manual", "number": 4}
        answer = self.call("game_launch", {"seed": "save", "load": save})[
            "structuredContent"
        ]
        self.assertIn("Characters\\Lyna\\Saves\\Slot_4", answer["seeded"]["copied"])
        profile = self.folder / "instances" / "two" / "BioWare" / "Dragon Age"
        self.assertTrue(
            (
                profile / "Characters" / "Lyna" / "Saves" / "Slot_4" / "save.das"
            ).is_file()
        )
        self.assertFalse((profile / "Characters" / "Other").exists())
        self.assertEqual(self.made["two"].started, 1)

    def test_status_lists_the_other_copies_running(self) -> None:
        self.processes.beside = [{"instance": "two", "pid": 200}]
        status = self.call("game_status")["structuredContent"]
        self.assertEqual(status["copies"], [{"instance": "two", "pid": 200}])
        self.assertNotIn("instance", status)


if __name__ == "__main__":
    unittest.main()
