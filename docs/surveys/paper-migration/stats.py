"""Aggregate the paper-migration ledger.

Usage: python stats.py FPS_DIR RING_DIR

FPS_DIR is a checkout of KenneyNL/Starter-Kit-FPS, RING_DIR a checkout of YYF233333/Ring
(commits in the survey write-up). The originals are read only to count code lines and,
for Ring, autoload accesses; every other number comes from ledger.tsv.
"""
import csv
import os
import re
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))

FPS_FILES = ["objects/player.gd", "objects/enemy.gd", "objects/cloud.gd", "objects/impact.gd",
             "scripts/audio.gd", "scripts/hud.gd", "scripts/weapon.gd"]
RING_FILES = ["breakout/scenes/" + p for p in [
    "managers/breakout_manager.gd", "components/health_component.gd",
    "components/death_component.gd", "components/interval_hurt_component.gd",
    "bricks/base/brick.gd", "bricks/sturdy_brick/sturdy_brick.gd",
    "bricks/indestructible_brick/indestructible_brick.gd",
    "enemies/base/enemy.gd", "utility/utility.gd"]]
RING_AUTOLOADS = ["BreakoutManager", "ValueManager", "ResourceManager", "Utility", "DialogueManager"]

# Categories that count as migration cost. undefined, silent-narrowing and benefit are reported apart.
COST = {"annotation", "rename", "conversion", "rewrite", "decision", "gap", "glue"}


def code_lines(path):
    """Non-blank lines that are not comments."""
    with open(path, encoding="utf-8") as f:
        return sum(1 for line in f if line.strip() and not line.strip().startswith("#"))


def autoload_uses(path, names):
    with open(path, encoding="utf-8") as f:
        text = "\n".join(l for l in f.read().splitlines() if not l.strip().startswith("#"))
    return sum(len(re.findall(rf"\b{n}\.", text)) for n in names)


def main():
    fps_dir, ring_dir = sys.argv[1], sys.argv[2]
    with open(os.path.join(HERE, "ledger.tsv"), encoding="utf-8") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    for r in rows:
        r["n"] = int(r["n"])
        r["proj"] = r["file"].split("/")[0]

    # FPS autoload accesses (Audio) are itemized in the ledger; Ring's are counted here.
    ring_auto = sum(autoload_uses(os.path.join(ring_dir, p), RING_AUTOLOADS) for p in RING_FILES)
    rows.append(dict(file="ring/*", line="-", cat="undefined", src="autoload", n=ring_auto,
                     conv="A6", note="autoload accesses", proj="ring"))

    loc = {"fps": sum(code_lines(os.path.join(fps_dir, p)) for p in FPS_FILES),
           "ring": sum(code_lines(os.path.join(ring_dir, p)) for p in RING_FILES)}
    loc["all"] = loc["fps"] + loc["ring"]
    print(f"code lines: fps {loc['fps']}, ring {loc['ring']}, total {loc['all']}")

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
    table("cost by source (v0.1)", lambda r: r["src"], lambda r: r["cat"] in COST)
    table("cost by the convention that removes it", lambda r: r["conv"], lambda r: r["cat"] in COST)

    def cost(proj, removed):
        return sum(r["n"] for r in rows
                   if r["cat"] in COST and r["conv"] not in removed and proj in ("all", r["proj"]))

    a = {f"A{i}" for i in range(1, 9)}
    scenarios = [
        ("v0.1", set()),
        ("v0.1 + A1", {"A1"}),
        ("+ A1-A8", a),
        ("+ A1-A8 + C-int", a | {"C-int"}),
        ("+ A1-A8 + C-int + C-scene", a | {"C-int", "C-scene"}),
        ("+ all A and C", a | {"C-int", "C-scene", "C-is", "C-r3", "C-trait"}),
    ]
    print("\n## cost by scenario\n| | fps | ring | total | per 100 lines |\n|---|---:|---:|---:|---:|")
    for name, removed in scenarios:
        f, g, t = cost("fps", removed), cost("ring", removed), cost("all", removed)
        print(f"| {name} | {f} | {g} | {t} | {t * 100 / loc['all']:.1f} |")


if __name__ == "__main__":
    main()
