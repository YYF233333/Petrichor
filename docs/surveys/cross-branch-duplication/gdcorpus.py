"""Corpus access shared by the scripts in this directory.

Corpus list and first-party rules, a GDScript tokenizer, the script header parser (class_name / extends),
native class hierarchy (from api.json, see build_api.py), and the composition of text scenes (.tscn) needed to
find which node types each script is attached under. Python 3 stdlib only.

Fetch the corpus (shallow, blobless, sparse clones pinned to fixed commits):
    python gdcorpus.py CORPUS_DIR [name ...]
"""
import collections
import json
import os
import re
import subprocess
import sys

# name, url, commit, group
REPOS = [
    ("beehave", "https://github.com/bitbrain/beehave.git", "fe589153fa780a3f88989578ceb5f539afffb7c2", "plugin"),
    ("gdUnit4", "https://github.com/MikeSchulze/gdUnit4.git", "9bc1a450682c9c0d337e1b8977535f0a710385e1", "plugin"),
    ("godot_dialogue_manager", "https://github.com/nathanhoad/godot_dialogue_manager.git", "7a72dcf64cfec8673d398606f2991d4cf81f2d11", "plugin"),
    ("godot-demo-projects", "https://github.com/godotengine/godot-demo-projects.git", "b761b4cd5718a7118f2c95eb55f181d0a5f004f1", "demos"),
    ("Godot-Game-Template", "https://github.com/Maaack/Godot-Game-Template.git", "6849d6c352dafe8ed54b5fa4f4b9774adfca31c8", "plugin"),
    ("godot-open-rpg", "https://github.com/gdquest-demos/godot-open-rpg.git", "d59f2f71bc046ac53239a4e590861269e7100a36", "game"),
    ("godot-statecharts", "https://github.com/derkork/godot-statecharts.git", "76d226a3efac66a72aea825382b320c94808f409", "plugin"),
    ("phantom-camera", "https://github.com/ramokz/phantom-camera.git", "21d3762492e2dc51b448f55438b89b3c19349f96", "plugin"),
    ("Pixelorama", "https://github.com/Orama-Interactive/Pixelorama.git", "38a8b03eafc56a2815c42ff53ef3f61db7cf432b", "tool"),
    ("Starter-Kit-3D-Platformer", "https://github.com/KenneyNL/Starter-Kit-3D-Platformer.git", "3fa8a04b1c01ab23db43123d4ce814a34c3fc7f0", "game"),
    ("Starter-Kit-City-Builder", "https://github.com/KenneyNL/Starter-Kit-City-Builder.git", "4535092b740b378b700efd9df9e27a631815b84a", "game"),
    ("Starter-Kit-FPS", "https://github.com/KenneyNL/Starter-Kit-FPS.git", "185fd2326d74a5cf858cffc616f87cf9696f9cc0", "game"),
    ("Starter-Kit-Racing", "https://github.com/KenneyNL/Starter-Kit-Racing.git", "2f2e5f2646dda89cb21d4e8539bab60c6e955dc8", "game"),
    ("tps-demo", "https://github.com/godotengine/tps-demo.git", "a82f15448e9b015440d3bbdf5e10801b260c4e9f", "game"),
    ("godot-open-rts", "https://github.com/lampe-games/godot-open-rts.git", "a628ad3bb6a9a903587a181a7ee3300dbf735e81", "game"),
    ("tanks-of-freedom-ii", "https://github.com/P1X-in/tanks-of-freedom-ii.git", "dbe629ea05691599dde4fd65ab9a13b78a4ccfe1", "game"),
    ("deck_builder_tutorial", "https://github.com/guladam/deck_builder_tutorial.git", "91b408bc660e9b7389892dd4242a4d76e5bc9e77", "game"),
    ("godot-tiny-mmo", "https://github.com/SlayHorizon/godot-tiny-mmo.git", "f4ed04f20963c3285117e58b1149fa1117658d3c", "game"),
    ("godot_open_star_fighter", "https://github.com/teeeece/godot_open_star_fighter.git", "6e7be5c1371b91fb9dbd7f3c8c67f8cfc967580c", "game"),
    ("Ring", "https://github.com/YYF233333/Ring.git", "0ec41610f25d83917e6410451ad6d796a948c766", "game"),
]
SPARSE = ["*.gd", "*.tscn", "*.tres", "*.scn", "*.uid", "*.cs", "project.godot", "plugin.cfg", "/LICENSE*", "/COPYING*", "/license*"]

