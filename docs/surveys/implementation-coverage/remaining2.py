"""The rerun at 602ee60: how many first-party GDScript files of the corpus use a construct of the first version
that the implementation still does not support.

Usage: python remaining2.py CORPUS_DIR EXTENSION_API_JSON [--samples N]
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py
    EXTENSION_API_JSON: thirdparty/godot-cpp/gdextension/extension_api-4-7.json, for the math utility functions
    --samples N: also print the first N locations of each construct, for reading

Constructs, found by token as remaining.py finds them; the implementation at 602ee60 reports each as not
supported yet, except literal property names, which it compiles without checking or exposing the member
(spec §10.4, §10.5):
  - from remaining.py, those still missing: array and dictionary patterns, non-constant default values, lambdas
    that await, preload() in a container of a constant, property names as literals on self, static functions of
    the repository's classes as values, `for` over a String;
  - a rest parameter: `...` before a parameter's name;
  - the `breakpoint` statement;
  - a class that defines `_iter_init`, for `for` over its objects;
  - a member of another instance of the file's own class: `x.m` where the file annotates x with its class_name
    and declares m by `var` or `func`. In this language, such a member is not pub.
Names are typed only where the file writes `name: Type` after `var`, `const`, `for`, `(` or `,`.
"""
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cross-branch-duplication"))
sys.path.insert(0, os.path.join(HERE, "..", "corpus-counts"))
import gdcorpus as G  # noqa: E402
import inner_classes as IC  # noqa: E402
import remaining as R  # noqa: E402

MATCH = "array or dictionary pattern"
DEFAULT = "non-constant default value"
ASYNC_LAMBDA = "lambda that awaits"
CONST_PRELOAD = "preload() in a container of a constant"
PROPERTY = "property name as a literal, on self"
STATIC_VALUE = "static function as a value"
STRING_FOR = "for over a String"
REST = "rest parameter"
BREAKPOINT = "breakpoint"
ITERATOR = "class with _iter_init"
OTHER_INSTANCE = "member of another instance of the class"
ORDER = [OTHER_INSTANCE, PROPERTY, DEFAULT, MATCH, ASYNC_LAMBDA, ITERATOR, REST, BREAKPOINT, STRING_FOR,
         CONST_PRELOAD, STATIC_VALUE]


