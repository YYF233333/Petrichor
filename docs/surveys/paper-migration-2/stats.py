"""Aggregate the ledger of the second paper migration.

Usage: python stats.py FPS_DIR RING_DIR

FPS_DIR is a checkout of KenneyNL/Starter-Kit-FPS, RING_DIR a checkout of YYF233333/Ring (commits in the
survey write-up). The originals are read only to count code lines; every other number comes from ledger.tsv.
"""
import csv
import os
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))

FPS_FILES = ["objects/player.gd", "objects/enemy.gd", "objects/cloud.gd", "objects/impact.gd",
             "scripts/audio.gd", "scripts/hud.gd", "scripts/weapon.gd"]
RING_FILES_V01 = ["breakout/scenes/" + p for p in [
    "managers/breakout_manager.gd", "components/health_component.gd",
    "components/death_component.gd", "components/interval_hurt_component.gd",
    "bricks/base/brick.gd", "bricks/sturdy_brick/sturdy_brick.gd",
    "bricks/indestructible_brick/indestructible_brick.gd",
    "enemies/base/enemy.gd", "utility/utility.gd"]]
# Subclasses that the first migration left in GDScript; GDScript cannot extend a class of this language.
RING_FILES_SUB = ["breakout/scenes/" + p for p in [
    "bricks/basic_brick/basic_brick.gd", "bricks/charge_brick/charge_brick.gd",
    "bricks/heal_brick/heal_brick.gd", "enemies/basic_enemy/basic_enemy.gd",
    "enemies/oppressive_enemy/oppressive_enemy.gd"]]

COST = {"annotation", "pub", "conversion", "rewrite", "decision", "narrow", "gap", "glue"}


def code_lines(path):
    """Non-blank lines that are not comments."""
    with open(path, encoding="utf-8") as f:
        return sum(1 for line in f if line.strip() and not line.strip().startswith("#"))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    fps_dir, ring_dir = sys.argv[1], sys.argv[2]
    with open(os.path.join(HERE, "ledger.tsv"), encoding="utf-8") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    for r in rows:
        r["n"] = int(r["n"])
        r["proj"] = r["file"].split("/")[0]

    ring_v01 = sum(code_lines(os.path.join(ring_dir, p)) for p in RING_FILES_V01)
    ring_sub = sum(code_lines(os.path.join(ring_dir, p)) for p in RING_FILES_SUB)
    loc = {"fps": sum(code_lines(os.path.join(fps_dir, p)) for p in FPS_FILES), "ring": ring_v01 + ring_sub}
    loc["all"] = loc["fps"] + loc["ring"]
    print(f"code lines: fps {loc['fps']}, ring {loc['ring']} ({ring_v01} as in the first migration "
          f"+ {ring_sub} in subclasses), total {loc['all']}")

    def table(title, key, keep):
        by = defaultdict(Counter)
        for r in rows:
            if keep(r):
                by[key(r)][r["proj"]] += r["n"]
        print(f"\n## {title}\n| | fps | ring | total |\n|---|---:|---:|---:|")
        for k in sorted(by, key=lambda k: (-sum(by[k].values()), k)):
            c = by[k]
            print(f"| {k} | {c['fps']} | {c['ring']} | {c['fps'] + c['ring']} |")

    table("by category", lambda r: r["cat"], lambda r: True)
    table("cost by source", lambda r: f"{r['cat']}/{r['src']}", lambda r: r["cat"] in COST)
    table("cost by kind (M mechanical, J judgment)", lambda r: r["kind"], lambda r: r["cat"] in COST)

    def cost(proj, keep):
        return sum(r["n"] for r in rows if r["cat"] in COST and keep(r) and proj in ("all", r["proj"]))

    views = [
        ("all cost", lambda r: True),
        ("without pub", lambda r: r["cat"] != "pub"),
        ("without pub and override signatures", lambda r: r["cat"] != "pub" and r["src"] != "override-signature"),
        ("judgment only", lambda r: r["kind"] == "J"),
    ]
    print("\n## cost\n| | fps | ring | total | per 100 lines |\n|---|---:|---:|---:|---:|")
    for name, keep in views:
        f, g, t = cost("fps", keep), cost("ring", keep), cost("all", keep)
        print(f"| {name} | {f} | {g} | {t} | {t * 100 / loc['all']:.1f} |")

    narrow = sum(r["n"] for r in rows if r["cat"] == "narrow")
    member_option = sum(r["n"] for r in rows if r["src"] == "member-option" and r["cat"] in COST)
    print(f"\nlocals added because members do not narrow: {narrow} "
          f"({narrow * 1000 / loc['all']:.1f} per 1,000 lines); all costs from Option-typed members: {member_option}")


if __name__ == "__main__":
    main()
