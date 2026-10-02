"""Tests of eluvian_bridge.py against a stand-in for the game's REST API, on loopback."""

from __future__ import annotations

import json
import os
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
            self.answer(200, {"state": "running", "mode": "explore"})
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
        elif self.path == "/api/tool/game":
            # A load, as Eluvian's game tool answers it; no such save for "Nobody".
            if args.get("character") == "Nobody":
                self.answer(400, {"error": "game: no such save to load"})
            elif args.get("character") == "Modded":
                # Asked, and then the game's own dialog: no load follows.
                self.answer(200, {"asked": True, "save": args, "cursor": 8})
            else:
                self.answer(200, {"asked": True, "save": args, "cursor": 7})
        elif self.path == "/api/tool/inspect":
            box = {
                "text": "This saved game cannot be loaded.",
                "buttons": [{"label": "Force Load"}, {"label": "Cancel"}],
            }
            self.answer(200, {"popup": box if FakeGame.popup else None})
        elif self.path == "/api/tool/wait_for":
            event = {"seq": 9, "topic": args.get("topic"), "payload": {"number": 2}}
            self.answer(200, {"found": args.get("since") == 7, "event": event})
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
    def __init__(self, exe: Path, pids: list[int]) -> None:
        self.exe = exe
        self.pids = pids
        self.started = 0
        self.closed: list[tuple[int, bool]] = []
        self.other: list[dict] = []  # games of another install

    def running(self) -> list[int]:
        return list(self.pids)

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
                "game_saves",
                "game_claim",
                "game_release",
                "game_status",
                "game_launch",
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
        result = self.call("inspect", {"kind": "game"})
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
        # It does not stop this install's game from starting, and the answer names it.
        result = self.call("game_launch")
        self.assertFalse(result["isError"])
        self.assertEqual(self.processes.started, 1)
        self.assertEqual(result["structuredContent"]["otherInstalls"][0]["pid"], 7)

    def test_a_process_is_this_installs_by_its_file_not_its_name(self) -> None:
        exe = Path(r"C:\Games\GOG\bin_ship\DAOrigins.exe")
        self.assertTrue(
            bridge_module.is_same_file(r"c:\games\gog\bin_ship\daorigins.exe", exe)
        )
        self.assertFalse(
            bridge_module.is_same_file(r"C:\Games\EA\bin_ship\daorigins.exe", exe)
        )
        self.assertFalse(bridge_module.is_same_file(None, exe))

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
        self.assertGreater(
            bridge_module.call_budget("travel", {"budgetMs": 120000}), 120
        )
        self.assertGreater(bridge_module.call_budget("travel", {}), 60)

    def test_a_claim_keeps_another_from_launching_until_it_is_released(self) -> None:
        self.processes.pids.clear()
        claimed = self.call(
            "game_claim", {"owner": "tester-a", "note": "a parity test"}
        )
        self.assertTrue(claimed["structuredContent"]["claimed"])
        self.assertEqual(
            self.call("game_status")["structuredContent"]["claim"]["owner"], "tester-a"
        )
        refused = self.call("game_launch", {"owner": "tester-b"})
        self.assertTrue(refused["isError"])
        self.assertIn("claimed by tester-a", refused["content"][0]["text"])
        self.assertIn("a parity test", refused["content"][0]["text"])
        self.assertEqual(self.processes.started, 0)
        self.assertTrue(self.call("game_claim", {"owner": "tester-b"})["isError"])
        self.assertTrue(self.call("game_release", {"owner": "tester-b"})["isError"])
        # Its owner launches, and releases.
        self.assertFalse(self.call("game_launch", {"owner": "tester-a"})["isError"])
        self.assertEqual(self.processes.started, 1)
        released = self.call("game_release", {"owner": "tester-a"})
        self.assertTrue(released["structuredContent"]["released"])
        self.assertNotIn("claim", self.call("game_status")["structuredContent"])

    def test_a_claim_past_its_time_no_longer_stands(self) -> None:
        self.processes.pids.clear()
        path = self.folder / "eluvian.claim.json"
        path.write_text(
            json.dumps({"owner": "gone", "note": "", "until": time.time() - 5}),
            encoding="utf-8",
        )
        self.assertNotIn("claim", self.call("game_status")["structuredContent"])
        self.assertFalse(self.call("game_launch", {"owner": "tester-b"})["isError"])

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
        two_minutes = {"steps": [{"sleepMs": 60000}, {"sleepMs": 60000}]}
        self.assertGreater(bridge_module.call_budget("scenario", two_minutes), 600)
        self.assertEqual(
            bridge_module.call_budget("wait_for", {"timeoutMs": 120000}), 180
        )
        self.assertEqual(bridge_module.call_budget("inspect", {"kind": "game"}), 60)
        self.assertEqual(
            bridge_module.call_budget("inspect", ["not", "an", "object"]), 60
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
        # (2026-09-30): the first, still running, serves on the default.
        write("eluvian.json", 71156, 8931)
        self.assertEqual(
            Game(self.folder, running=lambda: [47408]).port(),
            bridge_module.DEFAULT_PORT,
        )
        # The game the file names is the one running: its port.
        self.assertEqual(Game(self.folder, running=lambda: [71156]).port(), 8931)
        # A second game keeps its own file, and is found by its pid.
        write("eluvian.json", 100, 8930)
        write("eluvian.200.json", 200, 8931)
        self.assertEqual(Game(self.folder, running=lambda: [200]).port(), 8931)
        self.assertEqual(Game(self.folder, running=lambda: [100, 200]).port(), 8930)
        # No game runs: the last one known, for when it comes back.
        self.assertEqual(Game(self.folder, running=list).port(), 8930)


if __name__ == "__main__":
    unittest.main()