ALL = "*"
# addons/<name>/ directories that are first-party for the repo (everything else under addons/ is vendored).
FIRST_PARTY_ADDONS = {
    "beehave": {"beehave"},
    "gdUnit4": {"gdUnit4"},
    "godot_dialogue_manager": {"dialogue_manager"},
    "godot-demo-projects": ALL,
    "Godot-Game-Template": ALL,  # all addons are Maaack's own (plugin.cfg author)
    "godot-statecharts": {"godot_state_charts"},
    "phantom-camera": {"phantom_camera"},
    "godot-tiny-mmo": {"tinymmo"},
}

NATIVE_PARENT = {}


def load_api(path):
    """Load api.json (build_api.py); fills NATIVE_PARENT and returns the per-class member lists."""
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    classes = data["classes"]
    NATIVE_PARENT.clear()
    NATIVE_PARENT.update({k: v["inherits"] for k, v in classes.items()})
    return classes


def native_is(cls, base):
    seen = 0
    while cls and seen < 64:
        if cls == base:
            return True
        cls = NATIVE_PARENT.get(cls)
        seen += 1
    return False


# ----------------------------------------------------------------------------------------------
# GDScript tokenizer (comments and strings handled properly)

KEYWORDS = {
    "if", "elif", "else", "for", "while", "break", "continue", "pass", "return", "match", "when",
    "and", "or", "not", "in", "is", "as", "self", "await", "yield", "var", "const", "func", "static",
    "class", "class_name", "extends", "signal", "enum", "breakpoint", "preload", "assert", "void",
    "super", "true", "false", "null", "PI", "TAU", "INF", "NAN", "namespace", "trait",
}
NUM_RE = re.compile(r"0[xX][0-9a-fA-F_]+|0[bB][01_]+|\d[\d_]*(?:\.[\d_]*)?(?:[eE][+-]?\d+)?")


class Tok:
    __slots__ = ("k", "v", "line", "col", "first", "pre")

    def __init__(self, k, v, line, col, first, pre=""):
        self.k, self.v, self.line, self.col, self.first, self.pre = k, v, line, col, first, pre

    def __repr__(self):
        return f"{self.k}:{self.v!r}@{self.line}"


def tokenize(src):
    toks = []
    i, n, line, ls, first = 0, len(src), 1, 0, True
    while i < n:
        c = src[i]
        if c == "\n":
            toks.append(Tok("NL", None, line, i - ls, False))
            line += 1
            i += 1
            ls = i
            first = True
            continue
        if c in " \t\r\f":
            i += 1
            continue
        if c == "\\" and i + 1 < n and src[i + 1] in "\r\n":
            i += 1
            if src[i] == "\r":
                i += 1
            if i < n and src[i] == "\n":
                i += 1
            line += 1
            ls = i
            continue
        if c == "#":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        col = i - ls
        pre = ""
        if c in "r&^" and i + 1 < n and src[i + 1] in "\"'" and not (toks and toks[-1].k == "ID" and toks[-1].line == line and toks[-1].col + len(toks[-1].v) == col):
            pre = c
            i += 1
            c = src[i]
        if c in "\"'":
            q = c
            triple = src.startswith(q * 3, i)
            j = i + (3 if triple else 1)
            buf = []
            sline = line
            while j < n:
                ch = src[j]
                if ch == "\\" and pre != "r" and j + 1 < n:
                    nx = src[j + 1]
                    buf.append({"n": "\n", "t": "\t"}.get(nx, nx))
                    if nx == "\n":
                        line += 1
                    j += 2
                    continue
                if triple and src.startswith(q * 3, j):
                    j += 3
                    break
                if not triple and ch == q:
                    j += 1
                    break
                if ch == "\n":
                    line += 1
                    if not triple:
                        j += 1
                        break
                buf.append(ch)
                j += 1
            toks.append(Tok("STR", "".join(buf), sline, col, first, pre))
            if line != sline:
                ls = src.rfind("\n", 0, j) + 1
            i = j
            first = False
            continue
        if c.isalpha() or c == "_" or ord(c) > 127:
            j = i + 1
            while j < n and (src[j].isalnum() or src[j] == "_" or ord(src[j]) > 127):
                j += 1
            toks.append(Tok("ID", src[i:j], line, col, first))
            i = j
            first = False
            continue
        if c.isdigit():
            m = NUM_RE.match(src, i)
            j = m.end() if m else i + 1
            toks.append(Tok("NUM", src[i:j], line, col, first))
            i = j
            first = False
            continue
        if c == "@":
            j = i + 1
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            toks.append(Tok("AN", src[i:j], line, col, first))
            i = j
            first = False
            continue
        toks.append(Tok("OP", c, line, col, first))
        i += 1
        first = False
    return toks


