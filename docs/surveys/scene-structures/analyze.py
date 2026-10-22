"""Scene-structure survey: how many structures does each GDScript see, and do its literal node paths
resolve to the same type in all of them?

Usage: python analyze.py CORPUS OUT [name ...]      (after fetch_corpus.py)
Reads CORPUS/<name> for every repository in fetch_corpus.REPOS (or only the names given) and the
native class hierarchy in classes.json next to this script.
Writes OUT/repos.json, OUT/scripts.csv, OUT/uses.csv and OUT/contexts.csv.
Python 3 stdlib only.
"""
import collections
import csv
import hashlib
import json
import os
import re
import sys

from fetch_corpus import REPOS

HERE = os.path.dirname(os.path.abspath(__file__))
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

CLASSES = json.load(open(os.path.join(HERE, "classes.json"), encoding="utf-8"))
NATIVE_PARENT = CLASSES["parents"]


def native_is(cls, base):
    seen = 0
    while cls and seen < 64:
        if cls == base:
            return True
        cls = NATIVE_PARENT.get(cls)
        seen += 1
    return False


# ----------------------------------------------------------------------------------------------
# GDScript tokenizer (enough for node-path extraction; comments and strings handled properly)

KEYWORDS = {
    "if", "elif", "else", "for", "while", "break", "continue", "pass", "return", "match", "when",
    "and", "or", "not", "in", "is", "as", "self", "await", "yield", "var", "const", "func", "static",
    "class", "class_name", "extends", "signal", "enum", "breakpoint", "preload", "assert", "void",
    "super", "true", "false", "null", "PI", "TAU", "INF", "NAN", "namespace", "trait",
}
OPERAND_KW = {"self", "true", "false", "null", "PI", "TAU", "INF", "NAN", "super"}
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


def is_operand_end(t):
    if t is None:
        return False
    if t.k in ("NUM", "STR"):
        return True
    if t.k == "ID":
        return t.v not in KEYWORDS or t.v in OPERAND_KW
    return t.k == "OP" and t.v in ")]}"


def is_name(t):
    return t is not None and (t.k == "ID" or t.k == "STR")


