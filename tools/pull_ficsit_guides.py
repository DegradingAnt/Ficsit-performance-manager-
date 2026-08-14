#!/usr/bin/env python3
# WHY NOT RUST: same reason as extract_scalability.py, check_mod_linkage.py and package_fpm.py
# beside it - this is the Satisfactory project, whose tools/ is Python, and the job is one HTTP
# POST plus writing markdown. LAW 4: apply THIS project's rules, and the Rust-everywhere law is
# vox-scoped by its own wording. Ant confirmed it directly on 2026-08-12: "yeah just use python
# here". It lives here rather than in the brain because extract_scalability.py beside it already
# reads a mirror under 20-SOURCES - an FPM tool reaching into the sources tree is established.
#
# LADDER (bank -> catalogue -> online -> build), run 2026-08-14 before writing a line:
#   brain_skills  -> closest was sf-mine, which is the METHOD for routing what you mine and has
#                    no retrieval step at all; it assumes the source is already readable.
#   online        -> SMM exports a PROFILE and a "copy mods list"; neither touches guides. No
#                    third-party SMR guide mirror exists. Absence confirmed at the last rung.
"""Mirror every ficsit.app community guide to disk via the SMR GraphQL API, bodies included.

WHY THE API AND NOT THE PAGE: https://ficsit.app/guides returns HTTP 403 to a plain fetch (bot
protection), which reads exactly like "there is nothing there". The site's own API Docs page
recommends GraphQL over REST - "It is recommended that you use the GraphQL API as the REST API is
not feature complete by design" - so this is the sanctioned route, not a workaround.

    https://api.ficsit.app/v2/query      GraphQL endpoint (playground at /v2)
    https://api.ficsit.app/v1            REST, explicitly NOT feature complete

⚠ LICENCE: these guides are THIRD-PARTY AUTHORED CONTENT. This mirror is a PRIVATE read cache
under 20-SOURCES, exactly like the FModel dump. Per sf-mine: facts, mechanics and techniques
travel out of it; verbatim text does NOT, and none of it goes into the public FPM repo.

Writes one .md per guide plus a raw .json of everything, so a later read never re-hits the network
and the next pull can be diffed against these bytes.

    python tools/pull_ficsit_guides.py            # write
    python tools/pull_ficsit_guides.py --check    # count only, write nothing
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.request
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ENDPOINT = "https://api.ficsit.app/v2/query"
OUT = Path(r"C:\VOX-BRAIN-ROOT\20-SOURCES\satisfactory\ficsit-guides")
PAGE = 25

# ⚠ COUNT AND GUIDES CANNOT BE ASKED FOR IN THE SAME PAGED QUERY. Measured 2026-08-14: adding
# `count` beside `guides` with an offset returns {"message":"sql: no rows in result set",
# "path":["getGuides","count"]} and takes the WHOLE response down with it. The two are separate
# round trips on purpose, not redundancy.
COUNT_QUERY = "{ getGuides(filter:{limit:1}) { count } }"

PAGE_QUERY = """
query($limit:Int!,$offset:Int!){
  getGuides(filter:{limit:$limit, offset:$offset}){
    guides { id name short_description guide views created_at updated_at tags { name } user { username } }
  }
}
"""


def post(query: str, variables: dict | None = None) -> dict:
    payload_in: dict = {"query": query}
    if variables is not None:
        payload_in["variables"] = variables
    body = json.dumps(payload_in).encode()
    req = urllib.request.Request(
        ENDPOINT, data=body,
        headers={"Content-Type": "application/json", "User-Agent": "vox-brain-docs-mirror/1.0"},
    )
    with urllib.request.urlopen(req, timeout=60) as r:
        payload = json.load(r)
    if "errors" in payload:
        raise SystemExit(f"FAIL  GraphQL errors: {payload['errors']}")
    return payload["data"]["getGuides"]


def fetch_count() -> int:
    return post(COUNT_QUERY)["count"]


def fetch_page(limit: int, offset: int) -> list[dict]:
    return post(PAGE_QUERY, {"limit": limit, "offset": offset})["guides"]


def slug(text: str, gid: str) -> str:
    s = re.sub(r"[^A-Za-z0-9]+", "-", text).strip("-").lower()[:70] or "untitled"
    return f"{s}--{gid}.md"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="report the count; write nothing")
    args = ap.parse_args()

    total = fetch_count()
    print(f"ok    API reports {total} guide(s)")
    if args.check:
        print("\n--check: nothing written.")
        return 0

    OUT.mkdir(parents=True, exist_ok=True)

    seen: dict[str, dict] = {}
    offset = 0
    while offset < total:
        batch = fetch_page(PAGE, offset)
        if not batch:
            print(f"WARN  empty page at offset {offset}; stopping early")
            break
        for g in batch:
            seen[g["id"]] = g
        offset += PAGE

    # A COUNT IS A CLAIM. Say out loud whether we got what the API promised, because a short
    # fetch that prints "wrote N files" reads as complete.
    if len(seen) != total:
        print(f"WARN  fetched {len(seen)} distinct guide(s) but the API said {total} - NOT complete")
    else:
        print(f"ok    fetched all {len(seen)} guide(s)")

    (OUT / "_all-guides.json").write_text(
        json.dumps(sorted(seen.values(), key=lambda g: g["created_at"], reverse=True),
                   indent=2, ensure_ascii=False),
        encoding="utf-8")

    empty = 0
    for g in seen.values():
        tags = ", ".join(t["name"] for t in (g.get("tags") or []))
        body = g.get("guide") or ""
        if not body.strip():
            empty += 1
        header = (
            f"# {g['name']}\n\n"
            f"- id: `{g['id']}`\n"
            f"- author: {(g.get('user') or {}).get('username', 'unknown')}\n"
            f"- created: {g['created_at']}   updated: {g.get('updated_at')}\n"
            f"- views: {g.get('views')}\n"
            f"- tags: {tags or '(none)'}\n"
            f"- source: https://ficsit.app/guide/{g['id']}\n\n"
            f"> {g.get('short_description') or ''}\n\n---\n\n"
        )
        (OUT / slug(g["name"], g["id"])).write_text(header + body, encoding="utf-8")

    chars = sum(len(g.get("guide") or "") for g in seen.values())
    print(f"ok    wrote {len(seen)} .md + _all-guides.json to {OUT}")
    print(f"ok    {chars} chars of guide body total")
    # AN INSTRUMENT MUST PRINT ITS OWN COVERAGE - silence about scope reads as a clean bill.
    print(f"NOT CHECKED: {empty} guide(s) have an EMPTY body - the listing counts them but there "
          f"is nothing to read. This pulls the API's stored markdown ONLY: images, embedded video "
          f"and every external link inside a guide are NOT followed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
