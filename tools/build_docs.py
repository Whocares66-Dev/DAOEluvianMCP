#!/usr/bin/env python3
"""The docs site: what Eluvian serves, generated from what it registers (#109).

    python tools/build_docs.py --update     # refresh docs/_data/eluvian.json from eluvian_describe
    python tools/build_docs.py --check      # fail if it is stale, or a page will not render (ctest)
    python tools/build_docs.py              # stage and build the site into build/docs-site
    python tools/build_docs.py --serve      # the same, served at http://127.0.0.1:4000/DAOEluvianMCP/

The reference is never written by hand. `eluvian_describe` (src/plugin/Describe.cpp)
registers every tool and topic as the plugin does at load, without the game, and prints
them as tools/list and `events describe=true` give them; its output is kept in
docs/_data/eluvian.json, since the site is built on Linux, where the 32-bit Windows
program does not run, and --check keeps it equal to a fresh run. The pages are made from
it at build time: one per tool, the events, and the C interface (src/api/EluvianAPI.h,
shown whole: its comments are its documentation). The front page and the guide are the
site's own, short, in docs/ (index.md, getting-started.md); the README is linked, not
copied. Only the API is here: the tools, the events and the C interface, not the internals.
"""

from __future__ import annotations

import argparse
import functools
import http.server
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
DATA = DOCS / "_data" / "eluvian.json"
STAGE = ROOT / "build" / "docs-src"
SITE = ROOT / "build" / "docs-site"
BASEURL = "/DAOEluvianMCP"


def describe(exe: Path) -> dict:
    out = subprocess.run(
        [str(exe)], capture_output=True, text=True, encoding="utf-8", check=True
    )
    return json.loads(out.stdout)


def find_exe(given: str | None) -> Path:
    if given:
        return Path(given)
    for preset in ("debug", "release"):
        exe = ROOT / "build" / preset / "eluvian_describe.exe"
        if exe.exists():
            return exe
    sys.exit(
        "no eluvian_describe.exe: build it (tools\\build.ps1 -Preset debug) or pass --exe"
    )


def written(data: dict) -> str:
    return json.dumps(data, indent=1, ensure_ascii=False) + "\n"


# --- Page parts ------------------------------------------------------------


def front(title: str, permalink: str, nav_order: int, **more: object) -> str:
    lines = [
        "---",
        f"title: {json.dumps(title)}",
        f"permalink: {permalink}",
        f"nav_order: {nav_order}",
    ]
    lines += [f"{key}: {json.dumps(value)}" for key, value in more.items()]
    return "\n".join(lines + ["---", ""])


def raw(text: str) -> str:
    """A body Liquid leaves alone: tool text may hold `{{` or `{%`."""
    return "{% raw %}\n" + text.rstrip("\n") + "\n{% endraw %}\n"


# --- The reference --------------------------------------------------------


def cell(text: object) -> str:
    return str(text).replace("|", "\\|").replace("\n", " ")


def first_sentence(text: str) -> str:
    """The first sentence of the first paragraph, for the tools' index: a colon
    that opens a list ends it, as a full stop."""
    paragraph = text.split("\n\n", 1)[0].split("\n", 1)[0]
    match = re.match(r"(.+?)([.:])(\s|$)", paragraph)
    return (match.group(1) + "." if match else paragraph)[:200]


def type_of(schema: dict) -> str:
    if "enum" in schema:
        return " \\| ".join(f"`{json.dumps(v)}`" for v in schema["enum"])
    if "oneOf" in schema:
        return " or ".join(type_of(s) for s in schema["oneOf"])
    kind = schema.get("type")
    if isinstance(kind, list):
        kind = " or ".join(kind)
    text = kind or "any"
    bounds = []
    if "minimum" in schema:
        bounds.append(f"≥ {schema['minimum']}")
    if "maximum" in schema:
        bounds.append(f"≤ {schema['maximum']}")
    return text + (f" ({', '.join(bounds)})" if bounds else "")


def tool_page(tool: dict, order: int) -> str:
    name = tool["name"]
    schema = tool.get("inputSchema") or {}
    properties = schema.get("properties") or {}
    required = set(schema.get("required") or [])
    notes = []
    if (tool.get("annotations") or {}).get("readOnlyHint"):
        notes.append("Read-only: it changes nothing in the game.")
    budget = (tool.get("_meta") or {}).get("eluvian/budget")
    if budget:
        notes.append(
            f"A call may take up to `{budget['argument']}` milliseconds, {budget['defaultMs']} by default."
        )
    body = [f"# {name}", "{: .identifier}", "", tool["description"], ""]
    if notes:
        body += [" ".join(notes), ""]
    body += ["## Arguments", ""]
    if properties:
        body += ["| Name | Type | Required | What |", "|---|---|---|---|"]
        for arg, spec in properties.items():
            mark = "yes" if arg in required else ""
            body.append(
                f"| `{arg}` | {type_of(spec)} | {mark} | {cell(spec.get('description', ''))} |"
            )
    else:
        body.append("None.")
    body += [
        "",
        f"Call it as `{name}` over MCP, or `POST /api/tool/{name}` with the arguments as JSON.",
    ]
    page = front(name, f"/tools/{name}/", order, parent="Tools")
    return page + raw("\n".join(body))