def parse_header(src):
    """class_name and extends of a script (top-level header lines, optionally after annotations)."""
    ts = [t for t in tokenize(src) if t.k != "NL"]
    info = {"class_name": None, "extends": None, "extends_kind": None}
    N = len(ts)
    for i, t in enumerate(ts):
        if t.k != "ID" or t.v not in ("class_name", "extends"):
            continue
        hdr_pos = t.first and t.col == 0
        if not hdr_pos and i > 0:
            k = i - 1
            while k >= 0 and ts[k].line == t.line and ts[k].k == "AN":
                k -= 1
            hdr_pos = (k < 0 or ts[k].line != t.line) and ts[k + 1].col == 0 and k + 1 < i
        if not hdr_pos:
            continue
        j = i
        while j < N and ts[j].line == t.line:
            if ts[j].k == "ID" and ts[j].v == "class_name" and j + 1 < N and ts[j + 1].k == "ID":
                info["class_name"] = ts[j + 1].v
            if ts[j].k == "ID" and ts[j].v == "extends" and j + 1 < N:
                nx = ts[j + 1]
                if nx.k == "STR":
                    info["extends"], info["extends_kind"] = nx.v, "path"
                elif nx.k == "ID":
                    name = nx.v
                    k = j + 2
                    while k + 1 < N and ts[k].k == "OP" and ts[k].v == "." and ts[k + 1].k == "ID":
                        name += "." + ts[k + 1].v
                        k += 2
                    info["extends"], info["extends_kind"] = name, "name"
            j += 1
    return info


# ----------------------------------------------------------------------------------------------
# Repositories and Godot projects


class Project:
    def __init__(self, repo, root_rel):
        self.repo = repo
        self.root = root_rel  # repo-relative posix dir ('' = repo root)

    def to_rel(self, res, from_rel=None):
        if res.startswith("res://"):
            p = res[6:]
            return (self.root + "/" + p) if self.root else p
        if from_rel is not None and res and not res.startswith("uid://") and "://" not in res:
            base = os.path.dirname(from_rel)
            return os.path.normpath(os.path.join(base, res)).replace("\\", "/")
        return None


def git_head(d):
    """Commit checked out in clone d, read from .git without running git."""
    try:
        head = open(os.path.join(d, ".git", "HEAD")).read().strip()
    except OSError:
        return None
    if not head.startswith("ref:"):
        return head
    ref = head[4:].strip()
    try:
        return open(os.path.join(d, ".git", ref)).read().strip()
    except OSError:
        pass
    try:
        for ln in open(os.path.join(d, ".git", "packed-refs")):
            parts = ln.split()
            if len(parts) == 2 and parts[1] == ref:
                return parts[0]
    except OSError:
        pass
    return None


