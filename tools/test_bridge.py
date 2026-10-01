"""Tests of eluvian_bridge.py against a stand-in for the game's REST API, on loopback."""

from __future__ import annotations

import json
import os
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import eluvian_bridge as bridge_module
from eluvian_bridge import Bridge, Game


class FakeGame(BaseHTTPRequestHandler):
    """Answers as Eluvian does: health, the tool list, and three tools."""

    revision = 1

    def log_message(self, *args) -> None:  # quiet
        pass

    def answer(self, status: int, body: object) -> None:
        data = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        if self.path == "/api/health":
            self.answer(200, {"state": "running", "mode": "explore"})
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

    def running(self) -> list[int]:
        return list(self.pids)

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
            names, ["game_saves", "game_status", "game_launch", "game_quit", "health"]
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

    def test_launch_starts_the_game_and_waits_until_it_serves(self) -> None:
        self.processes.pids.clear()
        result = self.call("game_launch", {"logLevel": "debug"})
        self.assertFalse(result["isError"])
        self.assertEqual(result["structuredContent"]["health"]["state"], "running")
        self.assertEqual(self.processes.environment, {"DAOLIB_LOG_LEVEL": "debug"})
        self.assertIn(
            {"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}, self.sent
        )

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

    def test_the_port_comes_from_eluvian_json(self) -> None:
        (self.folder / "eluvian.json").write_text(
            json.dumps({"port": 8931}), encoding="utf-8"
        )
        self.assertEqual(Game(self.folder).port(), 8931)
        self.assertEqual(
            Game(self.folder / "missing").port(), bridge_module.DEFAULT_PORT
        )


if __name__ == "__main__":
    unittest.main()
