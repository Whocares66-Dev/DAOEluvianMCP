#!/usr/bin/env python3
"""Every Eluvian tool once, in the game: the check after a change that moves code.

    python tools/smoke.py --character Lyna --kind auto --number 2

src/game is verified by playing, so nothing in ctest calls it. This starts the game
into a save through the bridge under its own name, calls each tool once with an answer it
can judge, and closes the game. It loads only: it writes no save and says so if the game
wrote one. It spawns one wolf near the hero, kills it, and walks the hero four metres, so
run it on a save standing in open ground, away from doors and the plot's triggers (some of
them autosave). It will not drive a game that is not its own.

Exit 0 if every check passed, 1 if any failed, 2 if it could not start.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

import eluvian_bridge as bridge_module

OWNER = "eluvian-smoke"


class Run:
    def __init__(self, bridge: bridge_module.Bridge) -> None:
        self.bridge = bridge
        self.passed: list[str] = []
        self.failed: list[str] = []

    def tool(self, name: str, arguments: dict | None = None, timeout: float = 60):
        """One call of a game tool over REST: its answer, or {"error": ...} as it refused."""
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.bridge.game.port()}/api/tool/{name}",
            data=json.dumps(arguments or {}).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            try:
                return json.load(error)
            except ValueError:
                return {"error": f"HTTP {error.code}"}

    def check(self, name: str, answer: object, good) -> None:
        try:
            fine = bool(good(answer))
        except (KeyError, TypeError, IndexError, AttributeError):
            fine = False
        (self.passed if fine else self.failed).append(name)
        if not fine:
            print(f"FAIL {name}: {json.dumps(answer, ensure_ascii=False)[:400]}")

    def wait(self, topic: str, since: int, match: dict | None = None, ms: int = 12000):
        arguments = {"topic": topic, "since": since, "timeoutMs": ms}
        if match:
            arguments["match"] = match
        return self.tool("wait_for", arguments, timeout=ms / 1000 + 20)


def checks(run: Run) -> None:
    tool, check, wait = run.tool, run.check, run.wait
    health = tool("health")
    start = health.get("cursor", 0)
    check("health names this run as the owner", health, lambda a: a["owner"] == OWNER)

    game = tool("inspect", {"kind": "game"})
    check("inspect game", game, lambda a: a["hero"]["id"] and "popup" in a)
    hero = (game.get("hero") or {}).get("id")
    creatures = tool("inspect", {"kind": "creatures", "radius": 60})
    check(
        "inspect creatures",
        creatures,
        lambda a: (
            a["count"] >= 1 and {"room", "group", "side"} <= set(a["creatures"][0])
        ),
    )
    me = tool("inspect", {"kind": "creature", "id": hero})
    check("inspect creature", me, lambda a: "effects" in a and "abilities" in a)
    check(
        "inspect placeables",
        tool("inspect", {"kind": "placeables"}),
        lambda a: "placeables" in a,
    )
    check(
        "inspect dialogue",
        tool("inspect", {"kind": "dialogue"}),
        lambda a: a["active"] is False,
    )
    check(
        "inspect popup",
        tool("inspect", {"kind": "popup"}),
        lambda a: a["popup"] is None,
    )

    check(
        "script call",
        tool("script", {"action": "call", "command": "GetTag", "args": [hero]}),
        lambda a: isinstance(a["value"], str),
    )
    check(
        "script find",
        tool("script", {"action": "find", "command": "GroupHostility"}),
        lambda a: "error" not in a,
    )

    here = me.get("position", {"x": 0, "y": 0, "z": 0})
    point = [here["x"] - 4, here["y"], here["z"]]
    order = tool("order", {"action": "move", "id": hero, "point": point})
    check(
        "order move, and its end on the bus",
        wait(
            "eluvian.command.ended",
            order.get("cursor", 0),
            {"id": hero, "type": "MoveToLocation"},
        ),
        lambda a: a["found"],
    )

    back = [here["x"], here["y"], here["z"]]
    check(
        "travel, a walk back to where the hero stood",
        tool("travel", {"id": hero, "point": back, "budgetMs": 20000}, timeout=40),
        lambda a: a["arrived"] is True,
    )

    spawn = tool(
        "stage",
        {
            "action": "spawn",
            "template": "wolf",
            "point": [here["x"] - 12, here["y"], here["z"]],
        },
    )
    check("stage spawn", spawn, lambda a: a["creature"]["id"] and "moved" in a)
    wolf = (spawn.get("creature") or {}).get("id")
    check(
        "inspect geometry with the walk",
        tool("inspect", {"kind": "geometry", "id": hero, "target": wolf, "walk": True}),
        lambda a: "lineOfSight" in a and "found" in a["walk"],
    )
    check(
        "stage group",
        tool("stage", {"action": "group", "id": wolf, "group": 2}),
        lambda a: a["creature"]["group"] == 2,
    )
    check(
        "stage effect",
        tool("stage", {"action": "effect", "id": wolf, "effect": "stun", "seconds": 3}),
        lambda a: "error" not in a,
    )
    check(
        "stage health",
        tool("stage", {"action": "health", "id": wolf, "value": 10}),
        lambda a: "error" not in a,
    )
    abilities = me.get("abilities") or []
    if abilities:
        check(
            "stage cooldown",
            tool(
                "stage",
                {
                    "action": "cooldown",
                    "id": hero,
                    "ability": abilities[0]["id"],
                    "seconds": 0,
                },
            ),
            lambda a: a["cooldown"]["remaining"] == 0,
        )
    check("camera read", tool("camera", {}), lambda a: a["following"]["id"])
    check(
        "camera face",
        tool("camera", {"action": "face", "target": wolf}),
        lambda a: "yawDegrees" in a,
    )
    # A hostile creature appearing may start a fight, and the game (or a mod) pause for it; a
    # death does not come while it is paused.
    tool("game", {"action": "unpause"})
    kill = tool("stage", {"action": "kill", "id": wolf})
    died = wait("eluvian.creature.died", kill.get("cursor", 0), {"id": wolf}, ms=3000)
    for _ in range(4):
        # The fight the wolf's appearing began may pause the game a moment after; a death
        # waits for it to run.
        if died.get("found") or not died.get("paused"):
            break
        tool("game", {"action": "unpause"})
        died = wait(
            "eluvian.creature.died", kill.get("cursor", 0), {"id": wolf}, ms=3000
        )
    check("stage kill, and the death on the bus", died, lambda a: a["found"])

    check(
        "screenshot of the window",
        tool("screenshot", {"image": False}),
        lambda a: a["bytes"] > 1000,
    )
    check(
        "screenshot, the game's own",
        tool("screenshot", {"source": "game", "image": False}),
        lambda a: a["bytes"] > 10000,
    )
    check(
        "game pause", tool("game", {"action": "pause"}), lambda a: a["paused"] is True
    )
    check(
        "game unpause",
        tool("game", {"action": "unpause"}),
        lambda a: a["paused"] is False,
    )
    check("game saves", tool("game", {"action": "saves"}), lambda a: "error" not in a)

    # A message box of the scripts' own, read and answered. 379897 is an ability's name: any
    # string will do for a box's text.
    tool("script", {"action": "call", "command": "ShowPopup", "args": [379897, 1]})
    time.sleep(1.0)
    check(
        "inspect popup with a box up",
        tool("inspect", {"kind": "popup"}),
        lambda a: len(a["popup"]["buttons"]) == 2,
    )
    check(
        "game press",
        tool("game", {"action": "press", "button": 1}),
        lambda a: a["popup"] is None and a["pressed"]["label"],
    )
    check(
        "the box's answer reaches the scripts",
        wait("eluvian.script.popup_result", start, ms=3000),
        lambda a: a["found"],
    )
    check(
        "input, the pause key's binding",
        tool("input", {"binding": "UserTogglePauseMessage"}),
        lambda a: a["sent"] is True,
    )
    time.sleep(0.4)
    check(
        "which paused the game",
        tool("inspect", {"kind": "game"}),
        lambda a: a["paused"] is True,
    )
    tool("input", {"binding": "UserTogglePauseMessage"})
    time.sleep(0.4)
    check(
        "and unpaused it",
        tool("inspect", {"kind": "game"}),
        lambda a: a["paused"] is False,
    )
    check(
        "input refuses a text that names a save",
        tool("input", {"binding": "SaveGameMessage -1 2"}),
        lambda a: "names a save" in a["error"],
    )
    check(
        "input refuses a message the game does not know",
        tool("input", {"message": "Nobody.NoSuchThing 1"}),
        lambda a: a["sent"] is False,
    )
    check(
        "dialogue refuses with no conversation",
        tool("dialogue", {"action": "continue"}),
        lambda a: "no conversation" in a["error"],
    )
    check(
        "events describe",
        tool("events", {"describe": True}),
        lambda a: any(t["topic"] == "eluvian.command.ended" for t in a["topics"]),
    )
    check(
        "log",
        tool("log", {"log": "eluvian.log", "since": 0, "limit": 5}),
        lambda a: "error" not in a,
    )
    check(
        "scenario",
        tool("scenario", {"steps": [{"tool": "health"}, {"sleepMs": 50}]}),
        lambda a: a["stepsRun"] == 2,
    )
    events = tool(
        "events", {"since": start, "topic": "eluvian.game.saved", "limit": 10}
    )
    check("no save was written", events, lambda a: a["events"] == [])


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--character", required=True, help="the save's character folder"
    )
    parser.add_argument("--kind", default="manual", choices=["manual", "auto", "quick"])
    parser.add_argument("--number", type=int, required=True, help="the slot's number")
    parser.add_argument(
        "--exe", type=bridge_module.Path, default=bridge_module.DEFAULT_GAME
    )
    parser.add_argument(
        "--keep", action="store_true", help="leave the game running afterwards"
    )
    options = parser.parse_args()

    folder = bridge_module.log_folder()
    processes = bridge_module.Processes(options.exe)
    bridge = bridge_module.Bridge(
        bridge_module.Game(folder, None, processes.running),
        processes,
        folder / "eluvian.tools.json",
        lambda _message: None,
    )
    save = {
        "character": options.character,
        "kind": options.kind,
        "number": options.number,
    }
    launch = bridge.game_launch({"owner": OWNER, "load": save, "waitSeconds": 240})
    answer = launch.get("structuredContent") or {}
    if launch.get("isError") or not answer.get("launched"):
        print(f"not started: {json.dumps(launch, ensure_ascii=False)[:800]}")
        return 2
    run = Run(bridge)
    try:
        checks(run)
    finally:
        if not options.keep:
            bridge.game_quit({})
    print(f"{len(run.passed)} passed, {len(run.failed)} failed")
    for name in run.failed:
        print(f"  failed: {name}")
    return 1 if run.failed else 0


if __name__ == "__main__":
    sys.exit(main())
