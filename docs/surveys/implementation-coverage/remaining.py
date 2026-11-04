"""The rerun at a203ae1: how many first-party GDScript files of the corpus use a construct of the first version
that the implementation still does not support, once every construct of coverage.py is.

Usage: python remaining.py CORPUS_DIR EXTENSION_API_JSON [--samples N]
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py
    EXTENSION_API_JSON: thirdparty/godot-cpp/gdextension/extension_api-4-7.json, for the engine's classes, their
        constants and the methods that return Variant
    --samples N: also print the first N locations of each construct, for reading

Constructs, found by token; the implementation at a203ae1 reports each as not supported yet, except literal
property names, which it compiles without checking or exposing the member (spec §10.4, §10.5):
  - Variant as a type: `Array`, `Dictionary` without `[` and `Variant` after `:` or `->`;
  - a call of an engine method that returns Variant, by a name that every engine method of that name returns
    Variant from and no built-in type's method has, such as `get_meta(` or `get_value(`, other than `new`,
    unless the file declares a function of that name;
  - array and dictionary patterns: a branch of a match that starts with `[` or `{`;
  - `func _init(` with parameters;
  - a parameter's default value that is not constant: one with `self`, a method call (`.m(`), a call of a
    lowercase name other than a math utility function and preload, or a lowercase name of its own;
  - a lambda (`func` after another token on its line, other than `static` and annotations) whose body awaits;
  - `preload(` inside a `[` or `{` of a `const` line;
  - a member of the file named by a string literal on self: `tween_property(self, "p"`, and `set_deferred`,
    `set`, `get`, `set_indexed`, `get_indexed` with a literal first argument, bare or after `self.`, where p
    is declared by a `var` that starts a line;
  - a constant or enum value of an engine class (not a global one) by its bare name, such as
    `NOTIFICATION_PREDELETE`, unless the file declares the name;
  - a static function of a class_name class of the repository as a value: `Class.f` not followed by `(`;
  - `==` or `!=` with `self`, or a name the file annotates with an engine class or a class_name of the
    repository, as an operand on either side, and no `null` on the other;
  - `for` over a string literal, or over a name the file annotates as String.
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

VARIANT_TYPE = "Variant as a type (untyped Array, Dictionary)"
VARIANT_API = "engine method that returns Variant"
MATCH = "array or dictionary pattern"
INIT = "_init() with parameters"
DEFAULT = "non-constant default value"
ASYNC_LAMBDA = "lambda that awaits"
CONST_PRELOAD = "preload() in a container of a constant"
PROPERTY = "property name as a literal, on self"
BARE_CONSTANT = "engine class constant by its bare name"
STATIC_VALUE = "static function as a value"
OBJECT_EQ = "== or != between objects"
STRING_FOR = "for over a String"
ORDER = [VARIANT_API, OBJECT_EQ, VARIANT_TYPE, PROPERTY, DEFAULT, INIT, BARE_CONSTANT, MATCH, ASYNC_LAMBDA,
         STRING_FOR, CONST_PRELOAD, STATIC_VALUE]

LITERALS = {"true", "false", "null", "self", "PI", "TAU", "INF", "NAN"}
PROPERTY_CALLS = {"set_deferred", "set", "get", "set_indexed", "get_indexed"}
OPEN, CLOSE = "([{", ")]}"


def func_body(ts, i):
    """Token range [start, end) of the body of the `func` at token i (as corpus-counts/counts.py finds it)."""
    n, depth, j = len(ts), 0, i + 1
    while j < n and not (ts[j].v == ":" and depth == 0):
        depth += (ts[j].v in OPEN) - (ts[j].v in CLOSE)
        j += 1
    if j + 1 >= n:
        return n, n
    if ts[j + 1].line == ts[j].line:
        k, depth = j + 1, 0
        while k < n and ts[k].line == ts[j].line and not (depth == 0 and (ts[k].v in CLOSE or ts[k].v == ",")):
            depth += (ts[k].v in OPEN) - (ts[k].v in CLOSE)
            k += 1
        return j + 1, k
    h = i
    while h > 0 and ts[h - 1].line == ts[i].line:
        h -= 1
    k = j + 1
    while k < n and not (ts[k].first and ts[k].col <= ts[h].col):
        k += 1
    return j + 1, k


def adjacent(a, b):
    return a.line == b.line and a.col + 1 == b.col


def params(ts, i):
    """The parameters of the signature whose `(` is at i: (name token index, default token range or None)."""
    out, depth, j = [], 0, i
    while j < len(ts):
        if ts[j].k == "OP" and ts[j].v in OPEN:
            depth += 1
        elif ts[j].k == "OP" and ts[j].v in CLOSE:
            depth -= 1
            if depth == 0:
                break
        j += 1
    end = j
    k = i + 1
    while k < end:
        e, depth = k, 0
        while e < end and not (depth == 0 and ts[e].v == "," and ts[e].k == "OP"):
            if ts[e].k == "OP" and ts[e].v in OPEN:
                depth += 1
            elif ts[e].k == "OP" and ts[e].v in CLOSE:
                depth -= 1
            e += 1
        eq = next((x for x in range(k, e) if ts[x].v == "=" and ts[x].k == "OP" and not (x > k and ts[x - 1].v in ":!<>=")), None)
        name = k if ts[k].k == "ID" else None
        if name is not None:
            out.append((name, (eq + 1, e) if eq is not None else None))
        k = e + 1
    return out, end


def non_constant(ts, a, b, math):
    for x in range(a, b):
        t = ts[x]
        if t.k != "ID":
            continue
        prev = ts[x - 1] if x > a else None
        nxt = ts[x + 1] if x + 1 < b else None
        after_dot = prev is not None and prev.v == "." and prev.k == "OP"
        if t.v == "self":
            return True
        if nxt is not None and nxt.v == "(":
            if after_dot or (t.v[:1].islower() and t.v not in math and t.v != "preload"):
                return True
            continue
        if not after_dot and t.v[:1].islower() and t.v not in LITERALS:
            return True
    return False


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
    engine_classes = {c["name"] for c in api["classes"]}
    builtin_methods = {m["name"] for c in api["builtin_classes"] for m in c.get("methods", [])}
    returns = collections.defaultdict(set)
    for c in api["classes"]:
        for m in c.get("methods", []):
            returns[m["name"]].add((m.get("return_value") or {}).get("type"))
    variant_methods = {m for m, types in returns.items() if types == {"Variant"}} - builtin_methods - {"new"}
    global_names = {e["name"] for e in api.get("global_constants", [])}
    for e in api.get("global_enums", []):
        global_names |= {v["name"] for v in e.get("values", [])}
    class_constants = set()
    for c in api["classes"]:
        class_constants |= {k["name"] for k in c.get("constants", []) or []}
        for e in c.get("enums", []) or []:
            class_constants |= {v["name"] for v in e.get("values", [])}
    class_constants -= global_names
    math = {u["name"] for u in api["utility_functions"] if u.get("category") == "math"}

    stats = {key: {"files": 0, "lines": 0, "clean": 0, "uses": collections.Counter(), "hit": collections.Counter()}
             for key in ("all", "games")}
    per_repo = []
    where = collections.defaultdict(list)
    method_names = collections.Counter()
    for name, _, _, group in G.REPOS:
        repo = G.Repo(corpus, name, group)
        scripts = {}
        own_static = collections.defaultdict(set)
        script_classes = set()
        for f in repo.files:
            if f.endswith(".gd") and repo.first_party(f):
                src = repo.read(f)
                ts = [t for t in G.tokenize(src) if t.k != "NL"]
                scripts[f] = (src, ts)
                cname = next((ts[i + 1].v for i, t in enumerate(ts[:-1]) if t.v == "class_name"), None)
                if cname:
                    script_classes.add(cname)
                    for i, t in enumerate(ts[:-2]):
                        if t.v == "static" and ts[i + 1].v == "func":
                            own_static[cname].add(ts[i + 2].v)
        object_types = engine_classes | script_classes
        repo_files = repo_clean = 0
        for f, (src, ts) in sorted(scripts.items()):
            lines = sum(1 for x in src.splitlines() if x.strip() and not x.strip().startswith("#"))
            hits = []
            n = len(ts)
            declared = {ts[i + 1].v for i in range(n - 1) if ts[i].k == "ID" and ts[i].v in ("var", "const", "func", "signal", "enum", "class") and ts[i + 1].k == "ID"}
            functions = {ts[i + 1].v for i in range(n - 1) if ts[i].v == "func" and ts[i + 1].k == "ID"}
            members = {ts[i + 1].v for i in range(n - 1) if ts[i].v == "var" and ts[i + 1].k == "ID" and ts[i].first}
            typed = {}
            for i in range(1, n - 2):
                if ts[i].k == "ID" and ts[i + 1].v == ":" and ts[i + 2].k == "ID" and ts[i - 1].v in ("var", "const", "for", "(", ","):
                    typed[ts[i].v] = ts[i + 2].v
            starts = IC.logical_starts(ts)
            for i, t in enumerate(ts):
                prev = ts[i - 1] if i else None
                nxt = ts[i + 1] if i + 1 < n else None
                # Variant as a type.
                if t.k == "ID" and t.v in ("Array", "Dictionary", "Variant") and prev is not None and (prev.v == ":" or (prev.v == ">" and i > 1 and ts[i - 2].v == "-" and adjacent(ts[i - 2], prev))) and not (nxt and nxt.v == "["):
                    hits.append((VARIANT_TYPE, t.line))
                if t.k == "ID" and t.v in variant_methods and nxt is not None and nxt.v == "(" and t.v not in functions and not (prev is not None and prev.v == "func"):
                    hits.append((VARIANT_API, t.line))
                    method_names[t.v] += 1
                if t.k == "ID" and t.v == "func" and nxt is not None and nxt.v == "_init" and i + 3 < n and ts[i + 2].v == "(" and ts[i + 3].k == "ID":
                    hits.append((INIT, t.line))
                if t.k == "ID" and t.v == "func" and (prev is None or prev.line != t.line or prev.v == "static" or prev.k == "AN"):
                    j = i + 1
                    while j < n and ts[j].v != "(":
                        j += 1
                    if j < n:
                        for _, default in params(ts, j)[0]:
                            if default and non_constant(ts, default[0], default[1], math):
                                hits.append((DEFAULT, t.line))
                elif t.k == "ID" and t.v == "func" and prev is not None and prev.line == t.line:
                    s, e = func_body(ts, i)
                    if any(ts[x].v == "await" for x in range(s, e)):
                        hits.append((ASYNC_LAMBDA, t.line))
                if t.k == "ID" and t.v == "const" and t.first:
                    depth, j = 0, i + 1
                    while j < n and not (ts[j].first and depth == 0):
                        if ts[j].k == "OP" and ts[j].v in OPEN:
                            depth += 1
                        elif ts[j].k == "OP" and ts[j].v in CLOSE:
                            depth -= 1
                        elif ts[j].v == "preload" and depth > 0 and any(ts[x].v in "[{" for x in range(i, j) if ts[x].k == "OP"):
                            hits.append((CONST_PRELOAD, t.line))
                            break
                        j += 1
                if t.k == "ID" and nxt is not None and nxt.v == "(" and i + 2 < n:
                    on_self = prev is None or prev.v != "." or (i > 1 and ts[i - 2].v == "self")
                    if t.v == "tween_property" and i + 4 < n and ts[i + 2].v == "self" and ts[i + 3].v == "," and ts[i + 4].k == "STR" and ts[i + 4].v in members:
                        hits.append((PROPERTY, t.line))
                    elif t.v in PROPERTY_CALLS and on_self and ts[i + 2].k == "STR" and ts[i + 2].v in members and t.v not in functions:
                        hits.append((PROPERTY, t.line))
                if t.k == "ID" and t.v in class_constants and t.v not in declared and not (prev is not None and prev.v == "."):
                    hits.append((BARE_CONSTANT, t.line))
                if t.k == "ID" and t.v in own_static and nxt is not None and nxt.v == "." and i + 2 < n and ts[i + 2].v in own_static[t.v] and not (i + 3 < n and ts[i + 3].v == "(") and not (prev is not None and prev.v == "."):
                    hits.append((STATIC_VALUE, t.line))
                if t.k == "OP" and t.v == "=" and prev is not None and prev.k == "OP" and prev.v in "=!" and adjacent(prev, t) and nxt is not None and not (i > 1 and ts[i - 2].v == "=" and adjacent(ts[i - 2], prev)):
                    left, right = ts[i - 2] if i > 1 else None, nxt
                    # A whole operand: a name not after `.`, or not followed by `.`, `(` or `[`.
                    left_whole = left is not None and left.k == "ID" and not (i > 2 and ts[i - 3].v == ".")
                    right_whole = right.k == "ID" and not (i + 2 < n and ts[i + 2].v in (".", "(", "["))
                    objects = [tok for tok, whole in ((left, left_whole), (right, right_whole))
                               if whole and (tok.v == "self" or typed.get(tok.v) in object_types)]
                    if objects and "null" not in (left.v if left else None, right.v):
                        hits.append((OBJECT_EQ, t.line))
                if t.k == "ID" and t.v == "for" and t.first:
                    j = i + 1
                    while j < n and ts[j].line == t.line and ts[j].v != "in":
                        j += 1
                    if j + 2 < n and ts[j].v == "in" and ts[j + 2].v == ":" and (ts[j + 1].k == "STR" or typed.get(ts[j + 1].v) == "String"):
                        hits.append((STRING_FOR, t.line))
            # Branches of match statements.
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
    print("== Engine methods that return Variant, most called")
    print("   " + ", ".join(f"{m} {c}" for m, c in method_names.most_common(15)))
    if samples:
        print("== Samples")
        for c in ORDER:
            print(f"   {c}")
            for loc in where[c]:
                print(f"      {loc}")


if __name__ == "__main__":
    main()
