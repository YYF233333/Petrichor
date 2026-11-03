"""How first-party GDScript in the corpus declares and uses inner classes (`class Name [extends X]:`).

Usage: python inner_classes.py CORPUS_DIR EXTENSION_API_JSON [--samples N]
       python inner_classes.py --selftest
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py (first-party rules and groups from there)
    EXTENSION_API_JSON: thirdparty/godot-cpp/gdextension/extension_api-4-7.json (engine class hierarchy and members)

Token-based (gdcorpus tokenizer), no type resolution: an inner class is `class` first on a logical line (after
optional annotations); its body is the following logical lines indented deeper; `extends` is read from the class
line or from the first body line (gdscript_parser.cpp:960-980 at ed1daf0). Names are matched by spelling.
"""
import collections
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

SAMPLES = 4
PARENT, MEMBERS = {}, {}


def load_api(path):
    with open(path, encoding="utf-8") as fh:
        api = json.load(fh)
    for c in api["classes"]:
        PARENT[c["name"]] = c.get("inherits")
        m = set()
        for k in ("methods", "properties", "signals", "constants"):
            for x in c.get(k, []) or []:
                m.add(x["name"])
        for e in c.get("enums", []) or []:
            m.add(e["name"])
            for v in e.get("values", []):
                m.add(v["name"])
        MEMBERS[c["name"]] = m


def native_chain(cls):
    out = []
    while cls and len(out) < 64:
        out.append(cls)
        cls = PARENT.get(cls)
    return out


def native_is(cls, base):
    return base in native_chain(cls)


def native_members(cls):
    s = set()
    for c in native_chain(cls):
        s |= MEMBERS.get(c, set())
    return s


# ------------------------------------------------------------------------------------------------
# Structure of one file

class Cls:
    def __init__(self, name, line, indent, start, parent, depth):
        self.name, self.line, self.indent, self.start, self.parent, self.depth = name, line, indent, start, parent, depth
        self.end = None  # token index (exclusive)
        self.body_start = None
        self.children = []
        self.extends = None  # (kind, value) kind: name | path
        self.extends_in_body = False
        self.inline = False
        self.annotations = []
        self.file = None
        self.native = None
        self.consts, self.preload_consts, self.enums, self.enum_values = set(), set(), set(), set()
        self.signals, self.static_funcs, self.static_vars, self.vars, self.funcs = set(), set(), set(), set(), set()
        self.onready = 0
        self.exports = 0
        self.init_params = None  # None: no _init; int: number of params
        self.member_lines = []  # (kind, line)
        self.decls = set()  # every name declared inside own tokens (shadowing)


def logical_starts(ts):
    """Indices of tokens that begin a logical line (first on a physical line at bracket depth 0)."""
    starts = []
    depth = 0
    for i, t in enumerate(ts):
        if t.first and depth == 0:
            starts.append(i)
        if t.k == "OP":
            if t.v in "([{":
                depth += 1
            elif t.v in ")]}":
                depth = max(0, depth - 1)
    return starts


def skip_annotations(ts, a, b):
    j = a
    ans = []
    while j < b and ts[j].k == "AN":
        ans.append(ts[j].v)
        j += 1
        if j < b and ts[j].v == "(":
            d = 0
            while j < b:
                if ts[j].v == "(":
                    d += 1
                elif ts[j].v == ")":
                    d -= 1
                    if d == 0:
                        j += 1
                        break
                j += 1
    return j, ans


def read_extends(ts, x, b):
    nx = ts[x]
    if nx.k == "STR":
        return ("path", nx.v), x + 1
    if nx.k == "ID":
        name = nx.v
        x += 1
        while x + 1 < b and ts[x].v == "." and ts[x + 1].k == "ID":
            name += "." + ts[x + 1].v
            x += 2
        return ("name", name), x
    return None, x


