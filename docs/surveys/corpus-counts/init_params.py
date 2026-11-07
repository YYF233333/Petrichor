"""`_init` with parameters in first-party GDScript, by native ancestor, and how those classes are constructed.

Usage: python init_params.py CORPUS_DIR EXTENSION_API_JSON
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py (first-party rules and groups from there)
    EXTENSION_API_JSON: thirdparty/godot-cpp/gdextension/extension_api-4-7.json (engine class hierarchy)

Token-based (gdcorpus tokenizer). Only a file's own `func _init` (at column 0) counts; inner classes do not. The native
ancestor follows `extends` through the repository's class_name and path scripts (gdcorpus.script_infos). A parameter
has a default when `=` or `:=` follows its name. Constructions are `Name.new(` with Name a class_name whose `_init`
has parameters; constructions through preloaded constants are not counted.
"""
import collections
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402


def defaults(ts, i):
    """For the parameters of the func at ts[i], whether each has a default; None without a parameter list."""
    j = i + 2
    if j >= len(ts) or ts[j].v != "(":
        return None
    depth, out, has_default, named = 0, [], False, False
    while j < len(ts):
        v = ts[j].v
        if v in ("(", "[", "{"):
            depth += 1
        elif v in (")", "]", "}"):
            depth -= 1
            if depth == 0:
                break
        elif depth == 1 and v == ",":
            if named:
                out.append(has_default)
            has_default, named = False, False
        elif depth == 1 and ts[j].k == "ID" and not named:
            named = True
        elif depth == 1 and (v in ("=", ":=") or (v == ":" and j + 1 < len(ts) and ts[j + 1].v == "=")):
            has_default = True
        j += 1
    if named:
        out.append(has_default)
    return out


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[2], encoding="utf-8") as fh:
        G.NATIVE_PARENT.update({c["name"]: c.get("inherits") for c in json.load(fh)["classes"]})
    c = collections.Counter()
    places = collections.defaultdict(list)
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        infos = {f: s for f, s in G.script_infos(repo).items() if s["first_party"]}
        tokens = {f: [t for t in G.tokenize(repo.read(f)) if t.k != "NL"] for f in infos}
        with_params = {}
        for f, s in infos.items():
            native = s["native"] or "?"
            kind = "Node" if G.native_is(native, "Node") else "Resource" if G.native_is(native, "Resource") else "other"
            ts = tokens[f]
            for i, t in enumerate(ts):
                if t.k == "ID" and t.v == "func" and t.col == 0 and i + 1 < len(ts) and ts[i + 1].v == "_init":
                    found = defaults(ts, i)
                    if not found:
                        continue
                    key = f"{kind}, {'some required' if not all(found) else 'all with defaults'}"
                    c[key] += 1
                    if kind != "other":
                        places[key].append(f"{name}/{f}:{t.line} ({group})")
                    if s["class_name"]:
                        with_params[s["class_name"]] = kind
        for f, ts in tokens.items():
            for i in range(len(ts) - 4):
                if ts[i].k == "ID" and ts[i].v in with_params and ts[i + 1].v == "." and ts[i + 2].v == "new" and ts[i + 3].v == "(":
                    c[f"{with_params[ts[i].v]}, Name.new({'' if ts[i + 4].v == ')' else '...'})"] += 1
    for key in sorted(c):
        print(f"{key}: {c[key]}")
    for key in sorted(places):
        print(f"{key} at: {', '.join(places[key])}")


if __name__ == "__main__":
    main()
