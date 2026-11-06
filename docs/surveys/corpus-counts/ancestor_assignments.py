"""Assignments from a node's script to script members of the nodes below it, by scene ownership.

Usage: python ancestor_assignments.py CORPUS_DIR
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py

Evidence for members that must be assigned, which an ancestor may assign in its `_ready()`. The question is
how often the assigned node lies inside an instanced sub-scene rather than in the ancestor's own scene.

In the top-level functions and property accessors of first-party .gd files, it finds assignments (`=` and
compound) written `<target>.<member> = ...`, where <target> is one of:
  - a node path: `$Path`, `$"Path"`, `%Name`, `get_node("Path")` or `get_node_or_null("Path")`, the last two
    optionally after `self.`;
  - a member initialized with such a path (`@onready var x = $Path`, `as Type` allowed), or a member without
    an initializer that the script's functions assign one such path only;
  - an exported member, untyped or of an object type, whose value a scene file stores as a NodePath;
  - a local of the same function declared with such a path.
For each node of a composed text scene the script is attached to, it resolves the path as
Node.get_node_or_null does, `%Name` included (scene/main/node.cpp:1942-1946 at ed1daf0). Scenes are composed
with instances, inheritance and overrides. The root R of the assigning node N's scene is N itself when N is
the root of the composed scene, or of an instance whose own scene file attaches the script. Otherwise R is N's
owner. The target T is then:
  - in the same scene: T's owner is R, so T is declared in that scene or is an instance root placed in it;
  - in a sub-scene: T's owner is an instance root below R, so the path crosses an instance boundary;
  - in an outer scene: T's owner is above R (a node an outer scene added to an editable instance).
Only members the target's script declares with `var`, itself or through its `extends` chain, are classified.
These members are split into those with an initializer and those without. The latter are split by declared
type: a class or other object type, a built-in or enum type (which has a default value), or untyped.
Exported members are counted too. Assignments are split into those in `_ready()` outside lambdas and the
others. An assignment counts once per distinct classification over its scene contexts. One classified
differently in different scenes is also counted as ambiguous.
Not classified: absolute paths, paths with `..`, non-literal paths, paths found in no composed scene (nodes
added at run time), paths through imported models or placeholders, and exported members that no scene sets
to a NodePath. Neither are scripts attached to no text scene, nor targets whose assigned property no script
of theirs declares (engine properties).
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

OPEN, CLOSE = "([{", ")]}"
GET_NODE = {"get_node", "get_node_or_null"}
COMPOUND = set("+-*/%&|^")
EXPORT_GROUPS = {"@export_category", "@export_group", "@export_subgroup"}
BUILTIN_TYPES = {"bool", "int", "float", "String", "StringName", "NodePath", "Vector2", "Vector2i", "Rect2", "Rect2i",
                 "Vector3", "Vector3i", "Transform2D", "Vector4", "Vector4i", "Plane", "Quaternion", "AABB", "Basis",
                 "Transform3D", "Projection", "Color", "RID", "Callable", "Signal", "Dictionary", "Array", "Variant",
                 "PackedByteArray", "PackedInt32Array", "PackedInt64Array", "PackedFloat32Array", "PackedFloat64Array",
                 "PackedStringArray", "PackedVector2Array", "PackedVector3Array", "PackedColorArray", "PackedVector4Array"}
CLASSES = ("same", "sub", "outer")
KINDS = ("init", "object", "default", "untyped")
FAILS = ("engine", "nonliteral", "notfound", "unknown", "upward", "absolute", "self", "exported", "nocontext")


def adjacent(a, b):
    return a.line == b.line and b.col == a.col + len(a.v)


def close_of(ts, i):
    """Index of the bracket closing the one at token i."""
    depth = 0
    for k in range(i, len(ts)):
        depth += (ts[k].v in OPEN) - (ts[k].v in CLOSE)
        if depth == 0:
            return k
    return len(ts) - 1


def assign_op(ts, a):
    """The assignment operator beginning at token a, or None."""
    if a >= len(ts) or ts[a].k != "OP":
        return None
    t, nx = ts[a], ts[a + 1] if a + 1 < len(ts) else None
    if t.v == "=":
        return None if nx is not None and nx.v == "=" and adjacent(t, nx) else "="
    if nx is None or not adjacent(t, nx):
        return None
    if t.v in COMPOUND and nx.v == "=":
        return t.v + "="
    if t.v + nx.v in ("**", "<<", ">>") and a + 2 < len(ts) and ts[a + 2].v == "=" and adjacent(nx, ts[a + 2]):
        return t.v + nx.v + "="
    return None


def path_expr(ts, i):
    """The node path expression at token i as (path, index after it); path is None when not a literal."""
    n = len(ts)
    t = ts[i]
    if t.k == "OP" and t.v in "$%":
        j, path = i + 1, t.v.replace("$", "")
        if t.v == "$" and j < n and ts[j].v == "/":
            path, j = "/", j + 1
        while j < n:
            if ts[j].k == "OP" and ts[j].v == "%":
                path, j = path + "%", j + 1
            if j >= n or ts[j].k not in ("ID", "STR"):
                return None
            path, j = path + ts[j].v, j + 1
            if j + 1 < n and ts[j].v == "/" and (ts[j + 1].k in ("ID", "STR") or ts[j + 1].v == "%"):
                path, j = path + "/", j + 1
                continue
            return path, j
        return None
    if t.k == "ID" and t.v == "self" and i + 2 < n and ts[i + 1].v == "." and ts[i + 2].v in GET_NODE:
        i += 2
    if ts[i].k == "ID" and ts[i].v in GET_NODE and i + 1 < n and ts[i + 1].v == "(":
        c = close_of(ts, i + 1)
        arg = [x.v if x.k != "STR" else ("STR", x.v) for x in ts[i + 2:c]]
        path = None
        if len(arg) == 1 and isinstance(arg[0], tuple):
            path = arg[0][1]
        elif len(arg) == 4 and arg[0] == "NodePath" and arg[1] == "(" and isinstance(arg[2], tuple) and arg[3] == ")":
            path = arg[2][1]
        return path, c + 1
    return None


def binding_at(ts, i, end):
    """The binding of an initializer or assigned value at token i: ("lit", path), ("nonlit", None) or None."""
    r = path_expr(ts, i) if i < end else None
    if r is None:
        return None
    path, j = r
    if j < end and ts[j].v == "as" and j + 1 < end:
        j += 2
        while j + 1 < end and ts[j].v == "." and ts[j + 1].k == "ID":
            j += 2
    if j < end and not ts[j].first and ts[j].v not in (";", ":"):
        return None
    return ("lit", path) if path is not None else ("nonlit", None)


def parse_var(ts, j, end):
    """The `var` at token j: (name, type or None, has initializer, binding)."""
    name, k, typ, init = ts[j + 1].v, j + 2, None, None
    if k < end and ts[k].v == ":" and k + 1 < end and ts[k + 1].v == "=":
        typ, init = ":=", k + 2
    elif k < end and ts[k].v == ":" and k + 1 < end and ts[k + 1].k == "ID" and not ts[k + 1].first:
        typ, k = ts[k + 1].v, k + 2
        while k + 1 < end and ts[k].v == "." and ts[k + 1].k == "ID":
            typ, k = typ + "." + ts[k + 1].v, k + 2
        if k < end and ts[k].v == "[":
            k = close_of(ts, k) + 1
        if k < end and ts[k].v == "=" and not ts[k].first:
            init = k + 1
    elif k < end and ts[k].v == "=":
        init = k + 1
    return name, typ, init is not None, binding_at(ts, init, end) if init is not None else None


def lambda_range(ts, i, end):
    """Token range of the lambda whose `func` is token i."""
    j, depth = i + 1, 0
    while j < end and not (ts[j].v == ":" and depth == 0):
        depth += (ts[j].v in OPEN) - (ts[j].v in CLOSE)
        j += 1
    if j + 1 >= end:
        return i, end
    if ts[j + 1].line == ts[j].line:
        k, depth = j + 1, 0
        while k < end and ts[k].line == ts[j].line and not (depth == 0 and (ts[k].v in CLOSE or ts[k].v == ",")):
            depth += (ts[k].v in OPEN) - (ts[k].v in CLOSE)
            k += 1
        return i, k
    h = i
    while h > 0 and ts[h - 1].line == ts[i].line:
        h -= 1
    k = j + 1
    while k < end and not (ts[k].first and ts[k].col <= ts[h].col):
        k += 1
    return i, k


class Script:
    """Top-level members and the target assignments of one .gd file."""

    def __init__(self, src, enums):
        ts = [t for t in G.tokenize(src) if t.k != "NL"]
        self.lines = src.split("\n")
        self.members = {}  # name -> (kind, exported, line, type)
        bind = {}
        funcs = []
        starts = [i for i, t in enumerate(ts) if t.first and t.col == 0] + [len(ts)]
        pending = []
        for s, e in zip(starts, starts[1:]):
            j, anns = s, list(pending)
            while j < e and ts[j].k == "AN":
                anns.append(ts[j].v)
                j += 1
                if j < e and ts[j].v == "(" and adjacent(ts[j - 1], ts[j]):
                    j = close_of(ts, j) + 1
            if j >= e:
                pending = anns
                continue
            pending = []
            static = ts[j].v == "static"
            j += static
            if j + 1 >= e or ts[j].k != "ID":
                continue
            if ts[j].v == "var" and not static:
                name, typ, has_init, b = parse_var(ts, j, e)
                if has_init:
                    kind = "init"
                elif typ is None:
                    kind = "untyped"
                elif typ in BUILTIN_TYPES or typ.split(".")[-1] in enums:
                    kind = "default"
                else:
                    kind = "object"
                exported = any(a.startswith("@export") and a not in EXPORT_GROUPS for a in anns)
                self.members[name] = (kind, exported, ts[j].line, typ)
                if b is not None:
                    bind[name] = b
                elif exported and kind in ("object", "untyped"):
                    bind[name] = ("export", name)
                funcs.append(("set " + name, j + 2, e))
            elif ts[j].v == "func" and not static:
                funcs.append((ts[j + 1].v, j + 2, e))
        # Members without an initializer that the functions assign one path only.
        assigned = collections.defaultdict(set)
        for _, s, e in funcs:
            for i in range(s, e):
                t = ts[i]
                if t.k == "ID" and t.v in self.members and t.v not in bind and t.first and i + 1 < e and assign_op(ts, i + 1) == "=":
                    assigned[t.v].add(binding_at(ts, i + 2, e))
        for name, bs in assigned.items():
            if len(bs) == 1 and None not in bs and self.members[name][0] != "init":
                bind[name] = next(iter(bs))
        self.sites = []
        for fname, s, e in funcs:
            lambdas = [lambda_range(ts, i, e) for i in range(s, e) if ts[i].k == "ID" and ts[i].v == "func"]
            local = {}
            for i in range(s, e):
                t = ts[i]
                if t.k == "ID" and t.v == "var" and i + 1 < e:
                    name, _, _, b = parse_var(ts, i, e)
                    local[name] = b
                    continue
                if not (t.first or ts[i - 1].v in (":", ";")):
                    continue
                r = path_expr(ts, i)
                if r is not None:
                    b, k = (("lit", r[0]) if r[0] is not None else ("nonlit", None)), r[1]
                elif t.k == "ID" and t.v == "self" and i + 2 < e and ts[i + 1].v == "." and ts[i + 2].v in bind:
                    b, k = bind[ts[i + 2].v], i + 3
                elif t.k == "ID" and t.v not in G.KEYWORDS:
                    b, k = local[t.v] if t.v in local else bind.get(t.v), i + 1
                else:
                    continue
                if b is None or k + 2 >= e or ts[k].v != "." or ts[k + 1].k != "ID" or assign_op(ts, k + 2) is None:
                    continue
                ready = fname == "_ready" and not any(a <= i < z for a, z in lambdas)
                self.sites.append({"line": t.line, "ready": ready, "bind": b, "prop": ts[k + 1].v})


def node_path(n):
    parts = []
    while n.parent is not None:
        parts.append(n.name)
        n = n.parent
    return "/".join(reversed(parts)) or "."


def resolve(n, path, uniq):
    """Node.get_node_or_null on the composed tree: (status, target)."""
    if path.startswith("/"):
        return "absolute", None
    segs = [s for s in path.split("/") if s not in ("", ".")]
    if ".." in segs:
        return "upward", None
    if not segs:
        return "self", None
    cur = n
    for s in segs:
        if cur.unknown:
            return "unknown", None
        if s.startswith("%"):
            nx = uniq.get(id(cur), {}).get(s[1:])
            if nx is None and cur.owner is not None:
                nx = uniq.get(id(cur.owner), {}).get(s[1:])
        else:
            nx = cur.children.get(s)
        if nx is None:
            return "notfound", None
        cur = nx
    if cur.unknown and not cur.script:
        return "unknown", None
    return ("self", None) if cur is n else ("found", cur)


def classify(n, t):
    root = n.owner is None or (n.inst and n.assign_kind in ("node", "override_inh"))
    r = n if root else n.owner
    x = t.owner
    if x is r:
        return "same"
    while x is not None:
        if x is r:
            return "sub"
        x = x.parent
    return "outer"


def survey(R):
    scripts = G.script_infos(R)
    enums = set()
    srcs = {}
    for rel in scripts:
        srcs[rel] = R.read(rel)
        ts = G.tokenize(srcs[rel])
        enums.update(ts[k + 1].v for k in range(len(ts) - 1) if ts[k].k == "ID" and ts[k].v == "enum" and ts[k + 1].k == "ID")
    parsed = {rel: Script(src, enums) for rel, src in srcs.items()}
    contexts = G.scene_contexts(R)
    uniq_cache = {}

    def uniq_of(n):
        root = n
        while root.parent is not None:
            root = root.parent
        if id(root) not in uniq_cache:
            idx = collections.defaultdict(dict)
            for _, x in G.walk(root):
                if x.unique and x.owner is not None:
                    idx[id(x.owner)][x.name] = x
            uniq_cache[id(root)] = (root, idx)
        return uniq_cache[id(root)][1]

    def member(rel, prop):
        seen = set()
        while rel and rel not in seen and rel in parsed:
            seen.add(rel)
            if prop in parsed[rel].members:
                return parsed[rel].members[prop], rel
            rel = scripts[rel]["base_script"]
        return None

    props = {}

    def stored_path(ctx, name):
        """The NodePath that the context's scene file stores in member `name` of the context node."""
        if ctx["scene"] not in props:
            props[ctx["scene"]] = {}
            for s in G.parse_sections(R.read(ctx["scene"])):
                if s.tag == "node":
                    par, nm = G.unq(s.attrs.get("parent", "")), G.unq(s.attrs.get("name", ""))
                    props[ctx["scene"]]["." if "parent" not in s.attrs else nm if par == "." else par + "/" + nm] = s.props
        m = re.fullmatch(r'NodePath\("((?:[^"\\]|\\.)*)"\)', props[ctx["scene"]].get(ctx["path"], {}).get(name, ""))
        return m.group(1) if m else None

    C = collections.Counter()
    found = []  # (site key, class, member, member script, scene, target)
    for rel in sorted(parsed):
        if not R.first_party(rel):
            continue
        sc = parsed[rel]
        for site in sc.sites:
            where = "ready" if site["ready"] else "other"
            kind, path = site["bind"]
            C["sites"] += 1
            if kind == "nonlit":
                C["fail", "nonliteral"] += 1
                continue
            if not contexts.get(rel):
                C["fail", "nocontext"] += 1
                continue
            classes, keys, fails = set(), set(), collections.Counter()
            for ctx in contexts[rel]:
                n = ctx["node"]
                p = stored_path(ctx, path) if kind == "export" else path
                st, t = resolve(n, p, uniq_of(n)) if p is not None else ("exported", None)
                if st != "found":
                    fails[st] += 1
                    continue
                cls = classify(n, t)
                classes.add(cls)
                m = member(t.script, site["prop"]) if t.script and t.script.endswith(".gd") else None
                if m is None:
                    fails["engine"] += 1
                    if cls != "same":
                        found.append(((rel, site["line"]), where, cls, None, False, None, None, ctx["scene"], t))
                    continue
                (mkind, exported, mline, mtype), mrel = m
                keys.add((cls, mkind, exported, bool(t.inst)))
                found.append(((rel, site["line"]), where, cls, mkind, exported, f"{mrel}:{mline}", mtype, ctx["scene"], t))
            for cls in classes:
                C["any", cls, where] += 1
            for cls, mkind, exported, inst in keys:
                C["member", cls, mkind, where] += 1
                C["member", cls, "via_export", where] += kind == "export"
                C["member", cls, "inst_" + mkind, where] += inst
                if exported and mkind != "init":
                    C["member", cls, "exported", where] += 1
            if len({k[0] for k in keys}) > 1:
                C["ambiguous"] += 1
            if not keys:
                C["fail", next(f for f in FAILS if fails[f])] += 1
    return C, found, parsed


