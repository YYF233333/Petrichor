"""Composition workaround: first-party scripts placed as child nodes under hosts of different native types.

Uses the composed scene trees of gdcorpus.scene_contexts. For every first-party .gd attached to a non-root node
(own textual attachment or an instanced scene root), record the native type of its parent node. A script whose
parents span >= 2 native types with no ancestor relation between at least one pair (e.g. StaticBody2D and
CharacterBody2D) is counted as a cross-branch component; non-visual ones (script native Node) are reported separately.
Usage: python components.py --corpus CORPUS_DIR --api API_JSON --out OUT_DIR
  -> OUT_DIR/components.csv (scripts under >= 2 parent native types), OUT_DIR/components_summary.json
"""
import argparse
import collections
import csv
import json
import os

import gdcorpus as A
from gdcorpus import REPOS

GROUP_OF = {"game": "games", "demos": "demos", "plugin": "plugins/tools", "tool": "plugins/tools"}


def anc(a, b):
    return A.native_is(a, b) or A.native_is(b, a)


ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("--corpus", required=True)
ap.add_argument("--api", required=True)
ap.add_argument("--out", required=True)
args = ap.parse_args()
A.load_api(args.api)
os.makedirs(args.out, exist_ok=True)

rows = []
summary = collections.defaultdict(collections.Counter)
for name, url, sha, grp in REPOS:
    g = GROUP_OF[grp]
    R = A.Repo(args.corpus, name, grp)
    scripts, contexts = A.script_infos(R), A.scene_contexts(R)
    for f, ctxs in contexts.items():
        s = scripts.get(f)
        if not s or not s["first_party"] or not s["is_node"]:
            continue
        hosts = collections.Counter()
        for c in ctxs:
            n = c["node"]
            if n.parent is None or not n.parent.type or n.parent.type.startswith("?"):
                continue
            if c["how"] == "inherited":
                continue
            hosts[n.parent.type] += 1
        types = [t for t in hosts if t in A.NATIVE_PARENT]
        cross = any(not anc(a, b) for i, a in enumerate(types) for b in types[i + 1:])
        if len(types) >= 2:
            rows.append({"repo": name, "group": g, "script": f, "native": s["native"], "class_name": s["class_name"] or "",
                         "host_types": ";".join(f"{k}={v}" for k, v in hosts.most_common()), "cross_branch_hosts": cross})
        summary[g]["child_scripts"] += 1 if hosts else 0
        if len(types) >= 2:
            summary[g]["multi_host_type"] += 1
        if cross:
            summary[g]["cross_branch_hosts"] += 1
            if s["native"] == "Node":
                summary[g]["cross_branch_hosts_plain_Node"] += 1
with open(os.path.join(args.out, "components.csv"), "w", newline="", encoding="utf-8") as fh:
    w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()), lineterminator="\n")
    w.writeheader()
    w.writerows(rows)
KEYS = ("child_scripts", "multi_host_type", "cross_branch_hosts", "cross_branch_hosts_plain_Node")
plain = [{k: r[k] for k in ("group", "repo", "script", "host_types")} for r in rows
         if r["cross_branch_hosts"] and r["native"] == "Node" and r["group"] != "plugins/tools"]
with open(os.path.join(args.out, "components_summary.json"), "w", encoding="utf-8", newline="\n") as fh:
    json.dump({"summary": {g: {k: summary[g][k] for k in KEYS} for g in ("games", "demos", "plugins/tools")},
               "plain_node_cross_branch": plain}, fh, indent=1)
for g in ("games", "demos", "plugins/tools"):
    print(g, {k: summary[g][k] for k in KEYS})
for x in plain:
    print(" ", x["repo"], x["script"], x["host_types"])
