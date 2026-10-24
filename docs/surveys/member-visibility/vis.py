"""Where script members are used: only in their own file, also in subclasses, or in other scripts.

Usage: python vis.py CORPUS_DIR API_JSON
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py
    API_JSON:   written by ../cross-branch-duplication/build_api.py

Members are top-level `func`, `var`, `signal` and `const` declarations (static ones included) in first-party
scripts; overrides of engine virtual methods are skipped, and a redeclaration in a descendant script (an
override) is counted once, at the root declaration. Usage is found by name, without type resolution:

- a member is classified only when its name has exactly one root declaration among the repo's first-party
  scripts and is not an engine member name; other members are reported as "ambiguous";
- "external": `.name` appears in a first-party script that is neither the declaring script nor a descendant;
- "subclass_only": not external, and the name appears as an identifier in a descendant script;
- "internal": neither;
- "scene_conn": a function named as the method of a signal connection in a first-party .tscn (engine access).

For external vars, "written" means `.name` followed by an assignment operator in such a script.
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

CONN_RE = re.compile(r'^\[connection [^\]]*method="([^"]+)"', re.M)
ASSIGN_OPS = {"=", "+", "-", "*", "/", "%", "|", "&", "^"}
CATS = ["external", "subclass_only", "internal", "ambiguous", "scene_conn"]


def declarations(toks):
    """Top-level (column 0) declarations, after annotations and `static`: (kind, name, line)."""
    lines = collections.defaultdict(list)
    for t in toks:
        if t.k != "NL":
            lines[t.line].append(t)
    out = []
    for ln, ts in lines.items():
        if ts[0].col != 0:
            continue
        j = 0
        while j < len(ts) and ts[j].k == "AN":
            j += 1
            if j < len(ts) and ts[j].v == "(":
                depth = 0
                while j < len(ts):
                    depth += {"(": 1, ")": -1}.get(ts[j].v, 0)
                    j += 1
                    if depth == 0:
                        break
        if j < len(ts) and ts[j].v == "static":
            j += 1
        if j + 1 < len(ts) and ts[j].k == "ID" and ts[j].v in ("func", "var", "signal", "const") and ts[j + 1].k == "ID":
            out.append((ts[j].v, ts[j + 1].v, ln))
    return out


def survey_repo(R, engine_names):
    scripts = G.script_infos(R)
    fp = [f for f, s in scripts.items() if s["first_party"]]
    toks = {f: [t for t in G.tokenize(R.read(f)) if t.k != "NL"] for f in fp}
    loc = sum(1 for f in fp for ln in R.read(f).splitlines() if ln.strip() and not ln.strip().startswith("#"))

    conn_methods = set()
    for f in R.files:
        if f.endswith(".tscn") and R.first_party(f):
            try:
                conn_methods.update(CONN_RE.findall(R.read(f)))
            except OSError:
                pass

    children = collections.defaultdict(set)
    for f in fp:
        if scripts[f]["base_script"]:
            children[scripts[f]["base_script"]].add(f)

    def ancestors(f):
        out, cur = [], scripts[f]["base_script"]
        while cur and cur not in out:
            out.append(cur)
            cur = scripts[cur]["base_script"]
        return out

    def descendants(f):
        out, stack = set(), [f]
        while stack:
            for c in children.get(stack.pop(), ()):
                if c not in out:
                    out.add(c)
                    stack.append(c)
        return out

    decls = {f: declarations(toks[f]) for f in fp}
    declared = collections.defaultdict(set)
    for f, ds in decls.items():
        for _, n, _ in ds:
            declared[n].add(f)
    roots = {n: sum(1 for f in fs if not any(a in fs for a in ancestors(f))) for n, fs in declared.items()}
    overrides = {(f, n) for n, fs in declared.items() for f in fs if any(a in fs for a in ancestors(f))}

    idents = {f: collections.Counter(t.v for t in toks[f] if t.k == "ID") for f in fp}
    dotted, written = {}, {}
    for f in fp:
        ts = toks[f]
        d, w = collections.Counter(), collections.Counter()
        for i in range(1, len(ts)):
            if ts[i].k == "ID" and ts[i - 1].k == "OP" and ts[i - 1].v == ".":
                d[ts[i].v] += 1
                if i + 1 < len(ts) and ts[i + 1].k == "OP" and ts[i + 1].v in ASSIGN_OPS:
                    op, nx = ts[i + 1], ts[i + 2] if i + 2 < len(ts) else None
                    adjacent_eq = nx is not None and nx.v == "=" and nx.col == op.col + 1 and nx.line == op.line
                    if (op.v == "=") != adjacent_eq:  # `=` but not `==`, or a compound `op=`
                        w[ts[i].v] += 1
        dotted[f], written[f] = d, w

    c = collections.Counter(loc=loc)
    examples = []
    for f in fp:
        desc = descendants(f)
        for kind, n, ln in decls[f]:
            if (n.startswith("_") and n in engine_names) or (f, n) in overrides:
                continue
            unique = roots[n] == 1 and n not in engine_names
            others = [g for g in fp if g != f and g not in desc]
            if kind == "func" and n in conn_methods:
                cat = "scene_conn"
            elif not unique:
                cat = "ambiguous"
            elif any(dotted[g][n] for g in others):
                cat = "external"
            elif any(idents[d][n] for d in desc):
                cat = "subclass_only"
            else:
                cat = "internal"
            c[cat] += 1
            c[(kind, cat)] += 1
            if n.startswith("_"):
                c[("underscore", cat)] += 1
            if desc:
                c[("base", cat)] += 1
            if cat == "subclass_only":
                c["sub_overridden" if any((d, n) in overrides for d in desc) else "sub_used"] += 1
                examples.append(f"{R.name}/{f}:{ln} {kind} {n}")
            if kind == "var" and cat == "external":
                c["var_ext_written" if any(written[g][n] for g in others) else "var_ext_read_only"] += 1
    return c, examples


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    corpus, api_path = sys.argv[1], sys.argv[2]
    engine_names = set()
    for cls in G.load_api(api_path).values():
        for k in ("methods", "members", "signals", "constants"):
            engine_names.update(cls.get(k, []))

    total, games = collections.Counter(), collections.Counter()
    all_examples = []
    print("| repo | group | code lines | " + " | ".join(CATS) + " |")
    print("|---|---|---|" + "---|" * len(CATS))
    for name, _, _, group in G.REPOS:
        c, ex = survey_repo(G.Repo(corpus, name, group), engine_names)
        total.update(c)
        if group == "game":
            games.update(c)
        all_examples += ex
        print(f"| {name} | {group} | {c['loc']} | " + " | ".join(str(c[k]) for k in CATS) + " |")
    for label, c in (("all", total), ("games", games)):
        classified = sum(c[k] for k in ("external", "subclass_only", "internal"))
        print(f"\n{label}: {c['loc']} code lines; " + ", ".join(f"{k} {c[k]}" for k in CATS))
        print(f"  classified {classified}; external {c['external'] / classified:.1%} of classified, "
              f"{c['external'] * 1000 / c['loc']:.1f} per 1k lines; subclass_only {c['subclass_only']} = "
              f"{c['subclass_only'] / (c['external'] + c['subclass_only']):.1%} of members used outside their file "
              f"({c['sub_overridden']} overridden, {c['sub_used']} only called or read)")
    for kind in ("func", "var", "signal", "const"):
        print(f"{kind}: " + ", ".join(f"{k} {total[(kind, k)]}" for k in CATS))
    print("members of scripts that have subclasses: " + ", ".join(f"{k} {total[('base', k)]}" for k in CATS))
    print("names starting with '_': " + ", ".join(f"{k} {total[('underscore', k)]}" for k in CATS))
    print(f"external vars: written from other scripts {total['var_ext_written']}, only read {total['var_ext_read_only']}")
    print("\nsubclass_only members:")
    for e in all_examples:
        print("  " + e)


if __name__ == "__main__":
    main()