def parse_file(src, rel):
    ts = [t for t in G.tokenize(src) if t.k != "NL"]
    starts = logical_starts(ts)
    top = Cls(None, 0, -1, 0, None, 0)
    top.end = len(ts)
    top.body_start = 0
    top.file = rel
    classes = []
    stack = [top]
    lines = []
    for k, a in enumerate(starts):
        b = starts[k + 1] if k + 1 < len(starts) else len(ts)
        lines.append((a, b, ts[a].col))
    for li, (a, b, ind) in enumerate(lines):
        while len(stack) > 1 and ind <= stack[-1].indent:
            stack[-1].end = a
            stack.pop()
        j, _ = skip_annotations(ts, a, b)
        if j < b and ts[j].k == "ID" and ts[j].v == "class" and j + 1 < b and ts[j + 1].k == "ID":
            par = stack[-1]
            c = Cls(ts[j + 1].v, ts[j].line, ind, a, par, par.depth + 1)
            c.file = rel
            c.annotations = [ts[x].v for x in range(a, j) if ts[x].k == "AN"]
            p = li - 1
            while p >= 0:
                pa, pb, _ = lines[p]
                if ts[pa].k == "AN" and skip_annotations(ts, pa, pb)[0] >= pb:
                    c.annotations = [ts[x].v for x in range(pa, pb) if ts[x].k == "AN"] + c.annotations
                    p -= 1
                else:
                    break
            x = j + 2
            if x < b and ts[x].v == "extends" and x + 1 < b:
                c.extends, x = read_extends(ts, x + 1, b)
            while x < b and ts[x].v != ":":
                x += 1
            c.body_start = x + 1
            if c.body_start < b:  # single-line class
                c.end = b
                c.inline = True
            else:
                stack.append(c)
            par.children.append(c)
            classes.append(c)
    for c in stack[1:]:
        c.end = len(ts)
    for c in classes:
        if c.extends is None and not c.inline:
            first = next(((a, b) for a, b, _ in lines if c.body_start <= a < c.end), None)
            if first and ts[first[0]].v == "extends" and first[0] + 1 < first[1]:
                c.extends, _ = read_extends(ts, first[0] + 1, first[1])
                c.extends_in_body = True
    for c in [top] + classes:
        collect_members(c, ts, lines)
    return ts, top, classes


def own_indices(c):
    rng = [(ch.start, ch.end) for ch in c.children]
    i = c.body_start
    out = []
    while i < c.end:
        skip = next((e for s, e in rng if s <= i < e), None)
        if skip is not None:
            i = skip
            continue
        out.append(i)
        i += 1
    return out


def collect_members(c, ts, lines):
    rng = [(ch.start, ch.end) for ch in c.children]
    body_lines = [(a, b, ind) for (a, b, ind) in lines if c.body_start <= a < c.end and not any(s <= a < e for s, e in rng)]
    if c.inline:
        body_lines = [(c.body_start, c.end, 10 ** 6)]
    if body_lines:
        mind = min(ind for _, _, ind in body_lines)
        pending_export = False
        for a, b, ind in body_lines:
            if ind != mind:
                continue
            j, ans = skip_annotations(ts, a, b)
            exp = pending_export or any(x.startswith("@export") and x not in ("@export_group", "@export_subgroup", "@export_category") for x in ans)
            onr = "@onready" in ans
            if j >= b:
                pending_export = exp
                continue
            pending_export = False
            t = ts[j]
            static = False
            if t.v == "static" and j + 1 < b:
                static = True
                j += 1
                t = ts[j]
            nx = ts[j + 1] if j + 1 < b else None
            if t.k != "ID" or nx is None:
                continue
            line = t.line
            if t.v == "const" and nx.k == "ID":
                rest = [ts[x].v for x in range(j + 2, b)]
                if "preload" in rest or "load" in rest:
                    c.preload_consts.add(nx.v)
                c.consts.add(nx.v)
                c.member_lines.append(("const", line))
            elif t.v == "enum":
                if nx.k == "ID":
                    c.enums.add(nx.v)
                    c.member_lines.append(("enum", line))
                else:
                    c.member_lines.append(("enum (unnamed)", line))
                    d, prev_open = 0, False
                    for x in range(j + 1, b):
                        if ts[x].v == "{":
                            d += 1
                            prev_open = True
                            continue
                        if ts[x].v == "}":
                            d -= 1
                            continue
                        if d == 1 and ts[x].k == "ID" and (prev_open or ts[x - 1].v == ","):
                            c.enum_values.add(ts[x].v)
                        prev_open = False
            elif t.v == "signal" and nx.k == "ID":
                c.signals.add(nx.v)
                c.member_lines.append(("signal", line))
            elif t.v == "var" and nx.k == "ID":
                (c.static_vars if static else c.vars).add(nx.v)
                if static:
                    c.member_lines.append(("static var", line))
                if exp:
                    c.exports += 1
                    c.member_lines.append(("@export var", line))
                if onr:
                    c.onready += 1
                    c.member_lines.append(("@onready var", line))
            elif t.v == "func" and nx.k == "ID":
                (c.static_funcs if static else c.funcs).add(nx.v)
                if static and nx.v != "_static_init":
                    c.member_lines.append(("static func", line))
                if nx.v == "_init" and not static:
                    d, n, seen = 0, 0, False
                    for x in range(j + 2, b):
                        v = ts[x].v
                        if v == "(":
                            d += 1
                            continue
                        if v == ")":
                            d -= 1
                            if d == 0:
                                break
                            continue
                        if d == 1 and not seen and ts[x].k == "ID":
                            n += 1
                            seen = True
                        if d == 1 and v == ",":
                            seen = False
                    c.init_params = n
                    c.member_lines.append(("_init with params" if n else "_init without params", line))
                if nx.v == "_static_init":
                    c.member_lines.append(("_static_init", line))
    # names declared anywhere in own tokens (members, locals, parameters, loop variables)
    idx = own_indices(c)
    for i in idx:
        t = ts[i]
        if t.k == "ID" and t.v in ("var", "const", "func", "signal", "enum", "class", "for") and i + 1 < len(ts) and ts[i + 1].k == "ID":
            c.decls.add(ts[i + 1].v)
        if t.k == "ID" and t.v == "func":
            x = i + 1
            if x < len(ts) and ts[x].k == "ID":
                x += 1
            if x < len(ts) and ts[x].v == "(":
                d, expect = 0, True
                while x < len(ts):
                    v = ts[x].v
                    if v in "([{" and ts[x].k == "OP":
                        d += 1
                        if d == 1:
                            expect = True
                            x += 1
                            continue
                    elif v in ")]}" and ts[x].k == "OP":
                        d -= 1
                        if d == 0:
                            break
                    elif d == 1 and v == ",":
                        expect = True
                        x += 1
                        continue
                    if d == 1 and expect and ts[x].k == "ID":
                        c.decls.add(ts[x].v)
                    if d == 1:
                        expect = False
                    x += 1
    c.decls |= c.enum_values | {ch.name for ch in c.children}