def parse_gd(src):
    toks = tokenize(src)
    ts = [t for t in toks if t.k != "NL"]
    info = {
        "class_name": None, "extends": None, "extends_kind": None, "tool": False,
        "uses": [],  # literal self paths: dict(kind, path, line)
        "nonlit": [],  # get_node(non-literal) lines
        "other_recv_lit": [], "has_node_lit": [], "find_child": [], "owner_dot": [], "owner_any": [],
        "get_parent": [], "get_children": [], "add_child": [], "exports": [], "res_refs": [], "idents": set(), "set_script": 0,
    }
    N = len(ts)
    for i, t in enumerate(ts):
        if t.k == "ID":
            info["idents"].add(t.v)
        if t.k == "AN" and t.v == "@tool":
            info["tool"] = True
        # header: class_name / extends at column 0 (optionally after annotations on the same line)
        hdr_pos = (t.first and t.col == 0)
        if not hdr_pos and t.k == "ID" and i > 0:
            k = i - 1
            while k >= 0 and ts[k].line == t.line and ts[k].k == "AN":
                k -= 1
            hdr_pos = (k < 0 or ts[k].line != t.line) and ts[k + 1].col == 0 and k + 1 < i
        if t.k == "ID" and hdr_pos and t.v in ("class_name", "extends"):
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
        if t.k == "STR" and re.search(r"\.(gd|tscn|scn|tres)$", t.v):
            info["res_refs"].append(t.v)
        if t.k == "ID" and t.v == "set_script" and i + 1 < N and ts[i + 1].v == "(":
            info["set_script"] += 1
        prev = ts[i - 1] if i > 0 else None
        # $path
        if t.k == "OP" and t.v == "$":
            j = i + 1
            path = ""
            if j < N and ts[j].k == "OP" and ts[j].v == "/":
                path += "/"
                j += 1
            while j < N:
                if ts[j].k == "OP" and ts[j].v == "%":
                    path += "%"
                    j += 1
                if j < N and is_name(ts[j]):
                    path += ts[j].v
                    j += 1
                else:
                    break
                if j + 1 < N and ts[j].k == "OP" and ts[j].v == "/" and (is_name(ts[j + 1]) or ts[j + 1].v == "%"):
                    path += "/"
                    j += 1
                    continue
                break
            if path:
                info["uses"].append({"kind": "dollar", "path": path, "line": t.line})
            continue
        # %Unique (prefix position only)
        prev_same_line = prev if (prev is not None and prev.line == t.line) else None  # statement start => prefix %
        if t.k == "OP" and t.v == "%" and not is_operand_end(prev_same_line) and not (prev and prev.k == "OP" and prev.v == "$"):
            if prev is not None and prev.k == "OP" and prev.v == "/" and i >= 2 and is_name(ts[i - 2]):
                continue  # part of a $a/%b path, already consumed
            j = i + 1
            if j < N and is_name(ts[j]):
                path = "%" + ts[j].v
                j += 1
                while j + 1 < N and ts[j].k == "OP" and ts[j].v == "/" and is_name(ts[j + 1]):
                    path += "/" + ts[j + 1].v
                    j += 2
                info["uses"].append({"kind": "unique", "path": path, "line": t.line})
            continue
        if t.k == "ID" and i + 1 < N and ts[i + 1].k == "OP" and ts[i + 1].v == "(":
            if t.v in ("get_node", "get_node_or_null", "has_node"):
                recv_self = True
                if prev is not None and prev.k == "OP" and prev.v == ".":
                    recv_self = i >= 2 and ts[i - 2].k == "ID" and ts[i - 2].v == "self"
                # literal argument?
                j = i + 2
                lit = None
                if j < N and ts[j].k == "STR" and j + 1 < N and ts[j + 1].v in (")", ","):
                    lit = ts[j].v
                elif (j + 3 < N and ts[j].k == "ID" and ts[j].v == "NodePath" and ts[j + 1].v == "("
                      and ts[j + 2].k == "STR" and ts[j + 3].v == ")" and j + 4 < N and ts[j + 4].v in (")", ",")):
                    lit = ts[j + 2].v
                if t.v == "has_node":
                    if lit is not None and recv_self:
                        info["has_node_lit"].append({"kind": "has_node", "path": lit, "line": t.line})
                    continue
                if lit is None:
                    info["nonlit"].append(t.line)
                elif recv_self:
                    info["uses"].append({"kind": t.v, "path": lit, "line": t.line})
                else:
                    info["other_recv_lit"].append({"kind": t.v, "path": lit, "line": t.line})
                continue
            if t.v in ("find_child", "find_children"):
                info["find_child"].append(t.line)
            if t.v == "get_parent":
                info["get_parent"].append(t.line)
            if t.v in ("get_children", "get_child", "get_child_count"):
                info["get_children"].append(t.line)
            if t.v == "add_child":
                info["add_child"].append(t.line)
            if t.v == "get_owner":
                info["owner_any"].append(t.line)
                if i + 3 < N and ts[i + 2].v == ")" and ts[i + 3].v == ".":
                    info["owner_dot"].append(t.line)
        if t.k == "ID" and t.v == "owner":
            other_recv = prev is not None and prev.k == "OP" and prev.v == "." and not (i >= 2 and ts[i - 2].v == "self")
            decl = prev is not None and prev.k == "ID" and prev.v in ("var", "const", "func")
            nxt = ts[i + 1] if i + 1 < N else None
            assign = nxt is not None and nxt.k == "OP" and nxt.v == "=" and not (i + 2 < N and ts[i + 2].v == "=")
            if not other_recv and not decl and not assign:
                info["owner_any"].append(t.line)
                if nxt is not None and nxt.k == "OP" and nxt.v == ".":
                    info["owner_dot"].append(t.line)
        # @export var x: Type
        if t.k == "AN" and t.v.startswith("@export") and t.v not in ("@export_group", "@export_subgroup", "@export_category"):
            annot = t.v
            j = i + 1
            while j < N:
                if ts[j].k == "OP" and ts[j].v == "(":
                    depth = 0
                    while j < N:
                        if ts[j].v == "(" and ts[j].k == "OP":
                            depth += 1
                        elif ts[j].v == ")" and ts[j].k == "OP":
                            depth -= 1
                            if depth == 0:
                                j += 1
                                break
                        j += 1
                    continue
                if ts[j].k == "AN":
                    j += 1
                    continue
                break
            if j + 1 < N and ts[j].k == "ID" and ts[j].v == "static":
                j += 1
            if j + 1 < N and ts[j].k == "ID" and ts[j].v == "var":
                typ, elem = None, None
                if j + 3 < N and ts[j + 2].v == ":" and ts[j + 3].k == "ID":
                    typ = ts[j + 3].v
                    k = j + 4
                    while k + 1 < N and ts[k].v == "." and ts[k + 1].k == "ID":
                        typ = ts[k + 1].v
                        k += 2
                    if k + 1 < N and ts[k].v == "[" and ts[k + 1].k == "ID":
                        elem = ts[k + 1].v
                info["exports"].append({"annot": annot, "type": typ, "elem": elem, "line": ts[j].line})
    info["idents"] = info["idents"]
    return info


