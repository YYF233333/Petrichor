"""Uses of the engine APIs through which objects pass, in first-party GDScript, by name (spec §10.2).

Usage: python engine_objects.py CORPUS_DIR EXTENSION_API_JSON
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py (first-party rules and groups from there)
    EXTENSION_API_JSON: thirdparty/godot-cpp/gdextension/extension_api-4-7.json

Token-based (gdcorpus tokenizer), with no type resolution, so each name counts for every engine class declaring it.
- calls: `name(` of a method returning an object, after `.` or bare, not after `func`;
- reads: `.name` of a property holding an object, not followed by `(` or an assignment;
- connections: `.name.connect(`, `connect("name"` and scene `[connection signal="name"` of a signal with an object
  parameter, unless an engine signal of that name has none;
- overrides: `func name(` of a virtual method with an object parameter.
A script's own member of the same name is counted too, so names such as `get_item` over-count. Calls by name
(`call("name")`), method values and bare property reads are missed.
"""
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

ASSIGNMENTS = ("=", "+=", "-=", "*=", "/=")
SCENE_CONNECTION = re.compile(r'\[connection signal="([^"]+)"')
SHOWN = 80


def engine_apis(path):
    """Name -> sorted "Class:ReturnClass" for each kind."""
    with open(path, encoding="utf-8") as fh:
        classes = json.load(fh)["classes"]
    objects = {c["name"] for c in classes}
    calls, reads, signals, virtuals = (collections.defaultdict(set) for _ in range(4))
    for c in classes:
        for m in c.get("methods", []):
            returned = m.get("return_value", {}).get("type")
            params = [a["type"] for a in m.get("arguments", []) if a["type"] in objects]
            if m.get("is_virtual") and params:
                virtuals[m["name"]].add(f"{c['name']}({', '.join(params)})")
            elif not m.get("is_virtual") and returned in objects:
                calls[m["name"]].add(f"{c['name']}:{returned}")
        for p in c.get("properties", []):
            if p["type"] in objects:
                reads[p["name"]].add(f"{c['name']}:{p['type']}")
        for s in c.get("signals", []):
            params = [a["type"] for a in s.get("arguments", []) if a["type"] in objects]
            signals[s["name"]].add(f"{c['name']}({', '.join(params)})" if params else "")
    # A signal some class declares without objects, as BaseButton's `pressed`, is left out.
    signals = {name: classes for name, classes in signals.items() if "" not in classes}
    return calls, reads, signals, virtuals


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    calls, reads, signals, virtuals = engine_apis(sys.argv[2])
    counts = {kind: collections.Counter() for kind in ("calls", "reads", "connections", "overrides")}
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        for f, info in G.script_infos(repo).items():
            if not info["first_party"]:
                continue
            ts = [t for t in G.tokenize(repo.read(f)) if t.k != "NL"]
            for i, t in enumerate(ts):
                before = ts[i - 1].v if i > 0 else None
                after = ts[i + 1].v if i + 1 < len(ts) else None
                if t.k == "STR" and before == "(" and i > 1 and ts[i - 2].v == "connect" and t.v in signals:
                    counts["connections"][t.v] += 1
                if t.k != "ID":
                    continue
                if before == "func":
                    if t.v in virtuals:
                        counts["overrides"][t.v] += 1
                elif after == "(" and t.v in calls:
                    counts["calls"][t.v] += 1
                elif before == "." and t.v in signals and after == "." and i + 2 < len(ts) and ts[i + 2].v == "connect":
                    counts["connections"][t.v] += 1
                elif before == "." and t.v in reads and after != "(" and not (after in ASSIGNMENTS and (i + 2 >= len(ts) or ts[i + 2].v != "=")):
                    counts["reads"][t.v] += 1
        for scene in repo.files:
            if scene.endswith(".tscn") and repo.first_party(scene):
                for signal in SCENE_CONNECTION.findall(repo.read(scene)):
                    if signal in signals:
                        counts["connections"][signal] += 1
    for kind, apis in (("calls", calls), ("reads", reads), ("connections", signals), ("overrides", virtuals)):
        print(f"{kind}: {sum(counts[kind].values())}")
        for api, count in counts[kind].most_common(SHOWN):
            print(f"  {count:5} {api}: {' '.join(sorted(apis[api]))}")


if __name__ == "__main__":
    main()