class Repo:
    def __init__(self, corpus, name, group):
        self.name, self.group = name, group
        self.dir = os.path.join(corpus, name)
        if not os.path.isdir(self.dir):
            sys.exit(f"missing corpus clone: {self.dir}")
        self.files = []
        for dp, dns, fns in os.walk(self.dir):
            dns[:] = sorted(d for d in dns if d != ".git" and d != ".godot")
            for f in sorted(fns):
                self.files.append(os.path.relpath(os.path.join(dp, f), self.dir).replace("\\", "/"))
        self.fileset = set(self.files)
        roots = sorted({os.path.dirname(f) for f in self.files if os.path.basename(f) == "project.godot"}, key=len, reverse=True)
        if "" not in roots:
            roots.append("")
        self.roots = roots
        self.projects = {r: Project(self, r) for r in roots}
        self.uid = {}
        for f in self.files:
            if f.endswith(".uid"):
                try:
                    u = open(os.path.join(self.dir, f), encoding="utf-8", errors="replace").read().strip()
                    self.uid[u] = f[:-4]
                except OSError:
                    pass
        self.commit = git_head(self.dir)

    def project_of(self, rel):
        for r in self.roots:
            if r == "" or rel == r or rel.startswith(r + "/"):
                return self.projects[r]
        return self.projects[""]

    def first_party(self, rel):
        proj = self.project_of(rel)
        sub = rel[len(proj.root) + 1:] if proj.root else rel
        parts = sub.split("/")
        if "addons" in parts:
            k = parts.index("addons")
            if k + 1 < len(parts) - 1:
                name = parts[k + 1]
                fp = FIRST_PARTY_ADDONS.get(self.name, set())
                return fp == ALL or name in fp
        return True

    def read(self, rel):
        p = os.path.abspath(os.path.join(self.dir, rel))
        if os.name == "nt" and not p.startswith("\\\\?\\"):
            p = "\\\\?\\" + p  # long paths
        with open(p, encoding="utf-8", errors="replace") as fh:
            return fh.read()


# ----------------------------------------------------------------------------------------------
# Godot text resources (.tscn) and composed scene trees

HDR_RE = re.compile(r"^\[(\w+)(.*)\]\s*$")
ATTR_RE = re.compile(r'(\w+)=("(?:[^"\\]|\\.)*"|\w+\([^)]*\)|\[[^\]]*\]|[^\s\]]+)')
EXT_REF_RE = re.compile(r'ExtResource\(\s*"?([^")\s]+)"?\s*\)')
SUB_REF_RE = re.compile(r'SubResource\(\s*"?([^")\s]+)"?\s*\)')


def unq(v):
    if v and len(v) >= 2 and v[0] == '"' and v[-1] == '"':
        return v[1:-1].replace('\\"', '"').replace("\\\\", "\\")
    return v


class Section:
    __slots__ = ("tag", "attrs", "props", "line")

    def __init__(self, tag, attrs, line):
        self.tag, self.attrs, self.props, self.line = tag, attrs, {}, line


def parse_sections(text):
    secs = []
    cur = None
    i, n, line = 0, len(text), 1
    while i < n:
        j = text.find("\n", i)
        if j < 0:
            j = n
        raw = text[i:j]
        s = raw.strip()
        if not s or s.startswith(";"):
            i, line = j + 1, line + 1
            continue
        if s[0] == "[":
            m = HDR_RE.match(s)
            if m:
                attrs = {k: v for k, v in ATTR_RE.findall(m.group(2))}
                cur = Section(m.group(1), attrs, line)
                secs.append(cur)
                i, line = j + 1, line + 1
                continue
        eq = raw.find("=")
        if eq < 0:
            i, line = j + 1, line + 1
            continue
        key = raw[:eq].strip().strip('"')
        k = i + eq + 1
        depth, instr = 0, False
        while k < n:
            ch = text[k]
            if instr:
                if ch == "\\":
                    k += 2
                    continue
                if ch == '"':
                    instr = False
                elif ch == "\n":
                    line += 1
            else:
                if ch == '"':
                    instr = True
                elif ch in "([{":
                    depth += 1
                elif ch in ")]}":
                    depth -= 1
                elif ch == "\n":
                    if depth <= 0:
                        break
                    line += 1
            k += 1
        if cur is not None:
            cur.props[key] = text[i + eq + 1:k].strip()
        i, line = k + 1, line + 1
    return secs


class SceneFile:
    def __init__(self, rel, text):
        self.rel = rel
        self.secs = parse_sections(text)
        self.ext = {}
        self.nodes = []
        for s in self.secs:
            if s.tag == "ext_resource":
                self.ext[unq(s.attrs.get("id", ""))] = (unq(s.attrs.get("type", "")), unq(s.attrs.get("path", "")), unq(s.attrs.get("uid", "")))
            elif s.tag == "node":
                self.nodes.append(s)


class N:
    __slots__ = ("name", "type", "script", "unique", "children", "owner", "parent", "inst", "unknown", "assign", "assign_kind")

    def __init__(self, name, typ, unknown=False):
        self.name, self.type, self.unknown = name, typ, unknown
        self.script = None
        self.unique = False
        self.children = {}
        self.owner = None
        self.parent = None
        self.inst = None
        self.assign = None
        self.assign_kind = None


