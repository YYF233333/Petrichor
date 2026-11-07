"""Where Godot looks members up by name, and which script members it would need to find.

Usage: python names.py CORPUS_DIR EXTENSION_API_JSON
    CORPUS_DIR:          fetched with ../cross-branch-duplication/gdcorpus.py
    EXTENSION_API_JSON:  thirdparty/godot-cpp/gdextension/extension_api-4-7.json (Godot 4.7.2, ed1daf0)

Channels, in first-party files only (gdcorpus.py's rules):

- code: a string literal at a parameter that names a member (POSITIONS), by the method's name, without
  type resolution. The target is `self` when the call has no receiver or the receiver (or the object
  argument) is `self`.
- scenes: signal connections of .tscn files; the property and method tracks of animations; the
  properties of SceneReplicationConfig; AnimationTree transition expressions.
- declarations: `@export` variables and `@rpc` functions, which the engine also reaches by name.
- names of project things rather than members (input actions, groups, animations, shader parameters,
  theme items, metadata), found from the parameter names of the engine API (THINGS).

A name is resolved, in this order: containing "/" -> "dynamic" (the engine lists such properties per
instance); on `self`, a member of the script or a first-party ancestor -> "script", else of its native
class -> "engine"; elsewhere, a member name of some engine class -> "engine", a name with exactly one root
declaration among the repo's scripts -> "script", several -> "ambiguous"; otherwise "unknown".

A script member is "external" when `.name` appears in a first-party script other than its declaring
script and that script's descendants (as in ../member-visibility/vis.py). One that is not external would
need `pub` only for the engine's lookup if non-pub members were hidden from it.
"""
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

# method name -> [(index of the name, kind, target, role)]; target "recv" is the receiver, an int is that
# argument, None is no single object (a group, or the node and its descendants). From the parameters of
# extension_api-4-7.json named method/property/signal that name a member of an object.
POSITIONS = {
    "call": [(0, "method", "recv", "ref")],
    "call_deferred": [(0, "method", "recv", "ref")],
    "callv": [(0, "method", "recv", "ref")],
    "call_thread_safe": [(0, "method", "recv", "ref")],
    "call_deferred_thread_group": [(0, "method", "recv", "ref")],
    "rpc": [(0, "method", "recv", "ref")],
    "rpc_id": [(1, "method", "recv", "ref")],
    "rpc_config": [(0, "method", "recv", "ref")],
    "get_method_argument_count": [(0, "method", "recv", "ref")],
    "has_method": [(0, "method", "recv", "query")],
    "Callable": [(1, "method", 0, "ref")],
    "add_do_method": [(1, "method", 0, "ref")],
    "add_undo_method": [(1, "method", 0, "ref")],
    "set": [(0, "property", "recv", "ref")],
    "get": [(0, "property", "recv", "ref")],
    "set_deferred": [(0, "property", "recv", "ref")],
    "set_thread_safe": [(0, "property", "recv", "ref")],
    "set_deferred_thread_group": [(0, "property", "recv", "ref")],
    "set_indexed": [(0, "property", "recv", "ref")],
    "get_indexed": [(0, "property", "recv", "ref")],
    "property_can_revert": [(0, "property", "recv", "ref")],
    "property_get_revert": [(0, "property", "recv", "ref")],
    "tween_property": [(1, "property", 0, "ref")],
    "add_do_property": [(1, "property", 0, "ref")],
    "add_undo_property": [(1, "property", 0, "ref")],
    "emit_signal": [(0, "signal", "recv", "ref")],
    "connect": [(0, "signal", "recv", "ref")],
    "disconnect": [(0, "signal", "recv", "ref")],
    "is_connected": [(0, "signal", "recv", "ref")],
    "has_connections": [(0, "signal", "recv", "ref")],
    "get_signal_connection_list": [(0, "signal", "recv", "ref")],
    "Signal": [(1, "signal", 0, "ref")],
    "has_signal": [(0, "signal", "recv", "query")],
    "has_user_signal": [(0, "signal", "recv", "query")],
    "add_user_signal": [(0, "signal", "recv", "define")],
    "remove_user_signal": [(0, "signal", "recv", "ref")],
    "call_group": [(1, "method", None, "ref")],
    "call_group_flags": [(2, "method", None, "ref")],
    "set_group": [(1, "property", None, "ref")],
    "set_group_flags": [(2, "property", None, "ref")],
    "propagate_call": [(0, "method", None, "ref")],
}