# ----------------------------------------------------------------------------------------------
# Godot text resource parser (.tscn / .tres / project.godot)

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
        self.header = self.secs[0] if self.secs else None
        self.fmt = unq(self.header.attrs.get("format", "")) if self.header else ""
        self.uid = unq(self.header.attrs.get("uid", "")) if self.header else ""
        self.ext = {}
        self.sub = {}
        self.nodes = []
        self.editable = []
        self.resource = None
        for s in self.secs:
            if s.tag == "ext_resource":
                self.ext[unq(s.attrs.get("id", ""))] = (unq(s.attrs.get("type", "")), unq(s.attrs.get("path", "")), unq(s.attrs.get("uid", "")))
            elif s.tag == "sub_resource":
                self.sub[unq(s.attrs.get("id", ""))] = s
            elif s.tag == "node":
                self.nodes.append(s)
            elif s.tag == "editable":
                self.editable.append(unq(s.attrs.get("path", "")))
            elif s.tag == "resource":
                self.resource = s


# ----------------------------------------------------------------------------------------------
# Composed scene trees


class N:
    __slots__ = ("name", "type", "script", "unique", "children", "owner", "parent", "inst", "unknown", "assign", "assign_kind", "entry_line")

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
        self.entry_line = None


def clone(root):
    mp = {}
    order = []

    def rec(o):
        c = N(o.name, o.type, o.unknown)
        c.script, c.unique, c.inst, c.assign, c.assign_kind, c.entry_line = o.script, o.unique, o.inst, o.assign, o.assign_kind, o.entry_line
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


def path_of(n):
    parts = []
    while n.parent is not None:
        parts.append(n.name)
        n = n.parent
    return "/".join(reversed(parts)) or "."


class Project:
    def __init__(self, repo, root_rel, files):
        self.repo = repo
        self.root = root_rel  # repo-relative posix dir ('' = repo root)
        self.files = files

    def to_rel(self, res, from_rel=None):
        if res.startswith("res://"):
            p = res[6:]
            return (self.root + "/" + p) if self.root else p
        if from_rel is not None and res and not res.startswith("uid://") and "://" not in res:
            base = os.path.dirname(from_rel)
            return os.path.normpath(os.path.join(base, res)).replace("\\", "/")
        return None


class Repo:
    def __init__(self, name, group, corpus):
        self.name, self.group = name, group
        self.dir = os.path.join(corpus, name)
        self.files = []
        for dp, dns, fns in os.walk(self.dir):
            dns[:] = sorted(d for d in dns if d != ".git" and d != ".godot")  # sorted: same order on every OS
            for f in sorted(fns):
                self.files.append(os.path.relpath(os.path.join(dp, f), self.dir).replace("\\", "/"))
        self.fileset = set(self.files)
        roots = sorted({os.path.dirname(f) for f in self.files if os.path.basename(f) == "project.godot"}, key=len, reverse=True)
        if "" not in roots:
            roots.append("")
        self.roots = roots
        self.projects = {r: Project(self, r, []) for r in roots}
        self.uid = {}
        for f in self.files:
            if f.endswith(".uid"):
                try:
                    u = open(os.path.join(self.dir, f), encoding="utf-8", errors="replace").read().strip()
                    self.uid[u] = f[:-4]
                except OSError:
                    pass
        self.commit = None
        try:
            self.commit = open(os.path.join(self.dir, ".git", "HEAD")).read().strip()
        except OSError:
            pass

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