def tools_index(tools: list[dict]) -> str:
    rows = ["| Tool | What |", "|---|---|"]
    rows += [
        f"| [{t['name']}]({BASEURL}/tools/{t['name']}/) | {cell(first_sentence(t['description']))} |"
        for t in tools
    ]
    body = [
        "# Tools",
        "",
        "Every tool the bench serves, as `tools/list` gives them; generated from the code. Plugins add tools of "
        "their own (`health` lists them); their docs are theirs.",
        "",
        *rows,
    ]
    return front("Tools", "/tools/", 3, has_children=True, has_toc=False) + raw(
        "\n".join(body)
    )


def events_page(topics: list[dict]) -> str:
    body = [
        "# Events",
        "",
        'The topics Eluvian emits, as `events {"describe": true}` lists them: wait for one with `wait_for` '
        "from the `cursor` an answer gives. Plugins describe topics of their own under their prefix.",
        "",
    ]
    for topic in topics:
        body += [f"## {topic['topic']}", "", topic["description"], ""]
        fields = topic.get("fields") or {}
        if fields:
            body += ["| Field | What |", "|---|---|"]
            body += [f"| `{field}` | {cell(what)} |" for field, what in fields.items()]
            body.append("")
    return front("Events", "/events/", 4) + raw("\n".join(body))


def c_interface_page(header: str) -> str:
    body = [
        "# The C interface",
        "",
        "Another DAOLib plugin adds tools and events through `src/api/EluvianAPI.h`, a C header under the MIT "
        "license, so a plugin under any license can include it. Its comments are its documentation:",
        "",
        "```c",
        header.rstrip("\n"),
        "```",
    ]
    return front("C interface", "/c-interface/", 5) + raw("\n".join(body))


# --- The site --------------------------------------------------------------

# What a release's site takes from today rather than from its tag: the theme,
# so every version looks alike and the version selector is in each (as
# SkyrimFollowerTactics' build-docs.py does).
CHROME = ("_includes", "_sass", "Gemfile", "Gemfile.lock")
LOCAL_OUTPUT = shutil.ignore_patterns("_site", ".jekyll-cache", ".bundle", "vendor")


def stage(
    data: dict, docs: Path = DOCS, header: str | None = None, target: Path = STAGE
) -> None:
    """A docs folder with every generated page beside it, in `target`."""
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(docs, target, ignore=LOCAL_OUTPUT)
    if header is None:
        header = (ROOT / "src" / "api" / "EluvianAPI.h").read_text(encoding="utf-8")
    # The front page (docs/index.md) and the guide (docs/getting-started.md) are
    # the site's own and short: the README is linked, not copied, and the full
    # playbook is not on the site (the user, 2026-10-09).
    tools = sorted(data["tools"], key=lambda t: t["name"])
    folder = target / "tools"
    folder.mkdir(exist_ok=True)
    (folder / "index.md").write_text(tools_index(tools), encoding="utf-8", newline="\n")
    for order, tool in enumerate(tools, 1):
        (folder / f"{tool['name']}.md").write_text(
            tool_page(tool, order), encoding="utf-8", newline="\n"
        )
    (target / "events.md").write_text(
        events_page(data["topics"]), encoding="utf-8", newline="\n"
    )
    (target / "c-interface.md").write_text(
        c_interface_page(header), encoding="utf-8", newline="\n"
    )


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", *args], cwd=ROOT)


def release_tags() -> list[str]:
    """vX.Y.Z tags, newest first."""
    found = []
    for tag in git("tag", "--list").decode().split():
        match = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", tag)
        if match:
            found.append((tuple(map(int, match.groups())), tag))
    return [tag for _, tag in sorted(found, reverse=True)]


def from_tag(tag: str, into: Path) -> Path | None:
    """The tag's docs folder and C header, staged with today's theme; nothing for a
    tag from before the docs site."""
    archive = (
        git("archive", "--format=zip", tag, "docs", "src/api/EluvianAPI.h")
        if has_docs(tag)
        else None
    )
    if archive is None:
        return None
    raw_dir = into / f"{tag}-raw"
    with zipfile.ZipFile(io.BytesIO(archive)) as zipped:
        for entry in zipped.infolist():
            path = raw_dir / entry.filename
            if not path.resolve().is_relative_to(raw_dir.resolve()):
                raise ValueError(f"unsafe archive entry {entry.filename}")
            if entry.is_dir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(zipped.read(entry))
    docs = raw_dir / "docs"
    for name in CHROME:
        source, dest = DOCS / name, docs / name
        if dest.exists():
            shutil.rmtree(dest) if dest.is_dir() else dest.unlink()
        if source.is_dir():
            shutil.copytree(source, dest, ignore=LOCAL_OUTPUT)
        elif source.exists():
            shutil.copyfile(source, dest)
    target = into / tag
    data = json.loads((docs / "_data" / "eluvian.json").read_text(encoding="utf-8"))
    header = (raw_dir / "src" / "api" / "EluvianAPI.h").read_text(encoding="utf-8")
    stage(data, docs, header, target)
    return target