# ------------------------------------------------------------------------------------------------
# Item 5: outer declarations used by bare name inside an inner class

def outer_scope_names(c, own_cname):
    chain = []
    s = c.parent
    while s is not None:
        chain.append(s)
        s = s.parent
    cat = {}
    for sc in reversed(chain):  # nearer scopes override
        for n in sc.consts:
            cat[n] = "const (preload/load)" if n in sc.preload_consts else "const"
        for n in sc.enums:
            cat[n] = "enum (named)"
        for n in sc.enum_values:
            cat[n] = "unnamed-enum value"
        for n in sc.static_funcs:
            cat[n] = "static func"
        for n in sc.static_vars:
            cat[n] = "static var"
        for ch in sc.children:
            cat[ch.name] = "self" if ch is c else "other inner class"
        for n in sc.vars:
            cat[n] = "INSTANCE var"
        for n in sc.funcs:
            cat[n] = "INSTANCE func"
        for n in sc.signals:
            cat[n] = "INSTANCE signal"
        if sc.name is None and own_cname:
            cat[own_cname] = "outer class_name (any use, e.g. back-reference type)"
    return cat


def outer_uses(c, ts, own_cname, shadow):
    """[(category, name, line)] once per (category, name) for this inner class."""
    cat = outer_scope_names(c, own_cname)
    seen, out = set(), []
    for i in own_indices(c):
        t = ts[i]
        if t.k != "ID" or t.v not in cat:
            continue
        pv = ts[i - 1] if i else None
        if pv is not None and (pv.v in (".", "$") or (pv.v == "%" and pv.col + 1 == t.col) or pv.k == "AN"
                               or pv.v in ("var", "const", "func", "signal", "enum", "class", "for")):
            continue
        if pv is not None and pv.v == "/" and any(ts[x].v == "$" for x in range(max(0, i - 8), i) if ts[x].line == t.line):
            continue
        nx = ts[i + 1] if i + 1 < len(ts) else None
        nn = ts[i + 2] if i + 2 < len(ts) else None
        if nx is not None and nx.v == "=" and pv is not None and pv.v in ("{", ",") and not (nn is not None and nn.v == "="):
            continue  # Lua-style dictionary key
        category = cat[t.v]
        if category == "self":
            continue
        if t.v in shadow and not category.startswith("outer class_name"):
            continue
        if (category, t.v) in seen:
            continue
        seen.add((category, t.v))
        out.append((category, t.v, t.line))
    return out