def analyze_repo(repo_name, group, corpus):
    R = Repo(repo_name, group, corpus)
    stats = collections.Counter()
    # ---- parse scripts
    scripts = {}
    for f in R.files:
        if f.endswith(".gd"):
            try:
                src = R.read(f)
            except OSError:
                continue
            info = parse_gd(src)
            info["lines"] = src.count("\n") + 1
            info["first_party"] = R.first_party(f)
            scripts[f] = info
    # class_name map per project
    cname = collections.defaultdict(dict)
    for f, s in scripts.items():
        if s["class_name"]:
            cname[R.project_of(f).root][s["class_name"]] = f
    # resolve base scripts and native ancestors
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
        s["is_resource"] = bool(nat) and native_is(nat, "Resource")
        s["is_editor"] = bool(nat) and nat.startswith("Editor")
    subclasses = collections.defaultdict(set)
    for f, s in scripts.items():
        if s["base_script"]:
            subclasses[s["base_script"]].add(f)

    def all_subs(f):
        out, st = set(), [f]
        while st:
            x = st.pop()
            for y in subclasses.get(x, ()):
                if y not in out:
                    out.add(y)
                    st.append(y)
        return out

    # node-ness of export types
    def type_is_node(proj_root, tname):
        if not tname:
            return False
        if tname in NATIVE_PARENT:
            return native_is(tname, "Node")
        f = cname[proj_root].get(tname)
        return bool(f) and scripts[f]["is_node"]

    # ---- autoloads
    autoload = {}
    for r, proj in R.projects.items():
        pg = (r + "/project.godot") if r else "project.godot"
        if pg in R.fileset:
            secs = parse_sections(R.read(pg))
            for s in secs:
                if s.tag == "autoload":
                    for k, v in s.props.items():
                        p = unq(v).lstrip("*")
                        rel = proj.to_rel(p)
                        if p.startswith("uid://"):
                            rel = R.uid.get(p)
                        if rel:
                            autoload[rel] = k

    # ---- parse scenes and resources
    scenes = {}
    tres_attach = collections.defaultdict(set)  # script -> set of .tres files
    subres_attach = collections.defaultdict(set)  # script -> scene files embedding a scripted sub_resource
    builtin_scripts = 0
    builtin_with_paths = 0
    fmt_counter = collections.Counter()
    for f in R.files:
        if f.endswith(".tscn") or f.endswith(".tres"):
            try:
                sf = SceneFile(f, R.read(f))
            except Exception as e:  # noqa
                stats["parse_error"] += 1
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
            if f.endswith(".tscn"):
                scenes[f] = sf
                fmt_counter[sf.fmt] += 1
            # resource-script attachments (sub_resources and [resource])
            for sid, s in sf.sub.items():
                typ = unq(s.attrs.get("type", ""))
                if typ == "GDScript":
                    if f.endswith(".tscn"):
                        builtin_scripts += 1
                        srcv = s.props.get("script/source", "")
                        if re.search(r'\$|get_node\(|%[A-Za-z_]', srcv):
                            builtin_with_paths += 1
                    continue
                sc = s.props.get("script")
                if sc:
                    m = EXT_REF_RE.search(sc)
                    if m:
                        _, rel = ext_rel(m.group(1))
                        if rel:
                            (tres_attach if f.endswith(".tres") else subres_attach)[rel].add(f)
            if sf.resource is not None:
                sc = sf.resource.props.get("script")
                if sc:
                    m = EXT_REF_RE.search(sc)
                    if m:
                        _, rel = ext_rel(m.group(1))
                        if rel:
                            tres_attach[rel].add(f)
    for f in R.files:
        if f.endswith(".scn"):
            stats["binary_scn"] += 1

    # ---- compose
    cache = {}
    building = set()
    scene_meta = {}  # scene -> dict(inherited_from, added_in_instances, overrides_in_instances, editable, dangling)

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
        meta = {"inherited_from": None, "added_in_inst": 0, "override_in_inst": 0, "script_override_inst": 0,
                "editable": len(sf.editable), "dangling": 0, "added_in_base": 0, "override_in_base": 0}
        if not sf.nodes:
            root = N("?", "?empty", unknown=True)
        else:
            re0 = sf.nodes[0]
            name = unq(re0.attrs.get("name", "?"))
            if "instance" in re0.attrs and "parent" not in re0.attrs:
                m = EXT_REF_RE.search(re0.attrs["instance"])
                _, base = sf.ext_rel(m.group(1)) if m else (None, None)
                meta["inherited_from"] = base
                root = instantiate(base)
                root.name = name
                root.owner = None
                root.parent = None
                root.inst = None
                set_script_props(root, re0, sf, "override_inh")
            else:
                root = N(name, unq(re0.attrs.get("type", "?untyped")))
                set_script_props(root, re0, sf, "node")
            root.entry_line = re0.line
            for sec in sf.nodes[1:]:
                parent = lookup(root, unq(sec.attrs.get("parent", ".")))
                nm = unq(sec.attrs.get("name", "?"))
                if parent is None:
                    meta["dangling"] += 1
                    continue
                if parent.unknown:
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
                        meta["dangling"] += 1
                        continue
                    set_script_props(ex, sec, sf, "override_inst" if (inst_anc is not None or ex.inst) else "override_inh")
                    ex.entry_line = sec.line
                    if inst_anc is not None or ex.inst:
                        meta["override_in_inst"] += 1
                        if "script" in sec.props and ex.inst and not inst_anc:
                            meta["script_override_inst"] += 1
                    else:
                        meta["override_in_base"] += 1
                    continue
                child.owner = root
                child.parent = parent
                child.entry_line = sec.line
                parent.children[nm] = child
                set_script_props(child, sec, sf, kind)
                if inst_anc is not None:
                    meta["added_in_inst"] += 1
                elif meta["inherited_from"]:
                    meta["added_in_base"] += 1
                if child.inst and "script" in sec.props:
                    meta["script_override_inst"] += 1
        building.discard(rel)
        cache[rel] = root
        scene_meta[rel] = meta
        return root

    for rel in scenes:
        build(rel)

    # ---- unique-name helpers
    owned_cache = {}

    def owned(o):
        k = id(o)
        if k in owned_cache:
            return owned_cache[k]
        d = {}
        st = list(o.children.values())
        while st:
            x = st.pop()
            if x.owner is o and x.unique:
                d[x.name] = x
            st.extend(x.children.values())
        owned_cache[k] = d
        return d

    def visible_uniques(n):
        d = dict(owned(n.owner)) if n.owner is not None else {}
        d.update(owned(n))
        return d

    def tkey(n):
        return (n.type, n.script)

    def signature(n):
        items = []
        st = [(n, "")]
        while st:
            x, pre = st.pop()
            for k, c in x.children.items():
                p = pre + k
                items.append((p, c.type, c.script or "", c.unknown))
                st.append((c, p + "/"))
        # unique names of the scene the node belongs to (an instance root or composed root owns its own scene;
        # other nodes see their owner's). The runtime fallback from an instance root to the outer owner is
        # deliberately left out of the structure signature (it is still used by resolve()).
        prim = owned(n) if (n.inst or n.parent is None) else (owned(n.owner) if n.owner is not None else {})
        u = tuple(sorted((k, v.type, v.script or "") for k, v in prim.items()))
        # Stable digest rather than hash(): str hashes are randomized per process (PYTHONHASHSEED).
        return int(hashlib.sha1(repr((tuple(sorted(items)), u)).encode("utf-8")).hexdigest()[:16], 16)

    def resolve(n, p):
        p = p.split(":")[0]
        if p.startswith("/"):
            return ("ABS",)
        cur = n
        xs = ""
        for seg in p.split("/"):
            if seg in ("", "."):
                continue
            if seg == "..":
                if cur.parent is None:
                    return ("OUTSIDE",)
                cur = cur.parent
                continue
            if cur.unknown:
                return ("UNKNOWN",)
            if seg.startswith("%"):
                nm = seg[1:]
                u = owned(cur).get(nm)
                if u is None and cur.owner is not None:
                    u = owned(cur.owner).get(nm)
                    if u is not None and (cur.inst or cur is n and n.inst):
                        xs = "xscene"  # found in the *outer* scene via the instance root's owner
                if u is None:
                    return ("MISSING",)
                cur = u
                continue
            c = cur.children.get(seg)
            if c is None:
                return ("MISSING",)
            cur = c
        return ("NODE", cur.type, cur.script, xs)

    # ---- contexts
    contexts = collections.defaultdict(list)  # script -> list of ctx dicts
    inh_children = collections.defaultdict(set)
    for rel, meta in scene_meta.items():
        if meta["inherited_from"]:
            inh_children[meta["inherited_from"]].add(rel)
    for rel in scenes:
        root = cache[rel]
        for p, n in walk(root):
            if not n.script or n.script.startswith("builtin:") or not n.script.endswith(".gd"):
                if n.script and n.script.startswith("builtin:") and n.assign == rel:
                    stats["builtin_attach"] += 1
                continue
            if n.assign == rel:
                how = "text_" + (n.assign_kind or "node")
            elif enclosing_instance(n, root) is not None:
                how = "instanced"
            else:
                how = "inherited"
            contexts[n.script].append({"scene": rel, "path": p or ".", "node": n, "how": how, "root": n is root, "type": n.type})

    # ---- per-script rows
    rows = []
    uses_rows = []
    ctx_rows = []
    sig_cache = {}

    def sig(c):
        k = id(c["node"])
        if k not in sig_cache:
            sig_cache[k] = signature(c["node"])
        return sig_cache[k]

    for f, s in sorted(scripts.items()):
        if not s["first_party"]:
            continue
        own = contexts.get(f, [])
        text = [c for c in own if c["how"].startswith("text_")]
        text_files = sorted({c["scene"] for c in text})
        subs = sorted(all_subs(f))
        sub_ctx = [c for g in subs for c in contexts.get(g, [])]
        allc = own + sub_ctx
        own_sigs = {sig(c) for c in own}
        all_sigs = {sig(c) for c in allc}
        text_sigs = {sig(c) for c in text}
        inh = [c for c in own if c["how"] == "inherited"]
        ins = [c for c in own if c["how"] == "instanced"]
        mech = []
        if len(text_files) >= 2:
            mech.append("M1_text2+" + ("_diff" if len({sig(c) for c in text}) > 1 else "_same"))
        if len(text_files) == 1 and len(text) >= 2 and len(text_sigs) > 1:
            mech.append("M1b_same_file_multi_node_diff")
        inh_sigs = {sig(c) for c in inh}
        if any(c["scene"] in inh_children for c in text) or inh:
            mech.append("M2_inherited" + ("_diff" if inh_sigs - text_sigs else "_same"))
        ins_diff = [c for c in ins if sig(c) not in text_sigs and sig(c) not in inh_sigs]
        if ins_diff:
            mech.append("M3_instance_modified_diff")
        if any(c["how"] == "text_override_inst" for c in text):
            mech.append("M4_script_on_instance")
        if subs and sub_ctx and (all_sigs - own_sigs):
            mech.append("M5_subclass_diff")
        plain_inst = sum(1 for c in ins if sig(c) in text_sigs)
        if len(all_sigs) >= 2:
            grp = collections.OrderedDict()
            own_ids = {id(c) for c in own}
            for c in allc:
                via = "self" if id(c) in own_ids else c["node"].script
                k = (sig(c), via, c["how"])
                if k not in grp:
                    grp[k] = [0, c]
                grp[k][0] += 1
            for (sg, via, how), (cnt, c) in grp.items():
                ctx_rows.append({"repo": repo_name, "script": f, "sig": format(sg & 0xFFFFFF, "06x"), "via": via, "how": how,
                                 "count": cnt, "scene": c["scene"], "path": c["path"], "type": c["type"],
                                 "children": ",".join(sorted(c["node"].children.keys()))[:200]})
        # zero-scene reason
        reason = ""
        if not text_files:
            if f in autoload:
                reason = "autoload"
            elif s["is_editor"]:
                reason = "editor"
            elif tres_attach.get(f) or subres_attach.get(f):
                reason = "resource_attached"
            elif subs:
                reason = "base_class"
            elif re.search(r"(^|/)tests?/|test_|_test\.gd$", f) or (s["extends"] or "").startswith(("GdUnit", "Gut")):
                reason = "test"
            elif not s["is_node"]:
                reason = "non_node_code"
            else:
                reason = "node_code_only"
        proj = R.project_of(f)
        nexp = sum(1 for e in s["exports"] if (type_is_node(proj.root, e["type"]) or (e["type"] == "Array" and type_is_node(proj.root, e["elem"]))))
        npexp = sum(1 for e in s["exports"] if e["type"] == "NodePath" or e["annot"] == "@export_node_path" or (e["type"] == "Array" and e["elem"] == "NodePath"))
        uses = s["uses"]
        kinds = collections.Counter(u["kind"] for u in uses)
        dotdot = sum(1 for u in uses + s["other_recv_lit"] if u["path"].startswith("..") or "/.." in u["path"])
        rootp = sum(1 for u in uses + s["other_recv_lit"] if u["path"].startswith("/root"))
        # resolution of literal self-uses over all contexts (own + subclasses)
        status_c = collections.Counter()
        for u in uses:
            res = [resolve(c["node"], u["path"]) for c in allc]
            types = {(r[1], r[2]) for r in res if r[0] == "NODE"}
            kinds_r = {r[0] for r in res}
            if not allc:
                st = "no_context"
            elif u["path"].startswith("/"):
                st = "absolute"
            elif len(types) > 1:
                st = "differs_native" if len({t[0] for t in types}) > 1 else "differs_script"
            elif types and "MISSING" in kinds_r:
                st = "missing_in_some"
            elif types:
                st = "consistent"
            elif kinds_r == {"MISSING"}:
                st = "missing_all"
            elif "OUTSIDE" in kinds_r and "MISSING" not in kinds_r:
                st = "outside"
            elif "UNKNOWN" in kinds_r:
                st = "unknown"
            else:
                st = "mixed_unresolved"
            status_c[st] += 1
            n_ctx_resolved = sum(1 for r in res if r[0] == "NODE")
            missing_at = [f"{c['scene']}:{c['path']}" for c, r in zip(allc, res) if r[0] == "MISSING"][:3]
            xscene = sum(1 for r in res if r[0] == "NODE" and len(r) > 3 and r[3] == "xscene")
            uses_rows.append({
                "repo": repo_name, "group": group, "script": f, "line": u["line"], "kind": u["kind"], "path": u["path"],
                "status": st, "n_contexts": len(allc), "n_resolved": n_ctx_resolved, "n_structures": len(all_sigs),
                "types": ";".join(sorted(f"{t[0]}{'+' + t[1] if t[1] else ''}" for t in types)),
                "outcomes": ";".join(f"{k}={v}" for k, v in sorted(collections.Counter(r[0] for r in res).items())),
                "xscene_unique": xscene, "missing_at": ";".join(missing_at),
            })
        rows.append({
            "repo": repo_name, "group": group, "script": f, "lines": s["lines"], "class_name": s["class_name"] or "",
            "extends": s["extends"] or "", "native": s["native"] or "", "is_node": s["is_node"], "tool": s["tool"],
            "n_scenes": len(text_files), "bucket": ("0" if not text_files else "1" if len(text_files) == 1 else "2" if len(text_files) == 2 else "3+"),
            "n_text_attach": len(text), "n_text_nonroot": sum(1 for c in text if not c["root"]),
            "n_text_override_inst": sum(1 for c in text if c["how"] == "text_override_inst"),
            "n_text_override_inh": sum(1 for c in text if c["how"] == "text_override_inh"),
            "n_tres": len(tres_attach.get(f, ())), "n_subres": len(subres_attach.get(f, ())), "autoload": f in autoload,
            "zero_reason": reason, "n_subclasses": len(subs), "n_ctx_own": len(own), "n_ctx_all": len(allc),
            "n_inherited_ctx": len(inh), "n_instanced_ctx": len(ins), "plain_instances": plain_inst,
            "n_struct_own": len(own_sigs), "n_struct_all": len(all_sigs), "mech": "|".join(mech),
            "lit_uses": len(uses), "dollar": kinds["dollar"], "unique": kinds["unique"], "get_node_lit": kinds["get_node"],
            "get_node_or_null_lit": kinds["get_node_or_null"], "get_node_nonlit": len(s["nonlit"]),
            "other_recv_lit": len(s["other_recv_lit"]), "has_node_lit": len(s["has_node_lit"]), "find_child": len(s["find_child"]),
            "export_node": nexp, "export_nodepath": npexp, "dotdot": dotdot, "root_paths": rootp,
            "owner_dot": len(s["owner_dot"]), "owner_any": len(s["owner_any"]), "get_parent": len(s["get_parent"]),
            "get_children": len(s["get_children"]), "add_child": len(s["add_child"]),
            "set_script": s["set_script"],
            "st": dict(status_c),
            "scenes": ";".join(text_files[:8]) + (";..." if len(text_files) > 8 else ""),
        })
    # ---- repo-level scene stats
    inherited = [r for r, m in scene_meta.items() if m["inherited_from"]]
    rs = {
        "repo": repo_name, "group": group, "commit": R.commit, "projects": len([r for r in R.roots if (r + "/project.godot" if r else "project.godot") in R.fileset]),
        "tscn": len(scenes), "tres": sum(1 for f in R.files if f.endswith(".tres")), "scn": stats["binary_scn"],
        "formats": dict(fmt_counter),
        "inherited_scenes": len(inherited),
        "inherited_from_tscn": sum(1 for r in inherited if (scene_meta[r]["inherited_from"] or "").endswith(".tscn")),
        "inherited_from_model": sum(1 for r in inherited if not (scene_meta[r]["inherited_from"] or "").endswith(".tscn")),
        "inherited_scenes_with_structural_change": sum(1 for r in inherited if scene_meta[r]["added_in_base"] > 0),
        "base_scenes": len({scene_meta[r]["inherited_from"] for r in inherited}),
        "editable_entries": sum(m["editable"] for m in scene_meta.values()),
        "scenes_adding_into_instances": sum(1 for m in scene_meta.values() if m["added_in_inst"] > 0),
        "scenes_overriding_inside_instances": sum(1 for m in scene_meta.values() if m["override_in_inst"] > 0),
        "script_override_on_instance": sum(m["script_override_inst"] for m in scene_meta.values()),
        "dangling_entries": sum(m["dangling"] for m in scene_meta.values()),
        "builtin_scripts": builtin_scripts, "builtin_with_paths": builtin_with_paths,
        "autoloads_gd": sum(1 for a in autoload if a.endswith(".gd")),
        "autoloads_tscn": sum(1 for a in autoload if a.endswith(".tscn")),
        "parse_error": stats["parse_error"],
        "gd_total": len(scripts), "gd_first_party": sum(1 for s in scripts.values() if s["first_party"]),
        "gd_lines_first_party": sum(s["lines"] for s in scripts.values() if s["first_party"]),
    }
    # keep composed trees accessible for debugging
    return rs, rows, uses_rows, ctx_rows, {"scene_meta": scene_meta, "contexts": contexts, "cache": cache, "scripts": scripts}


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    corpus, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    only = set(sys.argv[3:])
    all_rs, all_rows, all_uses, all_ctx = [], [], [], []
    for name, url, sha, group in REPOS:
        if only and name not in only:
            continue
        rs, rows, uses, ctxs, _ = analyze_repo(name, group, corpus)
        rs["url"] = url
        all_rs.append(rs)
        all_rows.extend(rows)
        all_uses.extend(uses)
        all_ctx.extend(ctxs)
        print(f"{name}: {rs['gd_first_party']} scripts, {rs['tscn']} tscn, {rs['inherited_scenes']} inherited", flush=True)
    with open(os.path.join(out, "repos.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(all_rs, fh, indent=1)
    if all_rows:
        keys = list(all_rows[0].keys())
        with open(os.path.join(out, "scripts.csv"), "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=keys, lineterminator="\n")
            w.writeheader()
            for r in all_rows:
                r = dict(r)
                r["st"] = ";".join(f"{k}={v}" for k, v in sorted(r["st"].items()))
                w.writerow(r)
    for fname, data in (("uses.csv", all_uses), ("contexts.csv", all_ctx)):
        if data:
            with open(os.path.join(out, fname), "w", newline="", encoding="utf-8") as fh:
                w = csv.DictWriter(fh, fieldnames=list(data[0].keys()), lineterminator="\n")
                w.writeheader()
                w.writerows(data)


if __name__ == "__main__":
    main()