GROUPING = {"@export_group", "@export_subgroup", "@export_category"}
CONN_RE = re.compile(r'^\[connection ([^\]]*)\]', re.M)
NODEPATH_RE = re.compile(r'NodePath\("([^"]*)"\)')
TRACK_RE = re.compile(r'^tracks/(\d+)/(type|path|keys)$')
METHOD_KEY_RE = re.compile(r'"method":\s*&?"([^"]+)"')
REPL_RE = re.compile(r'^properties/(\d+)/path$')


def load_engine(path):
    api = json.load(open(path, encoding="utf-8"))
    parent, own = {}, {}
    for c in api["classes"]:
        parent[c["name"]] = c.get("inherits")
        own[c["name"]] = {
            "method": {m["name"] for m in c.get("methods", [])},
            "property": {p["name"] for p in c.get("properties", [])},
            "signal": {s["name"] for s in c.get("signals", [])},
        }
    G.NATIVE_PARENT.clear()
    G.NATIVE_PARENT.update(parent)
    names = set()
    for d in own.values():
        for s in d.values():
            names |= s

    def members(cls, kind):
        out, seen = set(), 0
        while cls in own and seen < 64:
            out |= own[cls][kind]
            cls, seen = parent.get(cls), seen + 1
        return out

    # Project things named by strings: parameter-name rules over every engine method.
    things = collections.defaultdict(set)
    anim = {"AnimationPlayer", "AnimationMixer", "AnimatedSprite2D", "AnimatedSprite3D", "SpriteFrames", "AnimationLibrary",
            "AnimationNodeStateMachinePlayback", "AnimationTree"}
    for c in api["classes"]:
        for m in c.get("methods", []):
            for i, a in enumerate(m.get("arguments", [])):
                n, t = a["name"], a["type"]
                if t not in ("StringName", "String"):
                    continue
                kind = None
                if n == "action" or n.endswith("_action") or (c["name"] == "Input" and m["name"] == "get_vector" and i < 4):
                    kind = "input action"
                elif n == "group":
                    kind = "group"
                elif c["name"] in anim and n in ("name", "anim", "animation", "to_node", "node") and not m["name"].startswith("_"):
                    kind = "animation"
                elif m["name"] in ("set_shader_parameter", "get_shader_parameter", "set_instance_shader_parameter", "get_instance_shader_parameter") and i == 0:
                    kind = "shader parameter"
                elif c["name"] == "Control" and re.match(r"(get|has|add|remove)_theme_\w+", m["name"]) and i == 0:
                    kind = "theme item"
                elif c["name"] == "Object" and m["name"] in ("get_meta", "set_meta", "has_meta", "remove_meta") and i == 0:
                    kind = "metadata"
                if kind:
                    things[m["name"]].add((i, kind))
    return names, members, things


def declarations(toks):
    """Top-level declarations: (kind, name, line, annotations)."""
    lines = collections.defaultdict(list)
    for t in toks:
        if t.k != "NL":
            lines[t.line].append(t)
    out, pending = [], []
    for ln in sorted(lines):
        ts = lines[ln]
        if ts[0].col != 0:
            continue
        j, notes = 0, []
        while j < len(ts) and ts[j].k == "AN":
            notes.append(ts[j].v)
            j += 1
            if j < len(ts) and ts[j].v == "(":
                depth = 0
                while j < len(ts):
                    depth += {"(": 1, ")": -1}.get(ts[j].v, 0)
                    j += 1
                    if depth == 0:
                        break
        if j == len(ts):
            pending += notes
            continue
        notes, pending = pending + notes, []
        if ts[j].v == "static":
            j += 1
        if j + 1 < len(ts) and ts[j].k == "ID" and ts[j].v in ("func", "var", "signal", "const") and ts[j + 1].k == "ID":
            out.append((ts[j].v, ts[j + 1].v, ln, notes))
    return out