# ------------------------------------------------------------------------------------------------
# Reference context

def is_arrow(ts, i):
    return i >= 1 and ts[i].v == ">" and ts[i - 1].v == "-" and ts[i - 1].line == ts[i].line and ts[i - 1].col + 1 == ts[i].col


def ref_context(ts, i, j):
    """Context of a class reference spanning tokens i..j (inclusive)."""
    prev = ts[i - 1] if i else None
    n1 = ts[j + 1] if j + 1 < len(ts) else None
    n2 = ts[j + 2] if j + 2 < len(ts) else None
    n3 = ts[j + 3] if j + 3 < len(ts) else None
    if prev is not None and prev.v == "class":
        return "declaration"
    if n1 is not None and n1.v == "." and n2 is not None and n2.k == "ID":
        if n2.v == "new" and n3 is not None and n3.v == "(":
            return ".new()"
        if n3 is not None and n3.v == "(":
            return "static call Inner.f()"
        if n2.v.isupper():
            return "Inner.CONST"
        if n2.v[:1].isupper():
            return "Inner.Name (CamelCase: enum or class)"
        return "Inner.member (lowercase: static var or other)"
    if prev is None:
        return "other (value)"
    if prev.v == "extends":
        return "extends"
    if prev.v == "is":
        return "is"
    if prev.v == "as":
        return "as"
    if prev.v == ":" or is_arrow(ts, i - 1):
        return "type annotation"
    if prev.v in ("[", ","):
        d = 0
        k = i - 1
        while k >= 0 and ts[k].line == ts[i].line:
            v = ts[k].v
            if v in ")]}":
                d += 1
            elif v in "([{":
                if d == 0:
                    if v == "[" and k > 0 and ts[k - 1].v in ("Array", "Dictionary"):
                        return "type annotation"
                    break
                d -= 1
            k -= 1
    return "other (value)"


# ------------------------------------------------------------------------------------------------

def is_test_file(rel):
    p = "/" + rel.lower()
    return "/test/" in p or "/tests/" in p or os.path.basename(p).startswith("test_") or p.endswith("_test.gd")


def autoloads(repo):
    out = {}
    for root, proj in repo.projects.items():
        pg = (root + "/" if root else "") + "project.godot"
        if pg not in repo.fileset:
            continue
        sec = None
        for ln in repo.read(pg).splitlines():
            s = ln.strip()
            if s.startswith("["):
                sec = s
                continue
            if sec == "[autoload]" and "=" in s:
                k, v = s.split("=", 1)
                v = v.strip().strip('"').lstrip("*")
                rel = proj.to_rel(v)
                if rel and rel.endswith(".tscn") and rel in repo.fileset:
                    try:
                        sf = G.SceneFile(rel, repo.read(rel))
                        sc = sf.nodes[0].props.get("script") if sf.nodes else None
                        m = G.EXT_REF_RE.search(sc or "")
                        rel = sf.ext.get(m.group(1), (None, None, None))[1] if m else None
                        rel = proj.to_rel(rel) if rel else None
                    except Exception:  # noqa
                        rel = None
                if rel and rel.endswith(".gd"):
                    out[(root, k.strip())] = rel
    return out


def purpose(c, test):
    lname = c.name.lower()
    inst_funcs = {x for x in c.funcs if x not in ("_init", "_to_string")}
    if test:
        return "in a test file"
    if c.native and native_is(c.native, "Node"):
        return "Node subclass"
    if "state" in lname:
        return "name mentions State"
    if not c.vars and not c.funcs and (c.consts or c.enums or c.static_funcs or c.static_vars or c.children):
        return "namespace (only const/enum/static/inner classes)"
    if not c.vars and not inst_funcs and not c.consts and not c.enums and not c.static_funcs:
        return "empty / marker (maybe _init)"
    if c.vars and not inst_funcs:
        return "data record (vars, maybe _init/_to_string)"
    if not c.vars and inst_funcs:
        return "stateless object (instance methods, no vars)"
    return "object with state and methods"