def has_docs(tag: str) -> bool:
    return (
        subprocess.run(
            ["git", "cat-file", "-e", f"{tag}:docs/_data/eluvian.json"],
            cwd=ROOT,
            capture_output=True,
            check=False,
        ).returncode
        == 0
    )


def permalinks(source: Path) -> list[str]:
    """The pages a version has, so the selector keeps you on the same page where it exists."""
    found = set()
    for path in source.rglob("*.md"):
        match = re.search(
            r"^permalink:\s*[\"']?(/[^\"'\s]*)", path.read_text(encoding="utf-8"), re.M
        )
        if match:
            found.add(match.group(1))
    return sorted(found)


def build_site() -> int:
    """Current (the working tree) at the root, each release tag under /X.Y.Z/, each with the
    list of versions for its selector."""
    out = SITE / BASEURL[1:]
    if SITE.exists():
        shutil.rmtree(SITE)
    out.mkdir(parents=True)
    with tempfile.TemporaryDirectory(
        prefix="docs-versions-", dir=ROOT / "build"
    ) as temporary:
        staging = Path(temporary)
        stage(json.loads(DATA.read_text(encoding="utf-8")), target=STAGE)
        sources = [("Current", STAGE, BASEURL)]
        for tag in release_tags():
            source = from_tag(tag, staging)
            if source is not None:
                sources.append((tag[1:], source, f"{BASEURL}/{tag[1:]}"))
        versions = [
            {"label": label, "baseurl": url, "pages": permalinks(source)}
            for label, source, url in sources
        ]
        env = {**os.environ, "BUNDLE_GEMFILE": str(DOCS / "Gemfile")}
        for index, (label, source, url) in enumerate(sources):
            # JSON is YAML: Jekyll merges this after the version's own config.
            config = staging / f"config-{index}.yml"
            config.write_text(
                json.dumps(
                    {"baseurl": url, "docs_version": label, "docs_versions": versions}
                ),
                encoding="utf-8",
            )
            command = [
                "bundle",
                "exec",
                "jekyll",
                "build",
                "--source",
                str(source),
                "--destination",
                str(out if index == 0 else out / label),
                "--config",
                f"{source / '_config.yml'},{config}",
            ]
            done = subprocess.run(
                command, cwd=DOCS, env=env, shell=sys.platform == "win32", check=False
            )
            if done.returncode != 0:
                return done.returncode
    (out / ".nojekyll").touch()
    print(f"built {len(sources)} version(s) into {out}")
    return 0


def serve() -> int:
    """The built site at http://127.0.0.1:4000/DAOEluvianMCP/, every version in it."""
    handler = functools.partial(
        http.server.SimpleHTTPRequestHandler, directory=str(SITE)
    )
    with http.server.ThreadingHTTPServer(("127.0.0.1", 4000), handler) as server:
        print(f"serving http://127.0.0.1:4000{BASEURL}/ (Ctrl+C stops)", flush=True)
        server.serve_forever()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--update", action="store_true", help="refresh docs/_data/eluvian.json"
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if it is stale or a page will not render",
    )
    parser.add_argument(
        "--serve", action="store_true", help="build every version and serve the site"
    )
    parser.add_argument(
        "--exe",
        help="eluvian_describe.exe; default build\\debug's, else build\\release's",
    )
    options = parser.parse_args()

    if options.update or options.check:
        fresh = describe(find_exe(options.exe))
        if options.update:
            DATA.parent.mkdir(parents=True, exist_ok=True)
            DATA.write_text(written(fresh), encoding="utf-8", newline="\n")
            print(
                f"{DATA.relative_to(ROOT)}: {len(fresh['tools'])} tools, {len(fresh['topics'])} topics"
            )
            return 0
        kept = DATA.read_text(encoding="utf-8") if DATA.exists() else ""
        if kept != written(fresh):
            print(
                "docs/_data/eluvian.json is not what eluvian_describe prints now: run "
                "python tools/build_docs.py --update and commit it"
            )
            return 1
        stage(fresh)
        print(
            f"current: {len(fresh['tools'])} tools, {len(fresh['topics'])} topics; pages staged"
        )
        return 0

    built = build_site()
    if built != 0 or not options.serve:
        return built
    return serve()


if __name__ == "__main__":
    sys.exit(main())