def clone(root):
    mp = {}
    order = []

    def rec(o):
        c = N(o.name, o.type, o.unknown)
        c.script, c.unique, c.inst, c.assign, c.assign_kind = o.script, o.unique, o.inst, o.assign, o.assign_kind
        mp[id(o)] = c
        order.append((o, c))
        for k, v in o.children.items():
            cc = rec(v)
            cc.parent = c
            c.children[k] = cc
        return c

    nr = rec(root)
    for o, c in order:
        c.owner = mp.get(id(o.owner)) if o.owner is not None else None
    return nr


def walk(n, prefix=""):
    yield prefix, n
    for k, c in n.children.items():
        yield from walk(c, (prefix + "/" if prefix else "") + k)


def script_infos(R):
    """Every .gd in the repo: header, first-party flag, base script and native ancestor."""
    scripts = {}
    for f in R.files:
        if f.endswith(".gd"):
            try:
                src = R.read(f)
            except OSError:
                continue
            info = parse_header(src)
            info["first_party"] = R.first_party(f)
            scripts[f] = info
    cname = collections.defaultdict(dict)
    for f, s in scripts.items():
        if s["class_name"]:
            cname[R.project_of(f).root][s["class_name"]] = f
    for f, s in scripts.items():
        proj = R.project_of(f)
        base = None
        if s["extends_kind"] == "path":
            base = proj.to_rel(s["extends"], f)
            if base and base.startswith("uid://"):
                base = R.uid.get(base)
        elif s["extends_kind"] == "name":
            nm = s["extends"]
            if nm in cname[proj.root]:
                base = cname[proj.root][nm]
        s["base_script"] = base if base in scripts else None
        s["native_direct"] = s["extends"] if s["extends_kind"] == "name" and s["extends"] in NATIVE_PARENT else None
    for f, s in scripts.items():
        cur, seen = f, set()
        nat = None
        while cur and cur not in seen:
            seen.add(cur)
            sc = scripts[cur]
            if sc["native_direct"]:
                nat = sc["native_direct"]
                break
            if sc["base_script"] is None:
                nat = "RefCounted" if sc["extends"] is None else ("?" + str(sc["extends"]))
                break
            cur = sc["base_script"]
        s["native"] = nat
        s["is_node"] = bool(nat) and native_is(nat, "Node")
    return scripts