def call_args(ts, i):
    """The arguments of the call whose `(` is ts[i], as token lists, and the index after `)`."""
    args, cur, depth, j = [], [], 0, i + 1
    while j < len(ts):
        t = ts[j]
        if t.k == "OP" and t.v in "([{":
            depth += 1
        elif t.k == "OP" and t.v in ")]}":
            if depth == 0:
                if cur:
                    args.append(cur)
                return args, j + 1
            depth -= 1
        elif t.k == "OP" and t.v == "," and depth == 0:
            args.append(cur)
            cur = []
            j += 1
            continue
        cur.append(t)
        j += 1
    return args, j


def receiver(ts, i):
    """'none', 'self' or 'other' for the call whose name is ts[i]."""
    if i == 0 or not (ts[i - 1].k == "OP" and ts[i - 1].v == "."):
        return "none"
    if i >= 2 and ts[i - 2].k == "ID" and ts[i - 2].v == "self" and not (i >= 3 and ts[i - 3].k == "OP" and ts[i - 3].v == "."):
        return "self"
    return "other"


def survey_repo(R, engine_names, engine_members, things, out):
    scripts = G.script_infos(R)
    fp = [f for f, s in scripts.items() if s["first_party"]]
    toks = {}
    for f in scripts:
        try:
            toks[f] = [t for t in G.tokenize(R.read(f)) if t.k != "NL"]
        except OSError:
            toks[f] = []
    decls = {f: declarations(toks[f]) for f in scripts}

    def ancestors(f):
        res, cur = [], scripts[f]["base_script"]
        while cur and cur not in res:
            res.append(cur)
            cur = scripts[cur]["base_script"]
        return res

    children = collections.defaultdict(set)
    for f in scripts:
        if scripts[f]["base_script"]:
            children[scripts[f]["base_script"]].add(f)

    def descendants(f):
        res, stack = set(), [f]
        while stack:
            for c in children.get(stack.pop(), ()):
                if c not in res:
                    res.add(c)
                    stack.append(c)
        return res

    declared = collections.defaultdict(set)
    for f, ds in decls.items():
        for _, n, _, _ in ds:
            declared[n].add(f)
    roots = {n: [f for f in fs if not any(a in fs for a in ancestors(f))] for n, fs in declared.items()}
    dotted = {}
    for f in fp:
        ts = toks[f]
        dotted[f] = collections.Counter(ts[i].v for i in range(1, len(ts)) if ts[i].k == "ID" and ts[i - 1].k == "OP" and ts[i - 1].v == ".")

    def declaring(f, name):
        for g in [f] + ancestors(f):
            if any(n == name for _, n, _, _ in decls.get(g, ())):
                return g
        return None

    def external(f, name):
        desc = descendants(f)
        return any(dotted[g][name] for g in fp if g != f and g not in desc)

    named = collections.defaultdict(set)  # (script, name) -> channels naming it

    def resolve(name, kind, on_script):
        """(class, script that declares it or None)."""
        if "/" in name:
            return "dynamic", None
        head = name.split(":")[0]
        if on_script is not None:
            g = declaring(on_script, head)
            if g:
                return "script", g
            nat = scripts[on_script]["native"]
            if nat and head in engine_members(nat, kind):
                return "engine", None
            return "unknown", None
        if head in engine_names:
            return "engine", None
        rs = roots.get(head, [])
        if len(rs) == 1:
            return "script", rs[0]
        return ("ambiguous" if rs else "unknown"), None

    def note(channel, name, cls, script, line):
        out["count"][(channel, cls)] += 1
        if cls == "script" and script in scripts and scripts[script]["first_party"]:
            named[(script, name.split(":")[0])].add(channel)
        if len(out["examples"][(channel, cls)]) < 4:
            out["examples"][(channel, cls)].append(f"{R.name}/{line}")

    # Code.
    for f in fp:
        ts = toks[f]
        for i in range(len(ts) - 1):
            t = ts[i]
            if t.k != "ID" or not (ts[i + 1].k == "OP" and ts[i + 1].v == "("):
                continue
            if i > 0 and ts[i - 1].k == "ID" and ts[i - 1].v == "func":
                continue
            specs = POSITIONS.get(t.v, []) + [(idx, kind, "thing", "thing") for idx, kind in things.get(t.v, ())]
            if not specs:
                continue
            args, _ = call_args(ts, i + 1)
            recv = receiver(ts, i)
            for idx, kind, target, role in specs:
                if idx >= len(args) or len(args[idx]) != 1 or args[idx][0].k != "STR":
                    continue
                name = args[idx][0].v
                where = f"{f}:{t.line}"
                if role == "thing":
                    out["count"][("thing", kind)] += 1
                    continue
                if target is None:
                    on = None
                    who = "no single object"
                elif target == "recv":
                    who = "self" if recv in ("none", "self") else "other"
                else:
                    a = args[target] if target < len(args) else []
                    who = "self" if len(a) == 1 and a[0].k == "ID" and a[0].v == "self" else "other"
                on = f if who == "self" else None
                if t.v in ("get", "set") and who == "other":
                    who = "other (get/set, may be a Dictionary)"
                cls, g = resolve(name, kind, on)
                channel = f"code {kind} {role} on {who}"
                note(channel, name, cls, g, where)
                out["calls"][t.v] += 1

    # Scenes and resources.
    contexts = G.scene_contexts(R)
    at = {}
    for s, ctxs in contexts.items():
        for c in ctxs:
            at[(c["scene"], c["path"])] = s
    for f in R.files:
        if not (f.endswith(".tscn") or f.endswith(".tres")) or not R.first_party(f):
            continue
        try:
            text = R.read(f)
        except OSError:
            continue
        if f.endswith(".tscn"):
            for m in CONN_RE.finditer(text):
                attrs = {k: G.unq(v) for k, v in G.ATTR_RE.findall(m.group(1))}
                target = attrs.get("to", "")
                s = at.get((f, target if target else "."))
                method = attrs.get("method", "")
                line = f"{f}:{text[:m.start()].count(chr(10)) + 1}"
                if s is None:
                    note("scene connection", method, "no script", None, line)
                    continue
                cls, g = resolve(method, "method", s)
                note("scene connection", method, cls, g, line)
        secs = G.parse_sections(text)
        for sec in secs:
            typ = G.unq(sec.attrs.get("type", ""))
            if typ == "Animation":
                tracks = collections.defaultdict(dict)
                for k, v in sec.props.items():
                    m = TRACK_RE.match(k)
                    if m:
                        tracks[m.group(1)][m.group(2)] = v
                for tr in tracks.values():
                    ttype = G.unq(tr.get("type", ""))
                    pm = NODEPATH_RE.search(tr.get("path", ""))
                    path = pm.group(1) if pm else ""
                    line = f"{f}:{sec.line}"
                    if ttype in ("value", "bezier") and ":" in path:
                        prop = path.split(":", 1)[1]
                        cls, g = resolve(prop, "property", None)
                        note("animation property track", prop, cls, g, line)
                    elif ttype == "method":
                        for name in METHOD_KEY_RE.findall(tr.get("keys", "")):
                            cls, g = resolve(name, "method", None)
                            note("animation method key", name, cls, g, line)
            elif typ == "SceneReplicationConfig":
                for k, v in sec.props.items():
                    pm = NODEPATH_RE.search(v) if REPL_RE.match(k) else None
                    if pm and ":" in pm.group(1):
                        prop = pm.group(1).split(":", 1)[1]
                        cls, g = resolve(prop, "property", None)
                        note("replicated property", prop, cls, g, f"{f}:{sec.line}")
            v = sec.props.get("advance_expression")
            if v and G.unq(v).strip():
                out["count"][("transition expression", "-")] += 1

    # Declarations the engine reaches by name.
    for f in fp:
        for kind, n, ln, notes in decls[f]:
            if kind == "var" and any(a.startswith("@export") and a not in GROUPING for a in notes):
                named[(f, n)].add("@export")
                out["count"][("@export var", "script")] += 1
            if kind == "func" and "@rpc" in notes:
                named[(f, n)].add("@rpc")
                out["count"][("@rpc func", "script")] += 1

    # Which of the named members would need pub only for the engine.
    for (f, n), channels in named.items():
        ext = external(f, n)
        key = " + ".join(sorted(channels))
        out["members"][(key, "external" if ext else "not external")] += 1
        for ch in channels:
            out["by_channel"][(ch, "external" if ext else "not external")] += 1
            if not ext and n.startswith("_"):
                out["by_channel"][(ch, "not external, named _x")] += 1
        if not ext:
            out["no_pub"].add((R.name, f, n))
    out["loc"] += sum(1 for f in fp for ln in R.read(f).splitlines() if ln.strip() and not ln.strip().startswith("#"))
    out["fp_members"] += sum(1 for f in fp for _ in decls[f])
    out["external_members"] += sum(1 for f in fp for _, n, _, _ in decls[f] if external(f, n))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    corpus, api_path = sys.argv[1], sys.argv[2]
    engine_names, engine_members, things = load_engine(api_path)
    outs = {}
    for label in ("all repositories", "games"):
        out = outs[label] = {"count": collections.Counter(), "examples": collections.defaultdict(list), "calls": collections.Counter(),
                             "members": collections.Counter(), "by_channel": collections.Counter(), "no_pub": set(),
                             "loc": 0, "fp_members": 0, "external_members": 0}
        for name, _, _, group in G.REPOS:
            if label == "all repositories" or group == "game":
                survey_repo(G.Repo(corpus, name, group), engine_names, engine_members, things, out)

    for label, out in outs.items():
        print(f"# {label}\n")
        print("## Names by channel and resolution\n")
        channels = sorted({c for c, _ in out["count"]})
        for ch in channels:
            row = {cls: n for (c, cls), n in out["count"].items() if c == ch}
            print(f"{ch}: {sum(row.values())} — " + ", ".join(f"{k} {v}" for k, v in sorted(row.items(), key=lambda kv: -kv[1])))
        print("\n## Script members the engine reaches by name\n")
        print(f"first-party code lines: {out['loc']}; top-level members: {out['fp_members']}, external (pub anyway): {out['external_members']}")
        for ch in sorted({c for c, _ in out["by_channel"]}):
            print(f"{ch}: external {out['by_channel'][(ch, 'external')]}, not external {out['by_channel'][(ch, 'not external')]}"
                  f" (named _x {out['by_channel'][(ch, 'not external, named _x')]})")
        print(f"members named by any channel and not external: {len(out['no_pub'])}\n")
    out = outs["all repositories"]
    print("# all repositories, details\n")
    print("by combination of channels:")
    for (key, ext), n in sorted(out["members"].items(), key=lambda kv: -kv[1]):
        print(f"  {key} [{ext}]: {n}")
    print("\n## Calls matched, by method\n")
    print(", ".join(f"{k} {v}" for k, v in out["calls"].most_common()))
    print("\n## Examples\n")
    for (ch, cls), ex in sorted(out["examples"].items()):
        print(f"{ch} / {cls}:")
        for e in ex:
            print("   " + e)


if __name__ == "__main__":
    main()