def row(C, cls, kinds):
    return [sum(C["member", cls, k, w] for k in kinds) for w in ("ready", "other")]


def main():
    out = []
    totals = collections.Counter()
    per_repo = []
    examples = []
    for name, _url, _sha, group in G.REPOS:
        R = G.Repo(sys.argv[1], name, group)
        C, found, parsed = survey(R)
        totals.update(C)
        per_repo.append((name, C))
        seen = set()
        for key, where, cls, mkind, exported, mdecl, mtype, scene, t in found:
            if (key, cls, mkind) in seen:
                continue
            seen.add((key, cls, mkind))
            code = parsed[key[0]].lines[key[1] - 1].strip()
            inst = t.owner.inst if cls == "sub" and t.owner is not None else None
            examples.append((name, key, where, cls, mkind, exported, mdecl, mtype, scene, node_path(t), inst, code))

    out.append("Assignments counted once per distinct classification; `_ready` / other functions.")
    out.append(f"Sites found: {totals['sites']}; classified differently in different scenes: {totals['ambiguous']}.")
    out.append("")
    out.append(f"{'':44}{'same scene':>14}{'into sub-scene':>18}{'outer scene':>15}")
    rows = [("resolved target, any property", None),
            ("(a) script member", KINDS),
            ("    of (a), the target is an instance root", tuple("inst_" + k for k in KINDS)),
            ("(b) script member without initializer", KINDS[1:]),
            ("    of (b), the target is an instance root", tuple("inst_" + k for k in KINDS[1:])),
            ("    typed as a class or object", ("object",)),
            ("    typed as a built-in or enum", ("default",)),
            ("    untyped", ("untyped",)),
            ("    of (b), exported", ("exported",)),
            ("of (a), through an exported reference", ("via_export",))]
    for label, kinds in rows:
        cells = []
        for cls in CLASSES:
            r, o = ([totals["any", cls, w] for w in ("ready", "other")] if kinds is None else row(totals, cls, kinds))
            cells.append(f"{r:>6} / {o:<6}")
        out.append(f"{label:44}" + "".join(f"{c:>15}   " for c in cells).rstrip())
    out.append("")
    out.append("Not classified (sites): " + ", ".join(f"{f} {totals['fail', f]}" for f in FAILS))
    out.append("  engine: the target resolves, but no script of it declares the property.")
    out.append("")
    out.append("By repository: (a) script member and (b) without initializer, `_ready` / other; not classified.")
    hdr = f"{'':26}{'(a) same':>11}{'(a) sub':>10}{'(b) same':>11}{'(b) sub':>10}  " + " ".join(f"{f[:8]:>8}" for f in FAILS)
    out.append(hdr)
    for name, C in per_repo:
        cells = [row(C, "same", KINDS), row(C, "sub", KINDS), row(C, "same", KINDS[1:]), row(C, "sub", KINDS[1:])]
        out.append(f"{name:26}" + "".join(f"{f'{r}/{o}':>11}" if i % 2 == 0 else f"{f'{r}/{o}':>10}" for i, (r, o) in enumerate(cells))
                   + "  " + " ".join(f"{C['fail', f]:>8}" for f in FAILS))
    for title, cond in (("Into a sub-scene, member without initializer", lambda e: e[3] == "sub" and e[4] not in ("init", None)),
                        ("Into a sub-scene, member with an initializer", lambda e: e[3] == "sub" and e[4] == "init"),
                        ("Into a sub-scene, engine property", lambda e: e[3] == "sub" and e[4] is None),
                        ("Outer scene", lambda e: e[3] == "outer"),
                        ("Same scene, in `_ready`, member without initializer",
                         lambda e: e[3] == "same" and e[4] not in ("init", None) and e[2] == "ready")):
        sel = [e for e in examples if cond(e)]
        out.append("")
        out.append(f"{title}: {len(sel)} (site, kind)")
        for name, key, where, cls, mkind, exported, mdecl, mtype, scene, tpath, inst, code in sorted(sel, key=lambda e: (e[2] != "ready", e[0], e[1])):
            via = f" (instance of {inst})" if inst else ""
            what = f"member {mdecl} ({mkind}{' exported' if exported else ''}, type {mtype})" if mdecl else "engine property"
            out.append(f"  [{where}] {name}/{key[0]}:{key[1]}  {code}")
            out.append(f"      -> {scene} :: {tpath}{via}; {what}")
    sys.stdout.reconfigure(encoding="utf-8")
    print("\n".join(out))


main()
