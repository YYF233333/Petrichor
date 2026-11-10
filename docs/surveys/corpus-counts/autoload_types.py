"""Autoload names in types in first-party GDScript: the bare name (`-> Global`) or a name declared in the autoload's
script (`Global.LayerTypes`).

Usage: python autoload_types.py CORPUS_DIR
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py (first-party rules and groups from there)

Token-based (gdcorpus tokenizer). An autoload is an entry of `[autoload]` in the project.godot of the file's project,
with a res:// or uid:// path; a scene autoload stands for the script of its root node. A type is a name after
`var`/`const`/`for x:`, after `x:` in the parameters of a func, signal or lambda, after `->`, `as`, `is` or `extends`,
or an element of `Array[...]` or `Dictionary[..., ...]`. `Autoload.X` is an enum or an inner class when the autoload's
script declares X as one, by spelling. Other references to autoloads are counted per repository for comparison, and
autoload scripts with a class_name per repository.
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402


def resolve(repo, proj, path):
    return proj.to_rel(path) if path.startswith("res://") else repo.uid.get(path)


def autoloads(repo):
    """(project root, autoload name) -> repo-relative path of its script, or None."""
    out = {}
    for root, proj in repo.projects.items():
        pg = (root + "/" if root else "") + "project.godot"
        if pg not in repo.fileset:
            continue
        section = None
        for ln in repo.read(pg).splitlines():
            s = ln.strip()
            if s.startswith("["):
                section = s
                continue
            if section != "[autoload]" or "=" not in s:
                continue
            k, v = s.split("=", 1)
            rel = resolve(repo, proj, v.strip().strip('"').lstrip("*"))
            if rel and rel.endswith((".tscn", ".scn")) and rel in repo.fileset:
                sf = G.SceneFile(rel, repo.read(rel))
                m = G.EXT_REF_RE.search(sf.nodes[0].props.get("script") or "") if sf.nodes else None
                rel = resolve(repo, proj, sf.ext[m.group(1)][1]) if m else None
            out[(root, k.strip())] = rel if rel and rel.endswith(".gd") and rel in repo.fileset else None
    return out


def position(ts, i, stack):
    """The type position of the name at ts[i], or None."""
    p1, p2, p3 = (ts[i - k] if i >= k else None for k in (1, 2, 3))
    if p1 is None:
        return None
    if p1.v == ":" and p2 is not None and p2.k == "ID" and p3 is not None:
        if p3.v in ("var", "const", "for"):
            return "var/const/for annotation"
        if p3.v in ("(", ",") and stack and stack[-1][1]:
            return "parameter"
        return None
    if p1.v == ">" and p2 is not None and p2.v == "-":
        return "return type"
    if p1.v in ("as", "extends"):
        return p1.v
    if p1.v == "is" or (p1.v == "not" and p2 is not None and p2.v == "is"):
        return "is"
    if p1.v == "[" and p2 is not None and p2.v in ("Array", "Dictionary"):
        return "container element"
    if p1.v == "," and stack and stack[-1][0] == "[":
        k, depth = i - 2, 0
        while k >= 0:
            if ts[k].v in ")]}":
                depth += 1
            elif ts[k].v in "([{":
                if depth == 0:
                    break
                depth -= 1
            k -= 1
        if k >= 1 and ts[k - 1].v == "Dictionary":
            return "container element"
    return None


def shape(repo, script, chain):
    if len(chain) == 1:
        return "bare autoload name"
    kind = "Autoload.X"
    if script:
        src = repo.read(script)
        if re.search(r"^\s*enum\s+" + re.escape(chain[1]) + r"\b", src, re.M):
            kind = "Autoload.Enum"
        elif re.search(r"^\s*class\s+" + re.escape(chain[1]) + r"\b", src, re.M):
            kind = "Autoload.InnerClass"
    return kind + (" (deeper)" if len(chain) > 2 else "")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    uses = collections.Counter()
    shapes = collections.Counter()
    per_repo = collections.Counter()
    other_refs = collections.Counter()
    listed = collections.defaultdict(list)
    print("== autoloads per repository (with class_name)")
    for name, _, _, group in G.REPOS:
        repo = G.Repo(sys.argv[1], name, group)
        al = autoloads(repo)
        if not al:
            continue
        named = sum(1 for s in al.values() if s and G.parse_header(repo.read(s))["class_name"])
        print(f"  {name} [{group}]: {len(al)} autoloads, {named} with class_name")
        for f in repo.files:
            if not f.endswith(".gd") or not repo.first_party(f):
                continue
            root = repo.project_of(f).root
            names = {n for (r, n) in al if r == root}
            if not names:
                continue
            ts = [t for t in G.tokenize(repo.read(f)) if t.k != "NL"]
            stack = []  # (bracket, opens the parameters of a func, signal or lambda)
            for i, t in enumerate(ts):
                if t.k == "OP" and t.v in "([{":
                    sig = t.v == "(" and i >= 1 and (ts[i - 1].v == "func" or (
                        i >= 2 and ts[i - 2].v in ("func", "signal") and ts[i - 1].k == "ID"))
                    stack.append((t.v, sig))
                    continue
                if t.k == "OP" and t.v in ")]}":
                    if stack:
                        stack.pop()
                    continue
                if t.k != "ID" or t.v not in names or (i and ts[i - 1].v == "."):
                    continue
                pos = position(ts, i, stack)
                if pos is None:
                    other_refs[(name, group)] += 1
                    continue
                chain, j = [t.v], i
                while j + 2 < len(ts) and ts[j + 1].v == "." and ts[j + 2].k == "ID":
                    chain.append(ts[j + 2].v)
                    j += 2
                s = shape(repo, al[(root, t.v)], chain)
                uses[(pos, s)] += 1
                shapes[s] += 1
                per_repo[(name, group)] += 1
                listed[s].append(f"{name}/{f}:{t.line} {pos}: {'.'.join(chain)}")
    print("== type uses by position and shape")
    for (pos, s), n in uses.most_common():
        print(f"  {n:5}  {pos:28} {s}")
    print("== by shape")
    for s, n in shapes.most_common():
        print(f"  {n:5}  {s}")
    print("== type uses per repository (other autoload references in parentheses)")
    for k in sorted(set(per_repo) | set(other_refs)):
        print(f"  {k[0]} [{k[1]}]: {per_repo[k]} ({other_refs[k]})")
    print("== all type uses")
    for s, rows in listed.items():
        print(f"-- {s}")
        for row in rows:
            print("   " + row)


if __name__ == "__main__":
    main()
