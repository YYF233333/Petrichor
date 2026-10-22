"""Aggregate OUT_DIR/summary.json (+ groups/regions, + labels.csv for K = 5) into OUT_DIR/report.txt.

Usage: python report.py OUT_DIR [--labels LABELS_CSV]   (default: labels.csv next to this script)
"""
import argparse
import collections
import csv
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
_ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
_ap.add_argument("out")
_ap.add_argument("--labels", default=os.path.join(HERE, "labels.csv"))
_args = _ap.parse_args()
OUT = _args.out
S = json.load(open(os.path.join(OUT, "summary.json"), encoding="utf-8"))
GROUPS = ["games", "demos", "plugins/tools"]
MODES = [("exact", "X"), ("near", "N"), ("loose", "L")]
CATS = ["cross_branch", "cross_forced", "cross_lowerable", "same_native_unrelated", "same_chain", "non_node", "tool_test", "intra_unit", "unresolved"]
KINDS = ["export", "onready", "signal", "member", "lifecycle", "per_frame", "input", "handler", "method", "other_virtual", "class_other"]
lines = []


def p(*a):
    lines.append(" ".join(str(x) for x in a))


def rd(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


G = {m: rd(os.path.join(OUT, f"groups_{m}.csv")) for m, _ in MODES}
RG = {m: rd(os.path.join(OUT, f"regions_{m}.csv")) for m, _ in MODES}

p(f"K = {S['K']} non-trivial logical lines per window. X = exact, N = near (locals+literals normalized), L = loose (all non-engine identifiers blind)")
p("\n== Per repo (LLOC = non-trivial logical lines, first-party; dup lines count every copy; cb = cross-branch)")
p("   snX/scX = exact lines in same_native_unrelated / same_chain pairs")
p(f"{'repo':26s} {'grp':6s} {'commit':8s} {'LLOC':>6s} {'dupX':>5s} {'cbX':>4s} {'cbX/k':>5s} {'cbN':>4s} {'cbN/k':>5s} {'cbL':>4s} {'gX':>3s} {'gN':>3s} {'prX':>3s} {'frcX':>4s} {'snX':>5s} {'scX':>5s}  top native pairs (exact groups)")
tot = collections.defaultdict(collections.Counter)
for r in S["repos"]:
    g = r["group"]
    ex, nr, lo = r["exact"], r["near"], r["loose"]
    cbx = ex["lines"].get("cross_branch", 0)
    cbn = nr["lines"].get("cross_branch", 0)
    pairs = list(ex.get("cross_native_pairs", {}).items())[:3]
    p(f"{r['repo'][:26]:26s} {g[:6]:6s} {r['commit'][:7]:8s} {r['lloc']:6d} {ex['lines'].get('any', 0):5d} {cbx:4d} {1000 * cbx / max(1, r['lloc']):5.1f} "
      f"{cbn:4d} {1000 * cbn / max(1, r['lloc']):5.1f} {lo['lines'].get('cross_branch', 0):4d} {ex['groups']['cross_branch']:3d} {nr['groups']['cross_branch']:3d} "
      f"{ex.get('pairs_with_clones', {}).get('cross_branch', 0):3d} {ex['lines'].get('cross_forced', 0):4d} "
      f"{ex['lines'].get('same_native_unrelated', 0):5d} {ex['lines'].get('same_chain', 0):5d}  "
      + ", ".join(f"{k} x{v}" for k, v in pairs))
    t = tot[g]
    t["lloc"] += r["lloc"]
    t["units"] += r["units"]
    for mode, M in MODES:
        d = r[mode]
        for c, v in d["lines"].items():
            t[f"{M}_{c}"] += v
        for c, v in d["exclusive"].items():
            t[f"{M}_excl_{c}"] += v
        for c, v in d.get("pairs_with_clones", {}).items():
            t[f"{M}_pairs_{c}"] += v
        for c, v in d["groups"].items():
            t[f"{M}_groups_{c}"] += v
        for k, v in d.get("cross_kinds", {}).items():
            t[f"{M}_kind_{k}"] += v
        t[f"{M}_connect"] += d.get("cross_connect_lines", 0)
    for c, v in r["pair_pop"].items():
        t[f"pop_{c}"] += v

p("\n== Totals by group: lines (all copies) / lines whose only clone category is this one / unit pairs with >=1 clone")
for g in GROUPS:
    t = tot[g]
    p(f"-- {g}: LLOC {t['lloc']}, units {t['units']}, dup lines X {t['X_any']} ({1000 * t['X_any'] / max(1, t['lloc']):.0f}/k), "
      f"N {t['N_any']} ({1000 * t['N_any'] / max(1, t['lloc']):.0f}/k), L {t['L_any']} ({1000 * t['L_any'] / max(1, t['lloc']):.0f}/k)")
    p(f"   {'category':22s} {'linX':>5s} {'exX':>5s} {'linN':>5s} {'exN':>5s} {'linL':>5s} {'popPairs':>8s} {'prX':>4s} {'prN':>4s} {'prL':>4s} {'prX/1k':>6s} {'prN/1k':>6s} {'gX':>4s}")
    for c in CATS:
        pop = t[f"pop_{c}"] if c not in ("cross_forced", "cross_lowerable") else 0
        px, pn = t[f"X_pairs_{c}"], t[f"N_pairs_{c}"]
        p(f"   {c:22s} {t[f'X_{c}']:5d} {t[f'X_excl_{c}']:5d} {t[f'N_{c}']:5d} {t[f'N_excl_{c}']:5d} {t[f'L_{c}']:5d} {pop:8d} {px:4d} {pn:4d} {t[f'L_pairs_{c}']:4d} "
          f"{(1000 * px / pop if pop else 0):6.2f} {(1000 * pn / pop if pop else 0):6.2f} {t.get(f'X_groups_{c}', 0):4d}")
    for mode, M in MODES[:2]:
        p(f"   cross-branch lines by kind ({mode}): " + ", ".join(f"{k}={t[f'{M}_kind_{k}']}" for k in KINDS if t[f'{M}_kind_{k}'])
          + f"; of which contain .connect: {t[f'{M}_connect']}")

for r in S["repos"]:
    if r["repo"] == "godot-demo-projects":
        for mode, _ in MODES[:2]:
            rows = RG[mode]
            projs = {x["project"] for x in rows if x["repo"] == "godot-demo-projects" and x["category"] == "cross_branch"}
            anyp = {x["project"] for x in rows if x["repo"] == "godot-demo-projects"}
            p(f"demo projects with first-party code: {r['projects_with_code']}; with any clone ({mode}): {len(anyp)}; with cross-branch clone: {len(projs)} {sorted(projs)}")

p("\n== Cross-branch native pairs (number of exact groups / near groups)")
for g in GROUPS:
    cx, cn = collections.Counter(), collections.Counter()
    for rows, c in ((G["exact"], cx), (G["near"], cn)):
        for row in rows:
            if row["group"] == g:
                for pr in row["native_pairs"].split(";"):
                    c[pr] += 1
    p(f"-- {g}: " + ", ".join(f"{k} {v}/{cn.get(k, 0)}" for k, v in cx.most_common(12)))
    only_n = [(k, v) for k, v in cn.most_common() if k not in cx][:8]
    if only_n:
        p("   near-only: " + ", ".join(f"{k} {v}" for k, v in only_n))

p("\n== Cross-branch clone pairs (distinct unit pairs)")
for g in GROUPS:
    for mode, _ in MODES:
        rows = RG[mode]
        pr = {(x["repo"], x["a"], x["b"]) for x in rows if x["group"] == g and x["category"] == "cross_branch"}
        prf = {(x["repo"], x["a"], x["b"]) for x in rows if x["group"] == g and x["category"] == "cross_branch" and x["sub"] == "cross_forced"}
        units = {(x[0], x[1]) for x in pr} | {(x[0], x[2]) for x in pr}
        repos = collections.Counter(x[0] for x in pr)
        p(f"-- {g} {mode}: {len(pr)} unit pairs ({len(prf)} forced by name evidence), {len(units)} units; by repo "
          + ", ".join(f"{k} {v}" for k, v in sorted(repos.items(), key=lambda kv: (-kv[1], kv[0]))))

lab = rd(_args.labels) if S["K"] == 5 else []  # the labels were made for the K = 5 exact groups
if lab:
    p("\n== Mechanism labels (manual judgement) for reviewed exact cross-branch groups")
    have = {(r["repo"], r["members"].split(";")[0], r["size_all_copies"]) for r in G["exact"]}
    want = {(r["repo"], r["example"], r["size_all_copies"]) for r in lab}
    if have != want:
        p(f"   WARNING: labels.csv does not match groups_exact.csv ({len(want - have)} labels without a group, "
          f"{len(have - want)} groups without a label); rerun make_labels.py")
    by = collections.defaultdict(collections.Counter)
    lines_by = collections.defaultdict(collections.Counter)
    for row in lab:
        by[row["group"]][row["label"]] += 1
        lines_by[row["group"]][row["label"]] += int(row["size_all_copies"])
    for g in GROUPS:
        tot_l = sum(lines_by[g].values())
        p(f"-- {g}: reviewed {sum(by[g].values())} groups / {tot_l} lines: " + ", ".join(f"{k}={v} ({lines_by[g][k]} lines)" for k, v in sorted(by[g].items())))
    p("   lines by label and repo; lines in groups with name evidence (a group has it if any of its unit pairs has)")
    for g in GROUPS:
        per_repo = collections.defaultdict(collections.Counter)
        frc = collections.Counter()
        for row in lab:
            if row["group"] == g:
                per_repo[row["label"]][row["repo"]] += int(row["size_all_copies"])
                if row["forced"] == "True":
                    frc[row["label"]] += int(row["size_all_copies"])
        for k in sorted(per_repo):
            p(f"   {g} {k}: " + ", ".join(f"{rp} {v}" for rp, v in sorted(per_repo[k].items(), key=lambda kv: (-kv[1], kv[0])))
              + f"; with name evidence {frc[k]} of {sum(per_repo[k].values())}")
    p("   lines by native relation of the group (ancestor = one native class is an ancestor of the other)")
    for g in GROUPS:
        rel = collections.Counter()
        for r in G["exact"]:
            if r["group"] == g:
                rel[r["relation"]] += int(r["size_all_copies"])
        p(f"   {g}: " + ", ".join(f"{k} {v}" for k, v in sorted(rel.items())))

p("\n== Heuristic pre-labels (exact cross-branch groups; automatic, see dup.py)")
for g in GROUPS:
    c, cl = collections.Counter(), collections.Counter()
    for r in G["exact"]:
        if r["group"] == g:
            c[r["heuristic"]] += 1
            cl[r["heuristic"]] += int(r["size_all_copies"])
    p(f"-- {g}: " + ", ".join(f"{k}={v} ({cl[k]} lines)" for k, v in sorted(c.items())))

p("\n== Shared capability: cross-branch Node unit pairs (no common script ancestor, different natives) declaring the same public")
p("   non-engine func / non-@onready var / signal; 'consumed' = the name is accessed on a non-self receiver somewhere in the project")
for g in GROUPS:
    c, mech = collections.Counter(), collections.Counter()
    for r in S["repos"]:
        if r["group"] != g:
            continue
        cap = r["capability"]
        for k in ("node_units", "cross_pairs_declaring_same", "cross_pairs_sharing_1plus", "cross_pairs_sharing_2plus", "cross_shared_names", "cross_shared_names_consumed"):
            c[k] += cap[k]
        mech.update(cap["consumer_mechanisms"])
    p(f"-- {g}: node units {c['node_units']}; pairs declaring a common name {c['cross_pairs_declaring_same']}, of which a consumed name {c['cross_pairs_sharing_1plus']}, "
      f">=2 consumed names {c['cross_pairs_sharing_2plus']}; (project,name) {c['cross_shared_names']}, consumed {c['cross_shared_names_consumed']}")
    p("   consumer sites by mechanism: " + ", ".join(f"{k}={v}" for k, v in mech.most_common()) + f"; total {sum(mech.values())}")
CONS = rd(os.path.join(OUT, "consumers.csv"))
CAPN = rd(os.path.join(OUT, "capability_names.csv"))
GROUP_OF_REPO = {r["repo"]: r["group"] for r in S["repos"]}
EXPLICIT = ("group_then_call", "owner_or_parent", "is_A_or_B", "has_method_guard", "string_has_method")
for g in GROUPS:
    parts = []
    for m in EXPLICIT:
        c = collections.Counter(x["repo"] for x in CONS if x["mechanism"] == m and GROUP_OF_REPO.get(x["repo"]) == g)
        if c:
            parts.append(f"{m}: " + ", ".join(f"{k} {v}" for k, v in sorted(c.items(), key=lambda kv: (-kv[1], kv[0]))))
    p(f"   {g} explicit cross-type sites by repo: " + "; ".join(parts))
    top = sorted((x for x in CAPN if GROUP_OF_REPO.get(x["repo"]) == g), key=lambda x: (-int(x["consumer_sites"]), x["repo"], x["name"]))[:6]
    p(f"   {g} shared names with most consumer sites: " + ", ".join(f"{x['repo']}:{x['name']} {x['consumer_sites']}" for x in top))

W = json.load(open(os.path.join(OUT, "workarounds.json"), encoding="utf-8"))
p("\n== Workaround indicators (first-party, non-test)")
for g in GROUPS:
    c = collections.Counter()
    by_repo = collections.defaultdict(dict)
    for r in S["repos"]:
        if r["group"] == g:
            c.update({k: v for k, v in W[r["repo"]].items() if isinstance(v, int)})
            for k, v in W[r["repo"]].items():
                if v:
                    by_repo[k][r["repo"]] = v
    p(f"-- {g}: " + ", ".join(f"{k}={v}" for k, v in sorted(c.items())))
    for k in sorted(by_repo):
        p(f"   {k}: " + ", ".join(f"{rp} {v}" for rp, v in sorted(by_repo[k].items(), key=lambda kv: (-kv[1], kv[0]))))

CS = os.path.join(OUT, "components_summary.json")
if os.path.exists(CS):
    C = json.load(open(CS, encoding="utf-8"))
    p("\n== Components (components.py): first-party Node scripts attached as child nodes, by parent native type")
    for g in GROUPS:
        p(f"-- {g}: " + ", ".join(f"{k}={v}" for k, v in C["summary"].get(g, {}).items()))
    for x in C["plain_node_cross_branch"]:
        p(f"   {x['group']} plain-Node cross-branch component: {x['repo']} {x['script']} {x['host_types']}")

with open(os.path.join(OUT, "report.txt"), "w", encoding="utf-8", newline="\n") as fh:
    fh.write("\n".join(lines) + "\n")
print("\n".join(lines))