def main():
    args = sys.argv[1:]
    global SAMPLES
    if "--samples" in args:
        k = args.index("--samples")
        SAMPLES = int(args[k + 1])
        del args[k:k + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    corpus, api_path = args
    load_api(api_path)

    keys = ("all", "games")
    C = {k: collections.defaultdict(collections.Counter) for k in keys}
    S = {k: collections.defaultdict(list) for k in keys}
    per_repo = []
    repo_uses = collections.defaultdict(collections.Counter)
    detail = collections.defaultdict(list)

    def bump(group, sec, item, loc):
        for k in (keys if group == "game" else ("all",)):
            C[k][sec][item] += 1
            if len(S[k][(sec, item)]) < SAMPLES:
                S[k][(sec, item)].append(loc)

    for name, _, _, group in G.REPOS:
        repo = G.Repo(corpus, name, group)
        files = {}
        for f in repo.files:
            if f.endswith(".gd"):
                try:
                    src = repo.read(f)
                except OSError:
                    continue
                files[f] = parse_file(src, f) + (G.parse_header(src),)
        fp = {f for f in files if repo.first_party(f)}
        cname = collections.defaultdict(dict)  # all scripts, vendored included: their class_names are visible
        for f, (_, _, _, hdr) in files.items():
            if hdr["class_name"]:
                cname[repo.project_of(f).root][hdr["class_name"]] = f
        al = autoloads(repo)
        memo, pmemo = {}, {}

        def file_native(f, seen=()):
            if f in memo:
                return memo[f]
            if f in seen or f not in files:
                return None
            hdr = files[f][3]
            proj = repo.project_of(f)
            res = None
            if hdr["extends_kind"] is None:
                res = "RefCounted"
            elif hdr["extends_kind"] == "path":
                b = proj.to_rel(hdr["extends"], f)
                if b and b.startswith("uid://"):
                    b = repo.uid.get(b)
                res = file_native(b, seen + (f,)) if b else None
            else:
                nm = hdr["extends"]
                if nm in PARENT:
                    res = nm
                elif nm in cname[proj.root]:
                    res = file_native(cname[proj.root][nm], seen + (f,))
            memo[f] = res
            return res

        def file_decls(f):
            ts = files[f][0]
            return {ts[i + 1].v for i, t in enumerate(ts[:-1])
                    if t.k == "ID" and t.v in ("var", "const", "func", "signal", "enum", "class") and ts[i + 1].k == "ID"}

        def preload_map(f):
            """NAME -> file for `const/var NAME [: T] = preload/load("x.gd")` anywhere in f."""
            if f in pmemo:
                return pmemo[f]
            ts = files[f][0]
            out = {}
            proj = repo.project_of(f)
            for i, t in enumerate(ts[:-5]):
                if t.v in ("const", "var") and ts[i + 1].k == "ID":
                    k = i + 2
                    if ts[k].v == ":":
                        k += 1
                        while k < len(ts) and ts[k].v != "=" and ts[k].line == t.line:
                            k += 1
                    if k + 3 < len(ts) and ts[k].v == "=" and ts[k + 1].v in ("preload", "load") and ts[k + 2].v == "(" and ts[k + 3].k == "STR":
                        tgt = proj.to_rel(ts[k + 3].v, f)
                        if tgt and tgt.startswith("uid://"):
                            tgt = repo.uid.get(tgt)
                        if tgt in files:
                            out[ts[i + 1].v] = tgt
            pmemo[f] = out
            return out

        def top_inner(f):
            return {c.name: c for c in files[f][2] if c.parent.name is None} if f in files else {}

        def base_file(c):
            """Script file an inner class extends (path or class_name), if any."""
            if c.extends is None:
                return None
            kind, v = c.extends
            proj = repo.project_of(c.file)
            if kind == "path":
                b = proj.to_rel(v, c.file)
                return b if b in files else None
            if "." not in v and v in cname[proj.root] and v not in {x.name for x in files[c.file][2]}:
                return cname[proj.root][v]
            return None

        def nested(cls, nm):
            """Inner class nm reachable as cls.nm: declared in cls or inherited from its script base."""
            for ch in cls.children:
                if ch.name == nm:
                    return ch
            bf = base_file(cls)
            if bf:
                return top_inner(bf).get(nm)
            return None

        def resolve_native(c):
            seen = set()
            cur = c
            while cur is not None and id(cur) not in seen:
                seen.add(id(cur))
                if cur.extends is None:
                    return "RefCounted"
                kind, v = cur.extends
                f = cur.file
                proj = repo.project_of(f)
                if kind == "path":
                    b = proj.to_rel(v, f)
                    return file_native(b) if b in files else None
                parts = v.split(".")
                if len(parts) == 1:
                    if v in PARENT:
                        return v
                    same = [x for x in files[f][2] if x.name == v]
                    if same:
                        cur = same[0]
                        continue
                    if v in cname[proj.root]:
                        return file_native(cname[proj.root][v])
                    return None
                head = parts[0]
                tf = cname[proj.root].get(head) or preload_map(f).get(head) or al.get((proj.root, head))
                cur = top_inner(tf).get(parts[1]) if tf else None
            return None

        repo_classes = repo_files = 0
        for f in sorted(fp):
            ts, top, classes, hdr = files[f]
            for c in classes:
                c.native = resolve_native(c)
            if not classes:
                continue
            repo_files += 1
            repo_classes += len(classes)
            proj = repo.project_of(f)
            own_cname = hdr["class_name"]
            names_in_file = {c.name for c in classes}
            pm = preload_map(f)
            test = is_test_file(f)
            bump(group, "1 totals", "files with inner classes", f"{name}/{f}")
            if hdr["class_name"]:
                bump(group, "1 totals", "files with inner classes that have a class_name", f"{name}/{f}")
            if f in al.values():
                bump(group, "1 totals", "files with inner classes that are autoloads", f"{name}/{f}")
            for c in classes:
                loc = f"{name}/{f}:{c.line} {c.name}"
                bump(group, "1 totals", "inner classes", loc)
                if test:
                    bump(group, "1 totals", "inner classes in test files", loc)
                # 2 extends
                if c.extends is None:
                    kind = "nothing (implicit RefCounted)"
                elif c.extends[0] == "path":
                    kind = "path string"
                else:
                    v = c.extends[1]
                    parts = v.split(".")
                    if len(parts) == 1 and v in PARENT:
                        if v in ("RefCounted", "Object", "Resource", "Node"):
                            kind = "engine: " + v
                        elif native_is(v, "Node"):
                            kind = "engine: other Node subclass"
                        elif native_is(v, "Resource"):
                            kind = "engine: other Resource subclass"
                        else:
                            kind = "engine: other"
                        bump(group, "2b engine bases", v, loc)
                    elif len(parts) == 1 and v in names_in_file:
                        kind = "inner class of the same file"
                    elif len(parts) == 1 and v in cname[proj.root]:
                        kind = "class_name class"
                    elif len(parts) > 1 and parts[0] == own_cname:
                        kind = "inner class of the same file (via own class_name)"
                    elif len(parts) > 1 and parts[0] in cname[proj.root]:
                        kind = "Other.Inner (class_name)"
                    elif len(parts) > 1 and parts[0] in pm:
                        kind = "Other.Inner (preload const)"
                    else:
                        kind = "unresolved name: " + v
                bump(group, "2 extends", kind, f"{loc} extends {c.extends[1] if c.extends else ''}")
                if c.extends_in_body:
                    bump(group, "2c extends written on the first body line", "yes", loc)
                bump(group, "2d native ancestor", "Node-derived" if c.native and native_is(c.native, "Node") else str(c.native), loc)
                # 3 depth
                bump(group, "3 nesting depth", f"depth {c.depth}", loc)
                # 4 contents
                for a in c.annotations:
                    bump(group, "4a annotations on/before the class line", a, loc)
                has = {}
                for k_, ln in c.member_lines:
                    has.setdefault(k_, ln)
                if c.children:
                    has["inner classes"] = c.children[0].line
                for h, ln in sorted(has.items()):
                    bump(group, "4 contains (classes having)", h, f"{name}/{f}:{ln} {c.name}")
                # 5 outer declarations
                shadow = c.decls | c.vars | c.funcs | c.consts | c.enums | c.signals | c.static_funcs | c.static_vars
                if c.native:
                    shadow |= native_members(c.native)
                bf = base_file(c)
                if bf:
                    shadow |= file_decls(bf)
                if c.extends and c.extends[0] == "name" and c.extends[1] in names_in_file:
                    bc = next(x for x in classes if x.name == c.extends[1])
                    shadow |= bc.decls | bc.vars | bc.funcs | bc.consts | bc.enums
                uses = outer_uses(c, ts, own_cname, shadow)
                for category, nm, ln in uses:
                    bump(group, "5 outer declaration used inside (per class+name)", category, f"{name}/{f}:{ln} {c.name} -> {nm}")
                    if category.startswith("INSTANCE"):
                        detail["5 instance-member candidates"].append(f"{name}/{f}:{ln} {c.name} -> {nm}")
                cats = {u[0] for u in uses} - {"outer class_name (any use, e.g. back-reference type)"}
                if cats:
                    bump(group, "5 summary", "inner classes using some outer declaration bare", loc)
                for cat_ in cats:
                    bump(group, "5 classes using, by kind", cat_, loc)
                # 9 purpose
                p = purpose(c, test)
                bump(group, "9 purpose", p, loc)
                if test:
                    bump(group, "9b purpose of test-file classes", purpose(c, False), loc)
                detail["9 " + p].append(f"{loc} extends={c.extends[1] if c.extends else '-'} vars={len(c.vars)} "
                                        f"consts={len(c.consts)} funcs={sorted(c.funcs | c.static_funcs)[:5]}")
                if c.native and native_is(c.native, "Node"):
                    detail["8 Node-derived inner classes"].append(f"{loc} extends {c.extends[1] if c.extends else '-'} native={c.native}")
        per_repo.append((name, group, len(fp), repo_files, repo_classes))

        # 6/7/8: references to inner classes in first-party files
        for f in sorted(fp):
            ts, top, classes, hdr = files[f]
            proj = repo.project_of(f)
            own_cname = hdr["class_name"]
            pm = preload_map(f)
            same_all = {c.name for c in classes}
            spans = [(c.start, c.end) for c in classes]
            for i, t in enumerate(ts):
                if t.k != "ID":
                    continue
                pv = ts[i - 1] if i else None
                if pv is not None and pv.v == ".":
                    continue
                n1 = ts[i + 1] if i + 1 < len(ts) else None
                n2 = ts[i + 2] if i + 2 < len(ts) else None
                where = "inside an inner class" if any(s <= i < e for s, e in spans) else "in the outer class"
                if n1 is not None and n1.v == "." and n2 is not None and n2.k == "ID":
                    tf, via = None, None
                    if t.v == own_cname:
                        tf, via = f, "own class_name"
                    elif t.v in cname[proj.root] and t.v not in same_all:
                        tf, via = cname[proj.root][t.v], "class_name"
                    elif t.v in pm:
                        tf, via = pm[t.v], "preload/load const or var"
                    elif (proj.root, t.v) in al:
                        tf, via = al[(proj.root, t.v)], "autoload name"
                    cls = top_inner(tf).get(n2.v) if tf else None
                    if cls is not None:
                        j = i + 2
                        while j + 2 < len(ts) and ts[j + 1].v == "." and ts[j + 2].k == "ID" and nested(cls, ts[j + 2].v):
                            cls = nested(cls, ts[j + 2].v)
                            j += 2
                        ctx = ref_context(ts, i, j)
                        text = ".".join(ts[x].v for x in range(i, j + 1, 2))
                        if tf == f:
                            bump(group, "7 same-file uses", ctx + " [qualified Own.Inner]", f"{name}/{f}:{t.line} {text}")
                            repo_uses[name]["7 same file"] += 1
                        else:
                            bump(group, f"6 other-file uses via {via}", ctx, f"{name}/{f}:{t.line} {text}")
                            bump(group, "6 summary (uses)", f"via {via}", f"{name}/{f}:{t.line} {text}")
                            repo_uses[name]["6 other files"] += 1
                            detail["6 targets"].append((name, tf, cls.name, f, via))
                        if ctx == ".new()" and cls.native and native_is(cls.native, "Node"):
                            detail["8 .new() of a Node-derived inner class"].append(f"{name}/{f}:{t.line} {text}.new()")
                        continue
                if t.v in same_all and (t.v not in cname[proj.root] or cname[proj.root].get(t.v) == f):
                    if pv is not None and pv.v in ("class", "var", "const", "func", "signal", "enum", "for"):
                        continue
                    cls = next(c for c in classes if c.name == t.v)
                    j = i
                    while j + 2 < len(ts) and ts[j + 1].v == "." and ts[j + 2].k == "ID" and nested(cls, ts[j + 2].v):
                        cls = nested(cls, ts[j + 2].v)
                        j += 2
                    ctx = ref_context(ts, i, j)
                    bump(group, "7 same-file uses", ctx, f"{name}/{f}:{t.line} {t.v}")
                    bump(group, "7b same-file uses by location", where, f"{name}/{f}:{t.line} {t.v}")
                    repo_uses[name]["7 same file"] += 1
                    if ctx == ".new()" and cls.native and native_is(cls.native, "Node"):
                        detail["8 .new() of a Node-derived inner class"].append(f"{name}/{f}:{t.line} {t.v}.new()")
            # 8 set_script / .script =
            for i, t in enumerate(ts):
                if t.k == "ID" and t.v in ("set_script", "script"):
                    seg = [ts[x] for x in range(i, min(len(ts), i + 12)) if ts[x].line == t.line]
                    hit = {x.v for x in seg if x.k == "ID"} & same_all
                    for x in range(len(seg) - 2):
                        if seg[x].k == "ID" and seg[x + 1].v == "." and seg[x + 2].k == "ID":
                            tf = cname[proj.root].get(seg[x].v) or pm.get(seg[x].v) or al.get((proj.root, seg[x].v))
                            if tf and seg[x + 2].v in top_inner(tf):
                                hit.add(seg[x].v + "." + seg[x + 2].v)
                    if hit and (t.v == "set_script" or (i + 1 < len(ts) and ts[i + 1].v == "=")):
                        detail["8 set_script / .script = with an inner class"].append(f"{name}/{f}:{t.line} {sorted(hit)}")

    for k, title in (("all", "ALL REPOSITORIES"), ("games", "GAME REPOSITORIES")):
        print(f"\n==================== {title}")
        for sec in sorted(C[k]):
            print(f"-- {sec}")
            for item, n in C[k][sec].most_common():
                print(f"   {n:5d}  {item}")
                for s_ in S[k][(sec, item)]:
                    print(f"            {s_}")
    print("\n==================== PER REPOSITORY: first-party .gd, files with inner classes, inner classes, same-file uses, other-file uses")
    for row in per_repo:
        u = repo_uses[row[0]]
        print(f"   {row[0]:28s} {row[1]:7s} {row[2]:5d} {row[3]:5d} {row[4]:5d} {u['7 same file']:6d} {u['6 other files']:6d}")
    print("\n==================== DETAIL")
    for key in sorted(detail):
        vals = detail[key]
        print(f"-- {key} ({len(vals)})")
        if key == "6 targets":
            cnt = collections.Counter((a, b, c_, v) for a, b, c_, _, v in vals)
            users = collections.defaultdict(set)
            for a, b, c_, u, v in vals:
                users[(a, b, c_, v)].add(u)
            for (a, b, c_, v), n in cnt.most_common(50):
                print(f"   {n:4d} uses from {len(users[(a, b, c_, v)]):3d} files  {a}/{b}::{c_}  (via {v})")
            continue
        for v in vals[:60]:
            print("   ", v)


SELFTEST = '''class_name Outer
extends Node
const K = 1
const P = preload("res://x.gd")
enum E {A, B}
enum {U1, U2}
signal sig
var inst_var = 0
static var sv = 0
func inst_func(): pass
static func sf(): pass

class A:
\tvar own = K + U1
\tfunc f(x):
\t\tvar e = E.A
\t\tsf()
\t\tinst_func()
\t\tprint(inst_var, sv, P, B.new(), Outer.K)
\t\tvar d = {inst_var = 1}
\t\treturn x

class B extends A:
\textends_marker_is_not_extends = 1

class C:
\textends A
\tclass D:
\t\tconst Z = K
'''


def selftest():
    ts, top, classes = parse_file(SELFTEST, "t.gd")
    by = {c.name: c for c in classes}
    assert by["B"].extends == ("name", "A") and by["C"].extends == ("name", "A") and by["C"].extends_in_body
    assert by["D"].depth == 2 and by["D"].parent is by["C"]
    got = {(cat, n) for cat, n, _ in outer_uses(by["A"], ts, "Outer", by["A"].decls | by["A"].vars | by["A"].funcs)}
    want = {("const", "K"), ("unnamed-enum value", "U1"), ("enum (named)", "E"), ("static func", "sf"),
            ("INSTANCE func", "inst_func"), ("INSTANCE var", "inst_var"), ("static var", "sv"),
            ("const (preload/load)", "P"), ("other inner class", "B"), ("outer class_name (any use, e.g. back-reference type)", "Outer")}
    assert got == want, got ^ want
    got = {(cat, n) for cat, n, _ in outer_uses(by["D"], ts, "Outer", by["D"].decls)}
    assert got == {("const", "K")}, got
    _, _, cl = parse_file('func f():\n\tg("\n~ start\nA: {m}.\n")\n\nclass T extends X:\n\tpass\n', "s.gd")
    assert [c.name for c in cl] == ["T"] and cl[0].line == 7, cl
    print("selftest ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        selftest()
    else:
        main()
