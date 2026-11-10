"""The rerun at 3f61379: how many first-party GDScript files of the corpus use a construct that GDScript allows
and the implementation still refuses. What the spec makes a compile error, such as assigning to a character of a
String, is not counted. Each construct was found among the compiler's diagnostics or by trying, and its refusal
confirmed by compiling a snippet of a few lines with the headless build (--petrichor-build).

Usage: python remaining3.py CORPUS_DIR EXTENSION_API_JSON [--samples N]
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py
    EXTENSION_API_JSON: thirdparty/godot-cpp/gdextension/extension_api-4-7.json, for the engine's classes
    --samples N: also print the first N locations of each construct, for reading

Constructs, found by token; the implementation at 3f61379 refuses each (the message it gives in brackets):
  - a class_name of the repository used as a value ("Identifier is not declared");
  - an engine class used as a value ("is a type, not a value"); engine singletons are objects and not counted;
  - an untyped lambda parameter in a lambda passed directly to an Array method that takes a Callable
    ("needs a type: nothing the lambda goes to gives one");
  - instance_from_id(), and get_object() of a Callable or Signal ("returns Object?, which is not supported yet");
  - `$` with an absolute path that does not start with an autoload ("Only paths from this node ... yet");
  - % formatting in the initializer of a constant ("Formatting with % is not supported in constant expressions");
  - the constructor of a typed container, as `Array[String](a)` (a syntax error).
A name is a value where it follows =, (, `,`, [, return or `in`, or : inside { }, and comes before ), `,`, ], },
or the end of the line. Type positions are left out: an element of Array[...] or Dictionary[..., ...], and the
arguments of is_instance_of(), which takes a written class. A file declaring a variable, function or inner class
of the same name is left out for that name.
"""
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cross-branch-duplication"))
sys.path.insert(0, os.path.join(HERE, "..", "corpus-counts"))
import gdcorpus as G  # noqa: E402
from autoload_types import autoloads  # noqa: E402

CLASS_NAME = "class_name as a value"
ENGINE_CLASS = "engine class as a value"
LAMBDA = "untyped lambda parameter, Array method"
OBJECT_RESULT = "instance_from_id(), get_object()"
ROOT_PATH = "$ with an absolute path"
CONST_FORMAT = "% formatting in a constant"
CONTAINER_CTOR = "Array[T](a), Dictionary[K, V](d)"
ORDER = [CLASS_NAME, ENGINE_CLASS, LAMBDA, OBJECT_RESULT, ROOT_PATH, CONST_FORMAT, CONTAINER_CTOR]

ARRAY_CALLABLE_METHODS = {"all", "any", "filter", "map", "reduce", "sort_custom", "bsearch_custom", "find_custom",
                          "rfind_custom"}
VALUE_BEFORE = {"=", "(", ",", "[", "return", "in"}
VALUE_AFTER = {")", ",", "]", "}"}


def enclosing_open(ts, i):
    depth = 0
    for k in range(i - 1, -1, -1):
        t = ts[k]
        if t.k == "OP" and t.v in ")]}":
            depth += 1
        elif t.k == "OP" and t.v in "([{":
            if depth == 0:
                return k
            depth -= 1
    return None


def value_position(ts, i):
    """Whether the name at ts[i] stands where a value goes."""
    prev = ts[i - 1] if i else None
    nxt = ts[i + 1] if i + 1 < len(ts) else None
    if prev is None or (prev.v == "." and prev.k == "OP"):
        return False
    if not (nxt is None or nxt.first or (nxt.k == "OP" and nxt.v in VALUE_AFTER)):
        return False
    o = enclosing_open(ts, i)
    if prev.v == ":" and prev.k == "OP":
        return o is not None and ts[o].v == "{"
    if prev.v not in VALUE_BEFORE:
        return False
    if o is not None and ts[o].v == "[" and o > 0 and ts[o - 1].v in ("Array", "Dictionary"):
        return False
    if o is not None and ts[o].v == "(" and o > 0 and ts[o - 1].v == "is_instance_of":
        return False
    return True


