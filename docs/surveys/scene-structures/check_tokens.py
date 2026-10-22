"""Cross-check tokenizer-based counts against naive regexes (sanity check only).

Usage: python check_tokens.py CORPUS OUT     (after analyze.py)
"""
import csv
import os
import re
import sys

import analyze as A
from fetch_corpus import REPOS

if len(sys.argv) < 3:
    sys.exit(__doc__)
corpus, out = sys.argv[1], sys.argv[2]
rows = {(r["repo"], r["script"]): r for r in csv.DictReader(open(os.path.join(out, "scripts.csv"), encoding="utf-8"))}
for name, url, sha, g in REPOS:
    R = A.Repo(name, g, corpus)
    d = gn = td = tg = u = tu = 0
    for f in R.files:
        if not f.endswith(".gd") or (name, f) not in rows:
            continue
        src = R.read(f)
        code = "\n".join(l if '"' in l or "'" in l else l.split("#")[0] for l in src.split("\n"))
        d += len(re.findall(r"\$[A-Za-z_\"%/']", code))
        gn += len(re.findall(r"(?<![.\w])get_node(?:_or_null)?\(\s*[\"^]", code))
        u += len(re.findall(r"(?<![\w)\]\"'\s])\s*%[A-Za-z_]", code))
        r = rows[(name, f)]
        td += int(r["dollar"])
        tg += int(r["get_node_lit"]) + int(r["get_node_or_null_lit"])
        tu += int(r["unique"])
    print(f"{name}: regex $ {d} / tok {td} | regex get_node(lit) {gn} / tok {tg} | regex %X~{u} / tok {tu}")