def scene_contexts(R):
    """Compose every .tscn (inherited scenes, instances and overrides included) and return
    {script: [{"scene", "path", "node", "how"}]} for each node a .gd script ends up attached to.
    how = text_node / text_override_inst / text_override_inh (attached in this scene file),
          instanced (comes from an instanced sub-scene), inherited (comes from the base scene)."""
    scenes = {}
    for f in R.files:
        if f.endswith(".tscn"):
            try:
                sf = SceneFile(f, R.read(f))
            except Exception:  # noqa
                continue
            proj = R.project_of(f)

            def ext_rel(eid, sf=sf, proj=proj):
                e = sf.ext.get(eid)
                if not e:
                    return None, None
                typ, path, uid = e
                rel = proj.to_rel(path) if path else None
                if (rel is None or rel not in R.fileset) and uid and uid in R.uid:
                    rel = R.uid[uid]
                return typ, rel

            sf.ext_rel = ext_rel
            scenes[f] = sf

    cache = {}
    building = set()

    def instantiate(rel):
        if rel is None:
            return N("?", "?missing", unknown=True)
        if rel.endswith(".tscn") and rel in scenes:
            t = build(rel)
            if t is None:
                return N("?", "?cycle", unknown=True)
            return clone(t)
        ext = os.path.splitext(rel)[1].lower()
        if ext == ".tscn":
            return N("?", "?missing", unknown=True)
        return N("?", "?" + ext.lstrip("."), unknown=True)

    def lookup(root, ppath):
        if ppath in (".", ""):
            return root
        cur = root
        for seg in ppath.split("/"):
            if seg in ("", "."):
                continue
            if cur.unknown:
                return cur
            nx = cur.children.get(seg)
            if nx is None:
                return None
            cur = nx
        return cur

    def enclosing_instance(n, root):
        x = n
        while x is not None and x is not root:
            if x.inst:
                return x
            x = x.parent
        return None

    def set_script_props(node, sec, sf, kind):
        sc = sec.props.get("script")
        if sc is not None:
            m = EXT_REF_RE.search(sc)
            m2 = SUB_REF_RE.search(sc)
            if m:
                typ, rel = sf.ext_rel(m.group(1))
                node.script = rel if rel else "?unresolved"
            elif m2:
                node.script = "builtin:" + sf.rel + "#" + m2.group(1)
            elif sc.strip() == "null":
                node.script = None
            node.assign, node.assign_kind = sf.rel, kind
        u = sec.props.get("unique_name_in_owner")
        if u is not None:
            node.unique = u.strip() == "true"

    def build(rel):
        if rel in cache:
            return cache[rel]
        if rel in building:
            return None
        building.add(rel)
        sf = scenes[rel]
        if not sf.nodes:
            root = N("?", "?empty", unknown=True)
        else:
            re0 = sf.nodes[0]
            name = unq(re0.attrs.get("name", "?"))
            if "instance" in re0.attrs and "parent" not in re0.attrs:
                m = EXT_REF_RE.search(re0.attrs["instance"])
                _, base = sf.ext_rel(m.group(1)) if m else (None, None)
                root = instantiate(base)
                root.name = name
                root.owner = None
                root.parent = None
                root.inst = None
                set_script_props(root, re0, sf, "override_inh")
            else:
                root = N(name, unq(re0.attrs.get("type", "?untyped")))
                set_script_props(root, re0, sf, "node")
            for sec in sf.nodes[1:]:
                parent = lookup(root, unq(sec.attrs.get("parent", ".")))
                nm = unq(sec.attrs.get("name", "?"))
                if parent is None or parent.unknown:
                    continue
                inst_anc = enclosing_instance(parent, root)
                if "instance" in sec.attrs:
                    m = EXT_REF_RE.search(sec.attrs["instance"])
                    _, sub = sf.ext_rel(m.group(1)) if m else (None, None)
                    child = instantiate(sub)
                    child.name = nm
                    child.inst = sub or "?"
                    kind = "override_inst"
                elif "instance_placeholder" in sec.attrs:
                    child = N(nm, "InstancePlaceholder", unknown=True)
                    kind = "node"
                elif "type" in sec.attrs:
                    child = N(nm, unq(sec.attrs["type"]))
                    kind = "node"
                else:
                    ex = parent.children.get(nm)
                    if ex is None:
                        continue
                    set_script_props(ex, sec, sf, "override_inst" if (inst_anc is not None or ex.inst) else "override_inh")
                    continue
                child.owner = root
                child.parent = parent
                parent.children[nm] = child
                set_script_props(child, sec, sf, kind)
        building.discard(rel)
        cache[rel] = root
        return root

    for rel in scenes:
        build(rel)

    contexts = collections.defaultdict(list)
    for rel in scenes:
        root = cache[rel]
        for p, n in walk(root):
            if not n.script or n.script.startswith("builtin:") or not n.script.endswith(".gd"):
                continue
            if n.assign == rel:
                how = "text_" + (n.assign_kind or "node")
            elif enclosing_instance(n, root) is not None:
                how = "instanced"
            else:
                how = "inherited"
            contexts[n.script].append({"scene": rel, "path": p or ".", "node": n, "how": how})
    return contexts


# ----------------------------------------------------------------------------------------------
# Fetching


def fetch(corpus, name, url, sha):
    dest = os.path.join(corpus, name)
    if git_head(dest) == sha:
        print(f"{name}: already at {sha}")
        return
    os.makedirs(dest, exist_ok=True)

    def git(*args, check=True):
        print("+ git", " ".join(args), flush=True)
        subprocess.run(["git", *args], cwd=dest, check=check)

    git("init", "-q")
    subprocess.run(["git", "remote", "remove", "origin"], cwd=dest, capture_output=True)
    git("remote", "add", "origin", url)
    git("config", "core.longpaths", "true")
    git("sparse-checkout", "set", "--no-cone", *SPARSE)
    git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", sha)
    git("checkout", "-q", "--detach", sha)
    assert git_head(dest) == sha, name


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    corpus_dir, want = sys.argv[1], set(sys.argv[2:])
    os.makedirs(corpus_dir, exist_ok=True)
    for name, url, sha, _ in REPOS:
        if not want or name in want:
            fetch(corpus_dir, name, url, sha)
