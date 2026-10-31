"""How many first-party GDScript files of the corpus use a construct that the implementation does not support yet.

Usage: python coverage.py CORPUS_DIR EXTENSION_API_JSON [--samples N]
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py
    EXTENSION_API_JSON: thirdparty/godot-cpp/gdextension/extension_api-4-7.json, for the static methods of
        engine classes and built-in types
    --samples N: also print the first N locations of each construct, for reading

Constructs, found by token; the implementation at 812aaa2 reports each as not supported yet or as a syntax error:
  - `const` declarations; `enum` declarations; inner classes; `static var`;
  - `Type.m()` for a static method of an engine class or a built-in type, or of a class_name class of the same
    repository, and `Type.new()` of such a class;
  - lambdas (`func` after another token on its line, other than `static` and annotations);
  - the operators `in` and `not in` (the first `in` of a line starting with `for` is the loop's), bitwise
    operators (`& | ^ ~ << >>` and their assignments), `**`;
  - `assert(...)`;
  - `%Name` and `$%Name`, and `$".."` paths;
  - StringName and NodePath literals (`&"..."`, `^"..."`);
  - `preload()` of anything but a scene;
  - class annotations `@tool`, `@icon`, `@static_unload`, `@abstract`, and member annotations other than
    @export, @export_range, @export_enum, @export_multiline, @onready and the group annotations.
"""
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

SUPPORTED_AN = {"@export", "@export_range", "@export_enum", "@export_multiline", "@onready",
                "@export_group", "@export_subgroup", "@export_category", "@owner", "@parent"}
CLASS_AN = {"@tool", "@icon", "@static_unload", "@abstract"}
OPERAND = {"ID", "NUM", "STR"}

CONST, ENUM, INNER, STATIC_VAR = "const", "enum", "inner class", "static var"
ENGINE_STATIC, OWN_STATIC, OWN_NEW = "Type.m(): engine or built-in static method", "Type.m(): static method of the repository's class", "Type.new() of a script class"
LAMBDA, IN, BITWISE, POWER, ASSERT = "lambda", "in / not in", "bitwise operators", "**", "assert"
UNIQUE, LITERAL, PRELOAD = "%Name and $\"..\"", "StringName and NodePath literals", "preload() of a non-scene"
CLASS_ANS, MEMBER_ANS = "@tool, @icon, @static_unload, @abstract", "other member annotations"


def adjacent(a, b):
    return a.line == b.line and a.col + 1 == b.col


def construct(ts, i, static_of, script_classes, loop_lines):
    """The construct that the token at i begins, if any."""
    t = ts[i]
    prev = ts[i - 1] if i else None
    nxt = ts[i + 1] if i + 1 < len(ts) else None
    if t.k == "ID":
        if t.v == "const" and t.first:
            return CONST
        if t.v == "enum" and (t.first or (prev and prev.k == "AN")):
            return ENUM
        if t.v == "class" and t.first:
            return INNER
        if t.v == "static" and nxt and nxt.v == "var":
            return STATIC_VAR
        if t.v == "func" and prev and prev.line == t.line and prev.v != "static" and prev.k != "AN":
            return LAMBDA
        if t.v == "assert" and nxt and nxt.v == "(":
            return ASSERT
        if t.v == "in":
            j = i
            while j > 0 and ts[j - 1].line == t.line:
                j -= 1
            if ts[j].v == "for" and t.line not in loop_lines:
                loop_lines.add(t.line)
                return None
            return IN
        if t.v == "preload" and nxt and nxt.v == "(" and i + 2 < len(ts) and ts[i + 2].k == "STR":
            return None if re.search(r"\.t?scn$", ts[i + 2].v) else PRELOAD
        if (nxt and nxt.v == "." and i + 3 < len(ts) and ts[i + 2].k == "ID" and ts[i + 3].v == "("
                and not (prev and prev.v == ".")):
            m = ts[i + 2].v
            if m in static_of["engine"].get(t.v, ()):
                return ENGINE_STATIC
            if m in static_of["own"].get(t.v, ()):
                return OWN_STATIC
            if m == "new" and t.v in script_classes:
                return OWN_NEW
        return None
    if t.k == "STR":
        return LITERAL if t.pre in ("&", "^") else None
    if t.k == "AN":
        if t.v in CLASS_AN:
            return CLASS_ANS
        return None if t.v in SUPPORTED_AN else MEMBER_ANS
    if t.k != "OP":
        return None
    v = t.v
    if v == "$" and nxt and (nxt.v == "%" or (nxt.k == "STR" and nxt.v.startswith(".."))):
        return UNIQUE
    if v == "%" and nxt and nxt.k == "ID" and adjacent(t, nxt) and not (prev and (prev.k in OPERAND or prev.v in (")", "]", "$"))):
        return UNIQUE
    if v == "*" and nxt and nxt.v == "*" and adjacent(t, nxt):
        return POWER
    if v in "&|" and not (nxt and nxt.v == v and adjacent(t, nxt)) and not (prev and prev.v == v and adjacent(prev, t)):
        return BITWISE
    if v in "^~":
        return BITWISE
    if v in "<>" and nxt and nxt.v == v and adjacent(t, nxt) and not (prev and prev.v == v and adjacent(prev, t)):
        return BITWISE
    return None


def scan(ts, static_of, script_classes):
    """(construct, line) for each use in a file's tokens."""
    loop_lines = set()
    hits = []
    for i in range(len(ts)):
        c = construct(ts, i, static_of, script_classes, loop_lines)
        if c:
            hits.append((c, ts[i].line))
    return hits


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
    engine_static = collections.defaultdict(set)
    for group in ("classes", "builtin_classes"):
        for c in api[group]:
            for m in c.get("methods", []):
                if m.get("is_static"):
                    engine_static[c["name"]].add(m["name"])

    stats = {key: {"files": 0, "lines": 0, "clean": 0, "uses": collections.Counter(), "hit": collections.Counter()}
             for key in ("all", "games")}
    per_repo = []
    where = collections.defaultdict(list)
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
        static_of = {"engine": engine_static, "own": own_static}
        repo_files = repo_clean = 0
        for f, (src, ts) in sorted(scripts.items()):
            lines = sum(1 for x in src.splitlines() if x.strip() and not x.strip().startswith("#"))
            hits = scan(ts, static_of, script_classes)
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
        for c, n in s["hit"].most_common():
            print(f"   {c:50s} files {n:5d} ({100 * n / s['files']:4.1f}%)  uses {s['uses'][c]:5d} ({1000 * s['uses'][c] / s['lines']:4.1f} per 1,000 lines)")
    print("== Per repository: files, files using none of the constructs")
    for name, group, n, clean in per_repo:
        print(f"   {name:28s} {group:7s} {n:5d} {clean:5d} ({100 * clean / n if n else 0:4.1f}%)")
    if samples:
        print("== Samples")
        for c, locs in sorted(where.items()):
            print(f"   {c}")
            for loc in locs:
                print(f"      {loc}")


main()
