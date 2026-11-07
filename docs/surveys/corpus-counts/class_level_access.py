"""Class-level access from one first-party GDScript file to another script's class, which GDScript cannot make to a
class of this language (spec §11.3): static method calls, and enums, constants or inner classes in types or constant
expressions. Also the reads of constants and enum values that it can make, and calls through autoload instances.

Usage: python class_level_access.py CORPUS_DIR EXTENSION_API_JSON
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py (first-party rules and groups from there)
    EXTENSION_API_JSON: thirdparty/godot-cpp/gdextension/extension_api-4-7.json (engine class hierarchy)

Token-based (gdcorpus tokenizer). `X.m` counts when X, outside a member access, names another file: a class_name of
the same project, an autoload (project.godot) or a constant or variable initialized with preload or load in the same
file; and m is a top-level `static func`, `enum`, `const` or `class` of that file, by spelling. A type is a reference
after `:`, `->`, `as` or `is`, or inside `Array[...]` or `Dictionary[...]` (inner_classes.ref_context). A constant
expression is the initializer of a `const` on the same logical line. A class "would break" GDScript if migrated when
another file makes one of the accesses GDScript cannot make to it.
"""
import collections
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "cross-branch-duplication"))
sys.path.insert(0, HERE)
import gdcorpus as G  # noqa: E402
import inner_classes as IC  # noqa: E402

KINDS = ("static method", "enum, constant or inner class as a type", "in a constant expression")


def declarations(ts):
    """Top-level declarations of a file: name -> kind."""
    out = {}
    for i, t in enumerate(ts[:-1]):
        if t.k != "ID" or t.col != 0 and not (i and ts[i - 1].v == "static" and ts[i - 1].col == 0):
            continue
        if t.v == "func" and i and ts[i - 1].v == "static" and ts[i + 1].k == "ID":
            out[ts[i + 1].v] = "static func"
        elif t.v in ("enum", "const", "class") and ts[i + 1].k == "ID":
            out[ts[i + 1].v] = t.v
    return out


def in_const(ts, i):
    """Whether ts[i] is in the initializer of a `const` declared on its logical line."""
    k = i - 1
    while k >= 0 and ts[k].line == ts[i].line:
        if ts[k].v == "const":
            return True
        k -= 1
    return False


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    IC.load_api(sys.argv[2])
    c = collections.Counter()
    by_repo = collections.defaultdict(collections.Counter)
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        infos = G.script_infos(repo)
        tokens = {f: [t for t in G.tokenize(repo.read(f)) if t.k != "NL"] for f in infos}
        decls = {f: declarations(ts) for f, ts in tokens.items()}
        cname = collections.defaultdict(dict)
        for f, s in infos.items():
            if s["class_name"]:
                cname[repo.project_of(f).root][s["class_name"]] = f
        autoload = IC.autoloads(repo)
        breaking = collections.defaultdict(set)  # target file -> files making a forbidden access
        for f, s in infos.items():
            if not s["first_party"]:
                continue
            ts = tokens[f]
            root = repo.project_of(f).root
            local = {}
            for i, t in enumerate(ts[:-4]):
                if t.v in ("const", "var") and ts[i + 1].k == "ID" and ts[i + 2].v == "=" and ts[i + 3].v in ("preload", "load") \
                        and ts[i + 4].v == "(" and i + 5 < len(ts) and ts[i + 5].k == "STR":
                    target = repo.project_of(f).to_rel(ts[i + 5].v, f)
                    if target in infos:
                        local[ts[i + 1].v] = target
            for i, t in enumerate(ts[:-2]):
                if t.k != "ID" or (i and ts[i - 1].v == ".") or ts[i + 1].v != "." or ts[i + 2].k != "ID":
                    continue
                target, via = local.get(t.v), "preload"
                if target is None and t.v in cname[root]:
                    target, via = cname[root][t.v], "class_name"
                if target is None and (root, t.v) in autoload:
                    target, via = autoload[(root, t.v)], "autoload"
                if target is None or target == f:
                    continue
                kind = decls[target].get(ts[i + 2].v)
                if kind is None:
                    if via == "autoload":
                        c["autoload instance member"] += 1
                    continue
                end = i + 2  # The whole dotted name, as in `Constants.Match.Player`.
                while end + 2 < len(ts) and ts[end + 1].v == "." and ts[end + 2].k == "ID" and not (end + 3 < len(ts) and ts[end + 3].v == "("):
                    end += 2
                if kind == "static func":
                    use = "static method"
                elif IC.ref_context(ts, i, end) in ("type annotation", "is", "as"):
                    use = "enum, constant or inner class as a type"
                elif in_const(ts, i):
                    use = "in a constant expression"
                else:
                    c[f"allowed: {kind} read via {via}"] += 1
                    continue
                c[f"{use} via {via}"] += 1
                by_repo[name][use] += 1
                breaking[target].add(f)
        classes = [f for f, s in infos.items() if s["first_party"] and (s["class_name"] or f in autoload.values())]
        broken = sum(1 for f in classes if breaking.get(f))
        c[f"{group}: named classes and autoloads"] += len(classes)
        c[f"{group}: would break GDScript if migrated"] += broken
        by_repo[name]["classes"] += len(classes)
        by_repo[name]["breaking"] += broken
    for key in sorted(c):
        print(f"{key}: {c[key]}")
    for repo, n in sorted(by_repo.items(), key=lambda x: -x[1]["breaking"]):
        uses = ", ".join(f"{k} {n[k]}" for k in KINDS if n[k])
        print(f"{repo}: {n['breaking']} of {n['classes']} classes would break; {uses}")


if __name__ == "__main__":
    main()
