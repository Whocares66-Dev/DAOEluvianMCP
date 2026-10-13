#!/usr/bin/env python3
"""Every Eluvian tool once, in the game: the check after a change that moves code.

    python tools/smoke.py --character Lyna --kind manual --number 4

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
        except (TimeoutError, urllib.error.URLError) as error:
            # A call that never answers fails its check; the run goes on (#113).
            return {"error": f"no answer in {timeout} s: {error}"}

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


def checks(run: Run, owner: str) -> None:
    tool, check, wait = run.tool, run.check, run.wait
    health = tool("health")
    start = health.get("cursor", 0)
    check("health names this run as the owner", health, lambda a: a["owner"] == owner)

    game = tool("state")
    check("state", game, lambda a: a["hero"]["id"] and "popup" in a)
    hero = (game.get("hero") or {}).get("id")
    creatures = tool("creatures", {"radius": 60})
    check(
        "creatures",
        creatures,
        lambda a: (
            a["count"] >= 1 and {"room", "group", "side"} <= set(a["creatures"][0])
        ),
    )
    me = tool("creature", {"id": hero})
    check("creature", me, lambda a: "effects" in a and "abilities" in a)
    check(
        "character",
        tool("character", {"id": hero}),
        lambda a: (
            a["level"] >= 1
            and a["class"]["label"]
            and a["attributes"]["strength"]["total"] > 0
        ),
    )
    check(
        "saves save refuses the player's quicksave",
        tool("saves", {"action": "save", "kind": "quick"}),
        lambda a: "quicksave is the player's" in a.get("error", ""),
    )
    check(
        "pointer holds the game's cursor at a creature, no click",
        tool("pointer", {"target": hero, "seconds": 5}),
        lambda a: "x" in a["pointed"] and a["holds"] == 5,
    )
    check(
        "pointer gives the cursor back",
        tool("pointer", {"release": True}),
        lambda a: a["released"] is True,
    )
    check(
        "steer holds a movement key for the controlled creature",
        tool("steer", {"direction": "forward", "ms": 300}),
        lambda a: a["held"][0]["direction"] == "forward" and "id" in a["controlled"],
    )
    check(
        "steer lets go",
        tool("steer", {"release": True}),
        lambda a: a["released"] == ["forward"],
    )
    check(
        "journal, the quests under way",
        tool("journal"),
        lambda a: (
            len(a["quests"]) > 0 and all(q["plot"] and q["name"] for q in a["quests"])
        ),
    )
    check(
        "exits, the ways out of the area",
        tool("exits"),
        lambda a: a["count"] > 0 and all(e["leadsTo"]["area"] for e in a["exits"]),
    )
    check(
        "plot, a story's flags",
        tool("plot", {"plot": "gen00pt_backgrounds"}),
        lambda a: len(a["set"]) == 1 and len(a["flags"]) > 5,
    )
    check(
        "plot refuses a plot the game has not",
        tool("plot", {"plot": "no_such_plot"}),
        lambda a: "no plot" in a.get("error", ""),
    )
    check(
        "tactics look, a creature's slots",
        tool("tactics", {"action": "look", "id": hero}),
        lambda a: (
            a["slots"] == len(a["tactics"])
            and all("empty" in t or "command" in t for t in a["tactics"])
        ),
    )
    check(
        "abilities, all a creature knows",
        tool("abilities", {"id": hero}),
        lambda a: (
            a["count"] >= len(a["talents"]) + len(a["spells"]) > 0 or a["count"] == 0
        ),
    )
    check(
        "equipment",
        tool("equipment", {"id": hero}),
        lambda a: isinstance(a["slots"], dict) and "weaponSet" in a,
    )
    pack = tool("inventory")
    check("inventory", pack, lambda a: a["count"] == len(a["items"]))
    # The tools no one had called since #110 split them (#113). Those that need a place the
    # save may not have (a quest board, a crafting skill, a follower with a gift) pass when
    # they refuse with their own clear message: their arguments and first reads still run.
    # The first call reads every archive's index from disk: a minute or more.
    found = tool(
        "resources", {"type": "uti", "match": "dagger", "most": 3}, timeout=180
    )
    check(
        "resources, item templates by a word",
        found,
        lambda a: a["count"] >= 1 and len(a["resources"]) >= 1,
    )
    check(
        "controls, the key bindings",
        tool("controls", {"match": "pause"}),
        lambda a: a["count"] == len(a["controls"]),
    )
    check(
        "surroundings, one look around",
        tool("surroundings", {"radius": 30}),
        lambda a: "creatures" in a and "usable" in a and "exits" in a,
    )
    check(
        "board refuses with no quest board up",
        tool("board", {"action": "look"}),
        lambda a: "quest board is not up" in a.get("error", "") or "entries" in a,
    )
    check(
        "craft lists poisons, or says the maker has not the skill",
        tool("craft", {"kind": "poison", "showMs": 0}, timeout=40),
        lambda a: "recipes" in a or a.get("error", "").startswith("craft:"),
    )
    check(
        "gift refuses a gift no follower can take",
        tool("gift", {"to": hero, "item": "0x0", "budgetMs": 1000}),
        lambda a: a.get("error", "").startswith("gift:"),
    )
    party = [m.get("id") for m in game.get("party") or [] if m.get("id")]
    other = next((m for m in party if m != hero), hero)
    check(
        "control, to a party member",
        tool("control", {"id": other}),
        lambda a: a["taken"] is True and a["controlled"]["id"] == other,
    )
    check(
        "control, back to the hero",
        tool("control", {"id": hero}),
        lambda a: a["taken"] is True and a["controlled"]["id"] == hero,
    )
    check(
        "wait_until, a condition on a tool's answer",
        tool(
            "wait_until",
            {"tool": "state", "pointer": "/hero/id", "equals": hero, "timeoutMs": 5000},
            timeout=20,
        ),
        lambda a: a["held"] is True,
    )
    if pack.get("items"):
        check(
            "item",
            tool("item", {"id": pack["items"][0]["id"]}),
            lambda a: a["baseType"]["label"] and "properties" in a,
        )
    check(
        "placeables",
        tool("placeables"),
        lambda a: "placeables" in a,
    )
    check(
        "dialogue look",
        tool("dialogue", {"action": "look"}),
        lambda a: a["active"] is False,
    )
    check(
        "gui",
        tool("gui"),
        lambda a: (
            a["popup"] is None and a["loot"] is None and isinstance(a["screens"], list)
        ),
    )
    aim = tool("screen", {"target": hero})
    check(
        "screen, a pixel to click the hero at",
        aim,
        lambda a: (
            a["view"]["width"] > 100 and a["aim"]["inView"] and 0 < a["aim"]["fx"] < 1
        ),
    )
    check(
        "screen, and the hero is what lies under it",
        tool("screen", {"pixel": [aim["aim"]["x"], aim["aim"]["y"]]})
        if aim.get("aim")
        else {},
        lambda a: a["hits"][0]["object"]["id"] == hero,
    )
    check(
        "data, a path the tree has not",
        tool("data", {"path": "Nothing.AtAll"}),
        lambda a: "found" in a,
    )
    check(
        "cursor",
        tool("cursor"),
        lambda a: "hits" in a and "object" in a,
    )
    check(
        "window",
        tool("window"),
        lambda a: a["client"]["width"] > 100 and a["client"]["height"] > 100,
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

    # The pause key's binding flips the pause. Here, before anything that may start a fight:
    # in one a mod may give the key another meaning, and the game pauses by itself.
    before = tool("state").get("paused")
    check(
        "input, the pause key's binding",
        tool("input", {"binding": "UserTogglePauseMessage"}),
        lambda a: a["sent"] is True,
    )
    time.sleep(0.4)
    check(
        "which flipped the pause",
        tool("state"),
        lambda a: a["paused"] is (not before),
    )
    tool("input", {"binding": "UserTogglePauseMessage"})
    time.sleep(0.4)
    check(
        "and flipped it back",
        tool("state"),
        lambda a: a["paused"] is before,
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
        "stage_spawn",
        {
            "template": "wolf",
            "point": [here["x"] - 12, here["y"], here["z"]],
        },
    )
    check("stage_spawn", spawn, lambda a: a["creature"]["id"] and "moved" in a)
    wolf = (spawn.get("creature") or {}).get("id")
    check(
        "geometry with the walk",
        tool("geometry", {"id": hero, "target": wolf, "walk": True}),
        lambda a: "lineOfSight" in a and "found" in a["walk"],
    )
    check(
        "stage_group",
        tool("stage_group", {"id": wolf, "group": 2}),
        lambda a: a["creature"]["group"] == 2,
    )
    check(
        "stage_effect apply",
        tool(
            "stage_effect",
            {"action": "apply", "id": wolf, "effect": "stun", "seconds": 3},
        ),
        lambda a: "error" not in a,
    )
    check(
        "stage_health",
        tool("stage_health", {"id": wolf, "value": 10}),
        lambda a: "error" not in a,
    )
    abilities = me.get("abilities") or []
    if abilities:
        check(
            "stage_cooldown",
            tool(
                "stage_cooldown",
                {
                    "id": hero,
                    "ability": abilities[0]["id"],
                    "seconds": 0,
                },
            ),
            lambda a: a["cooldown"]["remaining"] == 0,
        )
    templates = found.get("resources") or []
    if templates:
        template = templates[0]["name"]
        check(
            "stage_equip, an item made into the hero's pack",
            tool("stage_equip", {"id": hero, "template": template, "equip": False}),
            lambda a: a["item"]["id"] and a["equipped"] is False,
        )
    check("camera read", tool("camera", {}), lambda a: a["following"]["id"])
    check(
        "camera face",
        tool("camera", {"action": "face", "target": wolf}),
        lambda a: "yawDegrees" in a,
    )
    check(
        "camera turn, a tilt",
        tool("camera", {"action": "turn", "pitchDegrees": 100}),
        lambda a: abs(a["pitchDegrees"] - 100) < 0.5,
    )
    check(
        "camera behind the hero",
        tool("camera", {"action": "behind"}),
        lambda a: "yawDegrees" in a and "note" in a,
    )
    check(
        "camera zoom",
        tool("camera", {"action": "zoom", "zoom": 0.6}),
        lambda a: abs(a["zoomSetting"] - 0.6) < 0.01,
    )
    # A hostile creature appearing may start a fight, and the game (or a mod) pause for it; a
    # death does not come while it is paused.
    tool("pause", {"paused": False})
    kill = tool("stage_kill", {"id": wolf})
    died = wait("eluvian.creature.died", kill.get("cursor", 0), {"id": wolf}, ms=3000)
    for _ in range(4):
        # The fight the wolf's appearing began may pause the game a moment after; a death
        # waits for it to run.
        if died.get("found") or not died.get("paused"):
            break
        tool("pause", {"paused": False})
        died = wait(
            "eluvian.creature.died", kill.get("cursor", 0), {"id": wolf}, ms=3000
        )
    check("stage_kill, and the death on the bus", died, lambda a: a["found"])
    check(
        "stage_reset, the party back to full and the wolf's body taken away",
        tool("stage_reset", {}),
        lambda a: (
            len(a["reset"]) >= 1 and any(r.get("id") == wolf for r in a["removed"])
        ),
    )

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
    check("pause", tool("pause", {"paused": True}), lambda a: a["paused"] is True)
    check(
        "pause off",
        tool("pause", {"paused": False}),
        lambda a: a["paused"] is False,
    )
    check("saves list", tool("saves", {"action": "list"}), lambda a: "error" not in a)

    # A message box of the scripts' own, read and answered. 379897 is an ability's name: any
    # string will do for a box's text.
    tool("script", {"action": "call", "command": "ShowPopup", "args": [379897, 1]})
    time.sleep(1.0)
    check(
        "gui with a box up",
        tool("gui"),
        lambda a: len(a["popup"]["buttons"]) == 2,
    )
    check(
        "press",
        tool("press", {"button": 1}),
        lambda a: a["popup"] is None and a["pressed"]["label"],
    )
    check(
        "the box's answer reaches the scripts",
        wait("eluvian.script.popup_result", start, ms=3000),
        lambda a: a["found"],
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
    # A cutscene of the base game's, started as the scripts start one, and skipped as Escape
    # skips it.
    played = tool("stage_cutscene", {"resource": "bed110cs_tainted_mirror.cut"})
    check(
        "stage_cutscene puts the game in a cutscene",
        wait(
            "eluvian.mode.changed", played.get("cursor", 0), {"to": "cutscene"}, ms=5000
        ),
        lambda a: a["found"],
    )
    skipped = tool("dialogue", {"action": "skip_cutscene"})
    check("dialogue skip_cutscene", skipped, lambda a: a["skipped"] == "cutscene")
    check(
        "and the game is out of it",
        wait(
            "eluvian.mode.changed",
            skipped.get("cursor", 0),
            {"from": "cutscene"},
            ms=5000,
        ),
        lambda a: a["found"],
    )
    # A pre-rendered movie of the base game's, started by the scripts' PlayMovie, and skipped.
    cursor = tool("health").get("cursor", 0)
    tool(
        "script",
        {"action": "call", "command": "PlayMovie", "args": ["game_introduction.bik"]},
    )
    check(
        "a movie puts the game in its movie mode",
        wait("eluvian.mode.changed", cursor, {"to": "movie"}, ms=5000),
        lambda a: a["found"],
    )
    check(
        "dialogue look says a movie plays",
        tool("dialogue", {"action": "look"}),
        lambda a: a["movie"] is True and a["cutscene"] is False,
    )
    skipped = tool("dialogue", {"action": "skip_cutscene"})
    check(
        "dialogue skip_cutscene skips the movie",
        skipped,
        lambda a: a["skipped"] == "movie",
    )
    check(
        "and the game is out of the movie",
        wait(
            "eluvian.mode.changed", skipped.get("cursor", 0), {"from": "movie"}, ms=5000
        ),
        lambda a: a["found"],
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
    parser.add_argument(
        "--owner",
        default=OWNER,
        help="the name to start the game under, which its health then says as `owner`",
    )
    parser.add_argument(
        "--instance",
        default="",
        help="run in a named instance, a copy of the game beside the one that may be running, "
        "with a profile of its own (#116); a new one is given the settings and this save alone",
    )
    parser.add_argument(
        "--plugins",
        help="load the plugins from this folder in place of the game folder's: a build tree's "
        "daolib\\plugins, so the build under test is deployed nowhere",
    )
    options = parser.parse_args()

    instance = bridge_module.instance_name(options.instance)
    if instance is None:
        parser.error(
            f"--instance {options.instance!r}: 1 to 32 letters, digits, - or _"
        )
    folder = bridge_module.log_folder(instance)
    processes = bridge_module.Processes(options.exe, instance)
    bridge = bridge_module.Bridge(
        bridge_module.Game(folder, None, processes.running),
        processes,
        folder / "eluvian.tools.json",
        lambda _message: None,
        characters=folder.parent / "Characters",
        default=bridge_module.profile_folder(""),
    )
    save = {
        "character": options.character,
        "kind": options.kind,
        "number": options.number,
    }
    asked = {"owner": options.owner, "load": save, "waitSeconds": 240}
    if instance:
        asked["seed"] = "save"
    if options.plugins:
        asked["plugins"] = options.plugins
    launch = bridge.game_launch(asked)
    answer = launch.get("structuredContent") or {}
    if launch.get("isError") or not answer.get("launched"):
        print(f"not started: {json.dumps(launch, ensure_ascii=False)[:800]}")
        return 2
    run = Run(bridge)
    try:
        checks(run, options.owner)
    finally:
        if not options.keep:
            bridge.game_quit({})
    print(f"{len(run.passed)} passed, {len(run.failed)} failed")
    for name in run.failed:
        print(f"  failed: {name}")
    return 1 if run.failed else 0


if __name__ == "__main__":
    sys.exit(main())
