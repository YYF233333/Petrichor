"""Aggregate OUT/*.csv + OUT/repos.json into OUT/report.txt and OUT/multi_games.txt.

Usage: python report.py CORPUS OUT     (after analyze.py; CORPUS is read only to detect license files)
"""
import collections
import csv
import json
import os
import sys

if len(sys.argv) < 3:
    sys.exit(__doc__)
CORPUS, OUT = sys.argv[1], sys.argv[2]

repos = json.load(open(os.path.join(OUT, "repos.json"), encoding="utf-8"))
rows = list(csv.DictReader(open(os.path.join(OUT, "scripts.csv"), encoding="utf-8")))
uses = list(csv.DictReader(open(os.path.join(OUT, "uses.csv"), encoding="utf-8")))
I = int


def license_of(repo):
    d = os.path.join(CORPUS, repo)
    for f in sorted(os.listdir(d)):
        if f.lower().startswith(("license", "copying")) and os.path.isfile(os.path.join(d, f)):
            t = open(os.path.join(d, f), encoding="utf-8", errors="replace").read(3000)
            for key, name in (("MIT License", "MIT"), ("Permission is hereby granted, free of charge", "MIT"),
                              ("Apache License", "Apache-2.0"), ("GNU GENERAL PUBLIC", "GPL"), ("CC0", "CC0")):
                if key in t:
                    return name
            return "?" + f
    return "none"


lines = []
P = lines.append
P("repo | group | commit | license | gd(1st-party) | lines | tscn | b0 | b1 | b2 | b3+ | inh(tscn/model) | inhStruct | editable | lit-scripts | struct>=2 own/all | lit&struct>=2 | nonroot-attach")
by_repo = collections.defaultdict(list)
for r in rows:
    by_repo[r["repo"]].append(r)
for rs in repos:
    rr = by_repo[rs["repo"]]
    b = collections.Counter(r["bucket"] for r in rr)
    lit = sum(1 for r in rr if I(r["lit_uses"]) > 0)
    s_own = sum(1 for r in rr if I(r["n_struct_own"]) >= 2)
    s_all = sum(1 for r in rr if I(r["n_struct_all"]) >= 2)
    ls = sum(1 for r in rr if I(r["n_struct_all"]) >= 2 and I(r["lit_uses"]) > 0)
    nonroot = sum(I(r["n_text_nonroot"]) for r in rr)
    att = sum(I(r["n_text_attach"]) for r in rr)
    P(f"{rs['repo']} | {rs['group']} | {rs['commit'][:10]} | {license_of(rs['repo'])} | {len(rr)} | {rs['gd_lines_first_party']} | {rs['tscn']} | "
      f"{b['0']} | {b['1']} | {b['2']} | {b['3+']} | {rs['inherited_scenes']} ({rs['inherited_from_tscn']}/{rs['inherited_from_model']}) | "
      f"{rs['inherited_scenes_with_structural_change']} | {rs['editable_entries']} | {lit} | {s_own}/{s_all} | {ls} | {nonroot}/{att}")
P("")


def group_of(g):
    return {"game": "games", "demos": "demos"}.get(g, "plugins/tools")


