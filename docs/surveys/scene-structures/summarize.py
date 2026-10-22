"""Extra aggregates for the write-up: mechanisms, purposes, option-A/B stressors. Writes OUT/summary.txt.

Usage: python summarize.py OUT     (after analyze.py; reads purpose_labels.csv next to this script)
"""
import collections
import csv
import json
import os
import sys

if len(sys.argv) < 2:
    sys.exit(__doc__)
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1]
rows = list(csv.DictReader(open(os.path.join(OUT, "scripts.csv"), encoding="utf-8")))
uses = list(csv.DictReader(open(os.path.join(OUT, "uses.csv"), encoding="utf-8")))
repos = json.load(open(os.path.join(OUT, "repos.json"), encoding="utf-8"))
ctxs = list(csv.DictReader(open(os.path.join(OUT, "contexts.csv"), encoding="utf-8")))
labels = {(r["repo"], r["script"]): r["purpose"] for r in csv.DictReader(open(os.path.join(HERE, "purpose_labels.csv"), encoding="utf-8"))}
I = int
G = {"game": "games", "demos": "demos"}
L = []
P = L.append

for grp in ("games", "demos", "plugins/tools"):
    rr = [r for r in rows if G.get(r["group"], "plugins/tools") == grp]
    uu = [u for u in uses if G.get(u["group"], "plugins/tools") == grp]
    rg = [x for x in repos if G.get(x["group"], "plugins/tools") == grp]
    multi = [r for r in rr if I(r["n_struct_all"]) >= 2]
    single = [r for r in rr if I(r["n_ctx_all"]) > 0 and I(r["n_struct_all"]) < 2]
    P(f"== {grp}")
    b2 = [r for r in rr if r["bucket"] in ("2", "3+")]
    P(f"  textual 2+ scene files: {len(b2)} scripts; of these with >=2 distinct structures among the textual attachments: {sum(1 for r in b2 if 'M1_text2+_diff' in r['mech'])}")
    inh = [r for r in rr if "M2_inherited" in r["mech"]]
    P(f"  attached in a base scene that has inherited scenes: {len(inh)}; inherited structure differs: {sum(1 for r in inh if 'M2_inherited_diff' in r['mech'])}")
    P(f"  instanced elsewhere with modified subtree (M3): {sum(1 for r in rr if 'M3' in r['mech'])}")
    P(f"  script set on an instanced-scene node (M4): {sum(1 for r in rr if 'M4' in r['mech'])}; text attachments of that kind: {sum(I(r['n_text_override_inst']) for r in rr)}")
    P(f"  inherited scene root given its own script (override_inh attachments): {sum(I(r['n_text_override_inh']) for r in rr)}")
    P(f"  subclass attachments add structures (M5): {sum(1 for r in rr if 'M5' in r['mech'])}; of which script has no own attachment: {sum(1 for r in rr if 'M5' in r['mech'] and I(r['n_ctx_own']) == 0)}")
    P(f"  multi-node in one scene with different subtrees (M1b): {sum(1 for r in rr if 'M1b' in r['mech'])}")
    pi = [r for r in rr if I(r["plain_instances"]) > 0]
    P(f"  plain instancing (unchanged copies elsewhere): {len(pi)} scripts, {sum(I(r['plain_instances']) for r in rr)} instance contexts; of those scripts only-plain (single structure): {sum(1 for r in pi if I(r['n_struct_all']) < 2)}")
    att = sum(I(r["n_text_attach"]) for r in rr)
    nr = sum(I(r["n_text_nonroot"]) for r in rr)
    P(f"  textual attachments on non-root nodes: {nr}/{att} = {nr/max(att,1):.0%}")
    for name, sel in (("multi", multi), ("single", single)):
        lit = sum(1 for r in sel if I(r["lit_uses"]) > 0)
        gc = sum(1 for r in sel if I(r["get_children"]) > 0)
        ex = sum(1 for r in sel if I(r["export_node"]) + I(r["export_nodepath"]) > 0)
        P(f"  {name}-structure scripts {len(sel)}: literal paths {lit} ({lit/max(len(sel),1):.0%}), get_child(ren) {gc} ({gc/max(len(sel),1):.0%}), Node/NodePath exports {ex} ({ex/max(len(sel),1):.0%})")
    ms = {(r["repo"], r["script"]) for r in multi}
    um = [u for u in uu if (u["repo"], u["script"]) in ms]
    P(f"  multi-structure literal uses resolved in all contexts to one type: {sum(1 for u in um if u['status']=='consistent' and I(u['n_resolved'])==I(u['n_contexts']))}/{len(um)}; consistent-but-partial (some contexts OUTSIDE/UNKNOWN): {sum(1 for u in um if u['status']=='consistent' and I(u['n_resolved'])<I(u['n_contexts']))}")
    mis = [u for u in uu if u["status"] == "missing_in_some"]
    P(f"  missing_in_some uses {len(mis)}: kinds " + ", ".join(f"{k}={v}" for k, v in collections.Counter(u['kind'] for u in mis).items()))
    xs = [u for u in uu if I(u["xscene_unique"] or 0) > 0]
    P(f"  %Name resolved only through the outer scene's owner (instance-root fallback): {len(xs)} uses " + "; ".join(f"{u['repo']}/{u['script']}:{u['line']}" for u in xs[:4]))
    P(f"  absolute (/root) literal uses {sum(1 for u in uu if u['status']=='absolute')}; missing_all {sum(1 for u in uu if u['status']=='missing_all')}; no_context {sum(1 for u in uu if u['status']=='no_context')}; unknown (into imported model) {sum(1 for u in uu if u['status']=='unknown')}")
    P(f"  scripts calling add_child: {sum(1 for r in rr if I(r['add_child'])>0)}/{len(rr)}; inherited scenes {sum(x['inherited_scenes'] for x in rg)} (from imported models {sum(x['inherited_from_model'] for x in rg)}); editable-children entries {sum(x['editable_entries'] for x in rg)}; scenes adding nodes inside instances {sum(x['scenes_adding_into_instances'] for x in rg)}")
    if grp == "games":
        pc = collections.Counter()
        pl = collections.Counter()
        for r in multi:
            p = labels.get((r["repo"], r["script"]), "UNLABELLED")
            pc[p] += 1
            if I(r["lit_uses"]) > 0:
                pl[p] += 1
        P("  purposes (>=2-structure game scripts): " + ", ".join(f"{k}={v} (lit {pl[k]})" for k, v in pc.most_common()))

    # Exclusive attribution of each >=2-structure script to the first matching mechanism
    # (M2 counts only when the inherited copies differ, i.e. M2_inherited_diff).
    def attribution(r):
        m = r["mech"]
        m2, m5 = "M2_inherited_diff" in m, "M5_" in m
        if m2 or m5:
            return "inheritance:" + ("M2+M5" if m2 and m5 else "M2" if m2 else "M5")
        if "M1_text2+_diff" in m or "M1b" in m:
            return "multi-attach"
        if "M3" in m:
            return "instance-edit"
        return "other"

    ac, al = collections.Counter(), collections.Counter()
    for r in multi:
        a = attribution(r)
        ac[a] += 1
        al[a] += 1 if I(r["lit_uses"]) > 0 else 0
    P("  >=2-structure scripts by first matching mechanism (inheritance M2_diff/M5 > multi-attach M1/M1b > instance-edit M3): "
      + ", ".join(f"{k}={v} (lit {al[k]})" for k, v in sorted(ac.items())))
    m5 = {(r["repo"], r["script"]) for r in multi if "M5_" in r["mech"]}
    via_inh = {(c["repo"], c["script"]) for c in ctxs if (c["repo"], c["script"]) in m5 and c["via"] != "self"
               and c["how"] in ("text_override_inh", "inherited")}
    P(f"  M5 scripts {len(m5)}: subclass contexts include an inherited scene (subclass script on an inherited node) in {len(via_inh)}")
    mm = [u for u in um if u["status"] == "missing_in_some"]
    P(f"  missing_in_some uses in >=2-structure scripts {len(mm)}: kinds " + ", ".join(f"{k}={v}" for k, v in sorted(collections.Counter(u['kind'] for u in mm).items())))
    P(f"  distinct literal paths (script, path): {len({(u['repo'], u['script'], u['path']) for u in uu})} in {len({(u['repo'], u['script']) for u in uu})} scripts")
    P("")
open(os.path.join(OUT, "summary.txt"), "w", encoding="utf-8", newline="\n").write("\n".join(L) + "\n")
print("\n".join(L))
