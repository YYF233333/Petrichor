"""Sanity check: Ring Brick (StaticBody2D) vs Enemy (CharacterBody2D).

Prints several line counts for brick.gd and how much of it reappears in enemy.gd:
  physical code lines (non-blank, not comment-only), bag-of-lines overlap (comment-stripped, whitespace-normalised),
  and the window-based clone coverage from OUT_DIR/regions_{exact,near}.csv (one OUT_DIR per window size).
Usage: python ring_check.py --corpus CORPUS_DIR --api API_JSON OUT_DIR [OUT_DIR ...]
"""
import argparse
import csv
import os
import re

import dup

ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("--corpus", required=True)
ap.add_argument("--api", required=True)
ap.add_argument("outs", nargs="+")
args = ap.parse_args()
dup.init(args.api)

BASE = os.path.join(args.corpus, "Ring", "breakout", "scenes")
BRICK = os.path.join(BASE, "bricks", "base", "brick.gd")
ENEMY = os.path.join(BASE, "enemies", "base", "enemy.gd")


def code_lines(path):
    out = []
    for i, raw in enumerate(open(path, encoding="utf-8").read().splitlines(), 1):
        s = re.sub(r"#.*$", "", raw).strip()  # no '#' inside strings in these two files
        if s:
            out.append((i, re.sub(r"\s+", " ", s)))
    return out


b, e = code_lines(BRICK), code_lines(ENEMY)
eset = {t for _, t in e}
same = [(i, t) for i, t in b if t in eset]
ren = {t.replace("enemy", "brick") for _, t in e}
same_ren = [(i, t) for i, t in b if t in eset or t in ren]
print(f"brick.gd physical code lines: {len(b)}; enemy.gd: {len(e)}")
print(f"brick lines with an identical line anywhere in enemy.gd (bag overlap): {len(same)}; allowing brick_/enemy_ renaming: {len(same_ren)}")
nontriv = [ll for ll in dup.logical_lines(open(BRICK, encoding='utf-8').read()) if not dup.is_trivial(ll)]
print(f"brick.gd non-trivial logical lines (this survey's LLOC): {len(nontriv)}")
for d in args.outs:
    for mode in ("exact", "near"):
        p = os.path.join(d, f"regions_{mode}.csv")
        if not os.path.exists(p):
            continue
        rows = [r for r in csv.DictReader(open(p, encoding="utf-8")) if r["repo"] == "Ring"
                and {r["a"], r["b"]} == {"breakout/scenes/bricks/base/brick.gd", "breakout/scenes/enemies/base/enemy.gd"}]
        covered = set()
        for r in rows:
            side = "a" if r["a"].endswith("brick.gd") else "b"
            lo, hi = map(int, r[f"{side}_lines"].split("-"))
            covered |= {ll.line for ll in nontriv if lo <= ll.line <= hi}
        print(f"{os.path.basename(os.path.normpath(d))} {mode}: {len(rows)} regions; brick non-trivial logical lines inside a clone with enemy.gd: "
              f"{len(covered)}/{len(nontriv)} ({', '.join(r['a_lines'] + '~' + r['b_lines'] for r in rows)}); category {sorted({r['category'] for r in rows})}")