def untyped_lambda_args(ts, j):
    """Whether the call whose `(` is ts[j] passes a lambda with an untyped parameter as an argument."""
    depth, n = 0, len(ts)
    for k in range(j, n):
        t = ts[k]
        if t.k == "OP" and t.v in "([{":
            depth += 1
        elif t.k == "OP" and t.v in ")]}":
            depth -= 1
            if depth == 0:
                return False
        elif depth == 1 and t.v == "func" and ts[k - 1].v in ("(", ","):
            p = k + 2 if k + 1 < n and ts[k + 1].k == "ID" else k + 1
            d, param = 0, []
            while p < n:
                v = ts[p].v
                if ts[p].k == "OP" and v in "([{":
                    d += 1
                elif ts[p].k == "OP" and v in ")]}":
                    d -= 1
                if d == 0 or (d == 1 and v == ","):
                    if param and ":" not in param:
                        return True
                    if d == 0:
                        break
                    param = []
                elif d >= 1 and not (d == 1 and v == "("):
                    param.append(v)
                p += 1
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
    singletons = {s["name"] for s in api["singletons"]}
    engine_classes = {c["name"] for c in api["classes"]} - singletons

    stats = {key: {"files": 0, "lines": 0, "clean": 0, "uses": collections.Counter(), "hit": collections.Counter()}
             for key in ("all", "games")}
    per_repo = []
    repo_hits = collections.defaultdict(collections.Counter)
    where = collections.defaultdict(list)
    for name, _, _, group in G.REPOS:
        repo = G.Repo(corpus, name, group)
        loads = autoloads(repo)
        scripts = {}
        class_names = set()
        for f in repo.files:
            if f.endswith(".gd"):
                src = repo.read(f)
                ts = [t for t in G.tokenize(src) if t.k != "NL"]
                cname = next((ts[i + 1].v for i, t in enumerate(ts[:-1]) if t.v == "class_name" and ts[i + 1].k == "ID"), None)
                if cname:
                    class_names.add(cname)
                if repo.first_party(f):
                    scripts[f] = (src, ts)
        repo_files = repo_clean = 0
        for f, (src, ts) in sorted(scripts.items()):
            lines = sum(1 for x in src.splitlines() if x.strip() and not x.strip().startswith("#"))
            root = repo.project_of(f).root
            hits = []
            n = len(ts)
            declared = {ts[i + 1].v for i in range(n - 1) if ts[i].v in ("var", "const", "func", "class", "for") and ts[i + 1].k == "ID"}
            declared |= {ts[i].v for i in range(1, n - 1) if ts[i].k == "ID" and ts[i - 1].v in ("(", ",") and ts[i + 1].v == ":"}
            for i, t in enumerate(ts):
                prev = ts[i - 1] if i else None
                nxt = ts[i + 1] if i + 1 < n else None
                after_dot = prev is not None and prev.v == "." and prev.k == "OP"
                if t.k == "ID" and t.v not in declared and value_position(ts, i):
                    if t.v in class_names:
                        hits.append((CLASS_NAME, t.line))
                    elif t.v in engine_classes:
                        hits.append((ENGINE_CLASS, t.line))
                if t.k == "ID" and after_dot and t.v in ARRAY_CALLABLE_METHODS and nxt is not None and nxt.v == "(" and untyped_lambda_args(ts, i + 1):
                    hits.append((LAMBDA, t.line))
                if t.k == "ID" and nxt is not None and nxt.v == "(" and t.v not in declared:
                    if (t.v == "instance_from_id" and not after_dot) or (t.v == "get_object" and after_dot and i + 2 < n and ts[i + 2].v == ")"):
                        hits.append((OBJECT_RESULT, t.line))
                if t.k == "OP" and t.v == "$" and nxt is not None:
                    path = None
                    if nxt.k == "STR" and nxt.v.startswith("/"):
                        path = nxt.v
                    elif nxt.k == "OP" and nxt.v == "/":
                        path, k = "", i + 1
                        while k < n and ts[k].line == t.line and (ts[k].k == "ID" or ts[k].v == "/"):
                            path += ts[k].v
                            k += 1
                    if path is not None:
                        parts = path.split("/")
                        autoload = len(parts) > 2 and parts[1] == "root" and (root, parts[2]) in loads
                        if not autoload:
                            hits.append((ROOT_PATH, t.line))
                if t.k == "ID" and t.v == "const" and t.first:
                    k = i + 1
                    while k < n - 1 and not ts[k].first:
                        if ts[k].k == "STR" and ts[k + 1].k == "OP" and ts[k + 1].v == "%":
                            hits.append((CONST_FORMAT, t.line))
                            break
                        k += 1
                if t.k == "ID" and t.v in ("Array", "Dictionary") and nxt is not None and nxt.v == "[" and not after_dot:
                    k, d = i + 1, 0
                    while k < n:
                        if ts[k].k == "OP" and ts[k].v in "([{":
                            d += 1
                        elif ts[k].k == "OP" and ts[k].v in ")]}":
                            d -= 1
                            if d == 0:
                                break
                        k += 1
                    if k + 1 < n and ts[k + 1].v == "(" and ts[k + 1].line == ts[k].line:
                        hits.append((CONTAINER_CTOR, t.line))
            used = collections.Counter(h for h, _ in hits)
            for h, line in hits:
                if len(where[h]) < samples:
                    where[h].append(f"{name}/{f}:{line}")
            for h in used:
                repo_hits[h][name] += 1
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
            k = s["hit"][c]
            print(f"   {c:46s} files {k:5d} ({100 * k / s['files']:4.1f}%)  uses {s['uses'][c]:5d} ({1000 * s['uses'][c] / s['lines']:4.1f} per 1,000 lines)")
    print("== Repositories using each construct: files")
    for c in ORDER:
        print(f"   {c:46s} " + ", ".join(f"{r} {k}" for r, k in repo_hits[c].most_common()))
    print("== Per repository: files, files using none of the constructs")
    for name, group, k, clean in per_repo:
        print(f"   {name:28s} {group:7s} {k:5d} {clean:5d} ({100 * clean / k if k else 0:4.1f}%)")
    if samples:
        print("== Samples")
        for c in ORDER:
            print(f"   {c}")
            for loc in where[c]:
                print(f"      {loc}")


if __name__ == "__main__":
    main()