for grp in ("games", "demos", "plugins/tools"):
    rr = [r for r in rows if group_of(r["group"]) == grp]
    uu = [u for u in uses if group_of(u["group"]) == grp]
    n = len(rr)
    attached = [r for r in rr if I(r["n_ctx_all"]) > 0]
    lit = [r for r in rr if I(r["lit_uses"]) > 0]
    multi = [r for r in rr if I(r["n_struct_all"]) >= 2]
    multi_own = [r for r in rr if I(r["n_struct_own"]) >= 2]
    lit_multi = [r for r in multi if I(r["lit_uses"]) > 0]
    b = collections.Counter(r["bucket"] for r in rr)
    P(f"== {grp}: {n} first-party scripts; buckets 0/1/2/3+ = {b['0']}/{b['1']}/{b['2']}/{b['3+']}; with any scene context (own or subclass) {len(attached)}")
    P(f"   scripts with literal self node paths: {len(lit)}")
    P(f"   scripts seeing >=2 structures: own {len(multi_own)}, incl. subclass attachments {len(multi)}; of those using literal paths {len(lit_multi)}")
    P(f"   HEADLINE-1 lit&multi = {len(lit_multi)} / all {n} = {len(lit_multi)/max(n,1):.1%}; / scene-context scripts {len(attached)} = {len(lit_multi)/max(len(attached),1):.1%}; / literal-path scripts {len(lit)} = {len(lit_multi)/max(len(lit),1):.1%}")
    multi_set = {(r["repo"], r["script"]) for r in multi}
    um = [u for u in uu if (u["repo"], u["script"]) in multi_set]
    st_all = collections.Counter(u["status"] for u in uu)
    st_m = collections.Counter(u["status"] for u in um)
    diff_m = st_m["differs_native"] + st_m["differs_script"]
    resolved2 = sum(1 for u in um if I(u["n_resolved"]) >= 2)
    P(f"   literal uses total {len(uu)}: " + ", ".join(f"{k}={v}" for k, v in st_all.most_common()))
    P(f"   literal uses in >=2-structure scripts {len(um)}: " + ", ".join(f"{k}={v}" for k, v in st_m.most_common()))
    P(f"   HEADLINE-2 differs = {diff_m} / uses in multi-structure scripts {len(um)} = {diff_m/max(len(um),1):.1%}; / those resolved in >=2 contexts {resolved2} = {diff_m/max(resolved2,1):.1%}; / all literal uses {len(uu)} = {diff_m/max(len(uu),1):.1%}")
    mech = collections.Counter()
    for r in multi:
        for m in r["mech"].split("|"):
            if m:
                mech[m] += 1
    P("   mechanisms among >=2-structure scripts: " + ", ".join(f"{k}={v}" for k, v in sorted(mech.items())))
    zr = collections.Counter(r["zero_reason"] for r in rr if r["bucket"] == "0")
    P("   bucket-0 reasons: " + ", ".join(f"{k}={v}" for k, v in zr.most_common()))
    lit0 = [r for r in rr if r["bucket"] == "0" and I(r["n_ctx_all"]) == 0 and I(r["lit_uses"]) > 0]
    P(f"   scripts with NO scene context but literal self paths: {len(lit0)} (" + ", ".join(f"{k}={v}" for k, v in collections.Counter(r['zero_reason'] for r in lit0).most_common()) + ")")
    acc = collections.Counter()
    for r in rr:
        for k in ("dollar", "unique", "get_node_lit", "get_node_or_null_lit", "get_node_nonlit", "other_recv_lit", "has_node_lit",
                  "find_child", "export_node", "export_nodepath", "dotdot", "root_paths", "owner_dot", "get_parent", "set_script"):
            acc[k] += I(r[k])
            acc["scripts_" + k] += 1 if I(r[k]) else 0
    P("   access counts (uses/scripts): " + ", ".join(f"{k}={acc[k]}/{acc['scripts_' + k]}" for k in
                                                     ("dollar", "unique", "get_node_lit", "get_node_or_null_lit", "get_node_nonlit", "other_recv_lit",
                                                      "has_node_lit", "find_child", "export_node", "export_nodepath", "dotdot", "root_paths", "owner_dot", "get_parent", "set_script")))
    P("")

# per-repo headline breakdown for games and demos
P("per-repo literal-use statuses (all scripts):")
for rs in repos:
    uu = [u for u in uses if u["repo"] == rs["repo"]]
    P(f"  {rs['repo']}: " + ", ".join(f"{k}={v}" for k, v in collections.Counter(u['status'] for u in uu).most_common()))
open(os.path.join(OUT, "report.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
print("\n".join(lines))

# listing of >=2-structure scripts in games for manual purpose classification
ml = []
for r in rows:
    if r["group"] != "game" or I(r["n_struct_all"]) < 2:
        continue
    ml.append(f"{r['repo']}/{r['script']} [{r['lines']}L] extends={r['extends']} class={r['class_name']} native={r['native']} "
              f"scenes={r['n_scenes']} ctx={r['n_ctx_own']}/{r['n_ctx_all']} struct={r['n_struct_own']}/{r['n_struct_all']} subs={r['n_subclasses']} "
              f"mech={r['mech']} lit={r['lit_uses']} st={r['st']}")
open(os.path.join(OUT, "multi_games.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(ml) + "\n")