def main():
    args = sys.argv[1:]
    samples = 0
    if "--samples" in args:
        k = args.index("--samples")
        samples = int(args[k + 1])
        del args[k:k + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    corpus, api_path = args
    with open(api_path, encoding="utf-8") as fh:
        api = json.load(fh)
    math = {u["name"] for u in api["utility_functions"] if u.get("category") == "math"}

    stats = {key: {"files": 0, "lines": 0, "clean": 0, "uses": collections.Counter(), "hit": collections.Counter()}
             for key in ("all", "games")}
    per_repo = []
    where = collections.defaultdict(list)
    for name, _, _, group in G.REPOS:
        repo = G.Repo(corpus, name, group)
        scripts = {}
        own_static = collections.defaultdict(set)
        for f in repo.files:
            if f.endswith(".gd") and repo.first_party(f):
                src = repo.read(f)
                ts = [t for t in G.tokenize(src) if t.k != "NL"]
                scripts[f] = (src, ts)
                cname = next((ts[i + 1].v for i, t in enumerate(ts[:-1]) if t.v == "class_name"), None)
                if cname:
                    for i, t in enumerate(ts[:-2]):
                        if t.v == "static" and ts[i + 1].v == "func":
                            own_static[cname].add(ts[i + 2].v)
        repo_files = repo_clean = 0
        for f, (src, ts) in sorted(scripts.items()):
            lines = sum(1 for x in src.splitlines() if x.strip() and not x.strip().startswith("#"))
            hits = []
            n = len(ts)
            functions = {ts[i + 1].v for i in range(n - 1) if ts[i].v == "func" and ts[i + 1].k == "ID"}
            members = {ts[i + 1].v for i in range(n - 1) if ts[i].v == "var" and ts[i + 1].k == "ID" and ts[i].first}
            cname = next((ts[i + 1].v for i, t in enumerate(ts[:-1]) if t.v == "class_name"), None)
            typed = {}
            for i in range(1, n - 2):
                if ts[i].k == "ID" and ts[i + 1].v == ":" and ts[i + 2].k == "ID" and ts[i - 1].v in ("var", "const", "for", "(", ","):
                    typed[ts[i].v] = ts[i + 2].v
            starts = IC.logical_starts(ts)
            for i, t in enumerate(ts):
                prev = ts[i - 1] if i else None
                nxt = ts[i + 1] if i + 1 < n else None
                if t.k == "ID" and t.v == "func" and (prev is None or prev.line != t.line or prev.v == "static" or prev.k == "AN"):
                    j = i + 1
                    while j < n and ts[j].v != "(":
                        j += 1
                    if j < n:
                        for _, default in R.params(ts, j)[0]:
                            if default and R.non_constant(ts, default[0], default[1], math):
                                hits.append((DEFAULT, t.line))
                    if nxt is not None and nxt.v == "_iter_init":
                        hits.append((ITERATOR, t.line))
                elif t.k == "ID" and t.v == "func" and prev is not None and prev.line == t.line:
                    s, e = R.func_body(ts, i)
                    if any(ts[x].v == "await" for x in range(s, e)):
                        hits.append((ASYNC_LAMBDA, t.line))
                if t.v == "." and prev is not None and prev.v in ("(", ",") and i + 3 < n and ts[i + 1].v == "." and ts[i + 2].v == "." and ts[i + 3].k == "ID":
                    hits.append((REST, t.line))
                if t.k == "ID" and t.v == "breakpoint" and t.first:
                    hits.append((BREAKPOINT, t.line))
                if t.k == "ID" and t.v == "const" and t.first:
                    depth, j = 0, i + 1
                    while j < n and not (ts[j].first and depth == 0):
                        if ts[j].k == "OP" and ts[j].v in R.OPEN:
                            depth += 1
                        elif ts[j].k == "OP" and ts[j].v in R.CLOSE:
                            depth -= 1
                        elif ts[j].v == "preload" and depth > 0 and any(ts[x].v in "[{" for x in range(i, j) if ts[x].k == "OP"):
                            hits.append((CONST_PRELOAD, t.line))
                            break
                        j += 1
                if t.k == "ID" and nxt is not None and nxt.v == "(" and i + 2 < n:
                    on_self = prev is None or prev.v != "." or (i > 1 and ts[i - 2].v == "self")
                    if t.v == "tween_property" and i + 4 < n and ts[i + 2].v == "self" and ts[i + 3].v == "," and ts[i + 4].k == "STR" and ts[i + 4].v in members:
                        hits.append((PROPERTY, t.line))
                    elif t.v in R.PROPERTY_CALLS and on_self and ts[i + 2].k == "STR" and ts[i + 2].v in members and t.v not in functions:
                        hits.append((PROPERTY, t.line))
                if t.k == "ID" and t.v in own_static and nxt is not None and nxt.v == "." and i + 2 < n and ts[i + 2].v in own_static[t.v] and not (i + 3 < n and ts[i + 3].v == "(") and not (prev is not None and prev.v == "."):
                    hits.append((STATIC_VALUE, t.line))
                if cname and t.k == "ID" and typed.get(t.v) == cname and nxt is not None and nxt.v == "." and i + 2 < n and (ts[i + 2].v in members or ts[i + 2].v in functions) and not (prev is not None and prev.v in (".", "var", "const", "for", "(", ",")):
                    hits.append((OTHER_INSTANCE, t.line))
                if t.k == "ID" and t.v == "for" and t.first:
                    j = i + 1
                    while j < n and ts[j].line == t.line and ts[j].v != "in":
                        j += 1
                    if j + 2 < n and ts[j].v == "in" and ts[j + 2].v == ":" and (ts[j + 1].k == "STR" or typed.get(ts[j + 1].v) == "String"):
                        hits.append((STRING_FOR, t.line))
            for k, a in enumerate(starts):
                if ts[a].v != "match":
                    continue
                col = ts[a].col
                branch_col = None
                for b in starts[k + 1:]:
                    if ts[b].col <= col:
                        break
                    if branch_col is None:
                        branch_col = ts[b].col
                    if ts[b].col == branch_col and ts[b].v in ("[", "{"):
                        hits.append((MATCH, ts[b].line))
            used = collections.Counter(h for h, _ in hits)
            for h, line in hits:
                if len(where[h]) < samples:
                    where[h].append(f"{name}/{f}:{line}")
            repo_files += 1
            repo_clean += not used
            for key in ("all", "games") if group == "game" else ("all",):
                s = stats[key]
                s["files"] += 1
                s["lines"] += lines
                s["clean"] += not used
                s["uses"].update(used)
                s["hit"].update(used.keys())
        per_repo.append((name, group, repo_files, repo_clean))

    for key, title in (("all", "All repositories"), ("games", "Game repositories")):
        s = stats[key]
        print(f"== {title}: {s['files']} files, {s['lines']} code lines")
        print(f"   files using none of the constructs: {s['clean']} ({100 * s['clean'] / s['files']:.1f}%)")
        for c in ORDER:
            n = s["hit"][c]
            print(f"   {c:46s} files {n:5d} ({100 * n / s['files']:4.1f}%)  uses {s['uses'][c]:5d} ({1000 * s['uses'][c] / s['lines']:4.1f} per 1,000 lines)")
    print("== Per repository: files, files using none of the constructs")
    for name, group, n, clean in per_repo:
        print(f"   {name:28s} {group:7s} {n:5d} {clean:5d} ({100 * clean / n if n else 0:4.1f}%)")
    if samples:
        print("== Samples")
        for c in ORDER:
            print(f"   {c}")
            for loc in where[c]:
                print(f"      {loc}")


if __name__ == "__main__":
    main()
