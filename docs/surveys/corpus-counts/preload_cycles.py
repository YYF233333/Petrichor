"""Cycles of loads among the first-party files of the corpus, as GDScript loads them: a scene or a resource loads its
ext_resources, and a script, when it compiles, its preloads and its base script.

Usage: python preload_cycles.py CORPUS_DIR
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py

Edges start at first-party .tscn, .tres and .gd files: each ext_resource of a scene or a resource (a script, a
subscene or an exported resource), each preload() of a script with a literal path, and a script's base script (extends
by path or by class_name). Paths resolve as res:// in the file's project, relative to the file, or by uid. A cycle is a
strongly connected component of more than one file, or a file that loads itself, reported with whether a scene or a
resource is in it. A script that preloads a scene which has it as an ext_resource is also counted on its own.
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

UID_RE = re.compile(r'^\[gd_(?:scene|resource)\b[^\]]*\buid="(uid://[^"]+)"')


def components(edges):
    """The strongly connected components of the graph (Tarjan, iterative)."""
    index, low, on_stack, stack, found = {}, {}, set(), [], []
    counter = 0
    for root in edges:
        if root in index:
            continue
        work = [(root, iter(edges.get(root, ())))]
        index[root] = low[root] = counter
        counter += 1
        stack.append(root)
        on_stack.add(root)
        while work:
            node, it = work[-1]
            advanced = False
            for nxt in it:
                if nxt not in index:
                    index[nxt] = low[nxt] = counter
                    counter += 1
                    stack.append(nxt)
                    on_stack.add(nxt)
                    work.append((nxt, iter(edges.get(nxt, ()))))
                    advanced = True
                    break
                if nxt in on_stack:
                    low[node] = min(low[node], index[nxt])
            if advanced:
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[node])
            if low[node] == index[node]:
                component = []
                while True:
                    top = stack.pop()
                    on_stack.discard(top)
                    component.append(top)
                    if top == node:
                        break
                found.append(sorted(component))
    return found


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    totals = collections.Counter()
    listed = []
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        uids = dict(repo.uid)
        texts = {}
        for f in repo.files:
            if f.endswith((".tscn", ".tres", ".gd")) and repo.first_party(f):
                try:
                    texts[f] = repo.read(f)
                except OSError:
                    continue
                m = UID_RE.match(texts[f])
                if m:
                    uids[m.group(1)] = f

        def resolve(ref, origin):
            if ref.startswith("uid://"):
                return uids.get(ref)
            return repo.project_of(origin).to_rel(ref, origin)

        infos = G.script_infos(repo)
        edges = collections.defaultdict(set)
        preloads = collections.defaultdict(set)
        for f, text in texts.items():
            if f.endswith(".gd"):
                ts = [t for t in G.tokenize(text) if t.k != "NL"]
                for i in range(len(ts) - 3):
                    if ts[i].k == "ID" and ts[i].v == "preload" and ts[i + 1].v == "(" and ts[i + 2].k == "STR" and ts[i + 3].v == ")":
                        target = resolve(ts[i + 2].v, f)
                        if target in texts:
                            totals["preloads of a scene, a resource or a script"] += 1
                        elif ts[i + 2].v.startswith("uid://") or ts[i + 2].v.endswith((".tscn", ".tres", ".gd")):
                            totals["preloads of one not found"] += 1
                        if target:
                            edges[f].add(target)
                            preloads[f].add(target)
                base = infos.get(f, {}).get("base_script")
                if base:
                    edges[f].add(base)
            else:
                for _, path, uid in G.SceneFile(f, text).ext.values():
                    target = resolve(path, f) if path else (resolve(uid, f) if uid else None)
                    if target:
                        edges[f].add(target)
        totals["first-party scripts"] += sum(1 for f in texts if f.endswith(".gd"))
        totals["first-party scenes and resources"] += sum(1 for f in texts if not f.endswith(".gd"))
        totals["scripts with a preload"] += len(preloads)
        for script, targets in preloads.items():
            for scene in targets:
                if scene.endswith(".tscn") and script in edges.get(scene, ()):
                    totals["a script preloads a scene that has it"] += 1
                    listed.append(f"self: {name}/{script} -> {scene} ({group})")
        for component in components(edges):
            if len(component) < 2 and component[0] not in edges.get(component[0], ()):
                continue
            with_resource = any(not f.endswith(".gd") for f in component)
            key = "cycles through a scene or a resource" if with_resource else "cycles of scripts only"
            totals[key] += 1
            totals[key + ", files"] += len(component)
            listed.append(f"{'scene' if with_resource else 'script'} cycle: {name} ({group}): {', '.join(component)}")
    for key in sorted(totals):
        print(f"{key}: {totals[key]}")
    for line in listed:
        print(line)


if __name__ == "__main__":
    main()
