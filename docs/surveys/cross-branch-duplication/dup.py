"""Duplication survey: is code duplication forced by different native bases common?

Usage: python dup.py --corpus CORPUS_DIR --api API_JSON --out OUT_DIR [--k 5] [repo ...]
  CORPUS_DIR  clones made by `python gdcorpus.py CORPUS_DIR`
  API_JSON    engine class/member names made by `python build_api.py GODOT_DIR API_JSON`
  --k         window size in non-trivial logical lines (default 5)
Writes OUT_DIR/units.csv, regions_{exact,near,loose}.csv, groups_{exact,near,loose}.csv, capability_names.csv,
consumers.csv, workarounds.json, summary.json, snippets_{exact,near,loose}.txt.
Python 3 stdlib only.
"""
import argparse
import collections
import csv
import json
import os
import re
import sys

import gdcorpus as A  # tokenizer, Repo, first-party rules, class hierarchy
from gdcorpus import REPOS

K = 5
MODES = ("exact", "near", "loose")  # near: locals+literals normalized; loose: every non-engine identifier blind (upper bound)
OUT = None
CORPUS = None
API = {}
NATIVE_PARENT = A.NATIVE_PARENT
KEYWORDS = A.KEYWORDS | {"get", "set", "match", "when", "lambda"}
API_NAMES = set()
ENGINE_METHODS = set()
GROUP_OF = {"game": "games", "demos": "demos", "plugin": "plugins/tools", "tool": "plugins/tools"}


def init(api_path):
    """Load the engine API names (needed by everything below, including logical_lines and is_trivial)."""
    global API
    API = A.load_api(api_path)
    API_NAMES.clear()
    API_NAMES.update(API)
    ENGINE_METHODS.clear()
    for c in API.values():
        for k in ("methods", "members", "signals", "constants"):
            API_NAMES.update(c[k])
        ENGINE_METHODS.update(c["methods"])
    _spec_cache.clear()

LIFECYCLE = {"_ready", "_enter_tree", "_init", "_exit_tree", "_notification", "_static_init"}
PER_FRAME = {"_process", "_physics_process", "_integrate_forces"}
INPUT = {"_input", "_unhandled_input", "_gui_input", "_unhandled_key_input", "_shortcut_input"}
HEADER_ANNOT = {"@tool", "@icon", "@static_unload", "@abstract"}
GROUP_ANNOT = {"@export_group", "@export_subgroup", "@export_category"}
TEST_RE = re.compile(r"(^|/)(tests?|unit_tests?)/|(^|/)test_[^/]*\.gd$|_test\.gd$|Test\.gd$|Tests\.gd$")


def native_chain(cls):
    out, seen = [], 0
    while cls and seen < 64:
        out.append(cls)
        cls = NATIVE_PARENT.get(cls)
        seen += 1
    return out


def lca(a, b):
    ca = native_chain(a)
    sb = set(native_chain(b))
    for c in ca:
        if c in sb:
            return c
    return None


def api_names_of(cls):
    s = set()
    for c in native_chain(cls):
        d = API.get(c)
        if d:
            for k in ("methods", "members", "signals", "constants"):
                s.update(d[k])
    return s


_spec_cache = {}


def specific_names(native, upto):
    """Engine member names declared on native's chain strictly below `upto` and not on upto's chain."""
    key = (native, upto)
    if key not in _spec_cache:
        below = set()
        for c in native_chain(native):
            if c == upto:
                break
            d = API.get(c)
            if d:
                for k in ("methods", "members", "signals", "constants"):
                    below.update(d[k])
        _spec_cache[key] = below - api_names_of(upto)
    return _spec_cache[key]


# ----------------------------------------------------------------------------------------------
# Logical lines


class LL:
    __slots__ = ("toks", "vals", "level", "line", "exact", "near", "loose", "trivial", "kind", "func", "unit", "decl")

    def __init__(self, toks):
        self.toks = toks
        self.vals = [t.v for t in toks]
        self.line = toks[0].line
        self.level = 0
        self.kind = None
        self.func = None
        self.unit = None
        self.decl = None


def render(t, near):
    if t.k == "STR":
        return '"$"' if near else (t.pre + '"' + t.v.replace("\\", "\\\\").replace('"', '\\"') + '"')
    if t.k == "NUM":
        return "#" if near else t.v
    if t.k == "ID" and near and t.v not in KEYWORDS and t.v not in API_NAMES:
        return "ι"
    return t.v


def logical_lines(src):
    toks = A.tokenize(src)
    lines, cur, depth = [], [], 0
    for t in toks:
        if t.k == "NL":
            if depth > 0:
                continue
            if cur:
                lines.append(LL(cur))
                cur = []
            continue
        if t.k == "OP":
            if t.v in "([{":
                depth += 1
            elif t.v in ")]}":
                depth = max(0, depth - 1)
        cur.append(t)
    if cur:
        lines.append(LL(cur))
    stack = [0]
    for ll in lines:
        c = ll.toks[0].col
        while len(stack) > 1 and stack[-1] > c:
            stack.pop()
        if c > stack[-1]:
            stack.append(c)
        ll.level = len(stack) - 1
        ll.exact = " ".join(render(t, False) for t in ll.toks)
        ll.loose = " ".join(render(t, True) for t in ll.toks)
    return lines


def render_near(t, local):
    """Near-miss: literals and identifiers declared as locals/parameters of the unit are normalized."""
    if t.k == "STR":
        return '"$"'
    if t.k == "NUM":
        return "#"
    if t.k == "ID" and t.v in local:
        return "λ"
    return t.v


def strip_annots(vals, toks):
    """Skip leading annotations (and their parenthesised args). Returns (annots, rest_index)."""
    i, ann = 0, []
    while i < len(toks) and toks[i].k == "AN":
        ann.append(toks[i].v)
        i += 1
        if i < len(toks) and toks[i].k == "OP" and toks[i].v == "(":
            d = 0
            while i < len(toks):
                if toks[i].k == "OP" and toks[i].v == "(":
                    d += 1
                elif toks[i].k == "OP" and toks[i].v == ")":
                    d -= 1
                    if d == 0:
                        i += 1
                        break
                i += 1
    return ann, i


def is_trivial(ll):
    v = ll.vals
    if v in (["pass"], ["return"], ["else", ":"], ["break"], ["continue"], ["pass", ";"]):
        return True
    if len(v) == 1 and ll.toks[0].k == "STR":
        return True
    ann, i = strip_annots(v, ll.toks)
    rest = v[i:]
    if not rest:
        return all(a in HEADER_ANNOT or a in GROUP_ANNOT for a in ann) if ann else True
    if rest[0] in ("extends", "class_name"):
        return True
    if rest[0] == "class" and rest[-1] == ":":
        return True
    j = 1 if rest[0] == "static" else 0
    if len(rest) > j + 1 and rest[j] == "func" and rest[-1] == ":":
        name = rest[j + 1]
        if name.startswith("_") and name in ENGINE_METHODS:
            return True
    return False


def func_kind(name):
    if name in LIFECYCLE:
        return "lifecycle"
    if name in PER_FRAME:
        return "per_frame"
    if name in INPUT:
        return "input"
    if name.startswith("_") and name in ENGINE_METHODS:
        return "other_virtual"
    if name.startswith("_on_") or name.startswith("on_"):
        return "handler"
    return "method"


# ----------------------------------------------------------------------------------------------
# Units (top-level script classes and inner classes)


class Unit:
    def __init__(self, uid, repo, proj, file, inner, parent):
        self.uid, self.repo, self.proj, self.file, self.inner, self.parent = uid, repo, proj, file, inner, parent
        self.class_name = None
        self.ext_kind = None
        self.ext = None
        self.base = None  # unit uid
        self.native = None
        self.chain = []
        self.lines = []  # all logical lines (LL) belonging to this unit
        self.body_level = 0
        self.first_party = False
        self.test = False
        self.editor = False
        self.is_node = False
        self.members = set()  # body-level var/const names
        self.funcs = set()
        self.signals = set()
        self.idents = set()
        self.bare = set()  # identifiers accessed without a receiver, or via self./super.
        self.locals = set()  # local vars, loop vars, parameters
        self.pubvars = set()  # body-level `var` that is not @onready
        self.header_line = 1

    @property
    def label(self):
        return self.file + ("::" + self.inner if self.inner else "")


def parse_header_extends(vals, toks, start):
    """vals[start] == 'extends' -> (kind, spec)."""
    j = start + 1
    if j >= len(toks):
        return None, None
    if toks[j].k == "STR":
        return "path", toks[j].v
    if toks[j].k == "ID":
        name = toks[j].v
        k = j + 1
        while k + 1 < len(toks) and toks[k].v == "." and toks[k + 1].k == "ID":
            name += "." + toks[k + 1].v
            k += 2
        return "name", name
    return None, None


def build_units(R, f, src, info, counter):
    lines = logical_lines(src)
    top = Unit(f"{R.name}:{f}", R.name, R.project_of(f).root, f, None, None)
    top.class_name = info["class_name"]
    top.ext_kind, top.ext = info["extends_kind"], info["extends"]
    units = [top]
    stack = [(top, -1)]
    for ll in lines:
        while len(stack) > 1 and stack[-1][1] >= ll.level:
            stack.pop()
        u = stack[-1][0]
        v = ll.vals
        if v and v[0] == "class" and len(v) >= 3 and ll.toks[1].k == "ID" and v[-1] == ":":
            inner = (u.inner + "." if u.inner else "") + v[1]
            nu = Unit(f"{R.name}:{f}::{inner}", R.name, top.proj, f, inner, u)
            nu.header_line = ll.line
            if "extends" in v:
                nu.ext_kind, nu.ext = parse_header_extends(v, ll.toks, v.index("extends"))
            nu.body_level = ll.level + 1
            units.append(nu)
            ll.unit = u
            u.lines.append(ll)  # header line counted (as trivial) in the enclosing unit
            stack.append((nu, ll.level))
            continue
        ll.unit = u
        u.lines.append(ll)
    # contexts and kinds
    for u in units:
        cur_func = None
        for ll in u.lines:
            v = ll.vals
            ann, i = strip_annots(v, ll.toks)
            rest = v[i:]
            tk = ll.toks
            for q, t in enumerate(tk):
                if t.k != "ID":
                    continue
                u.idents.add(t.v)
                if q == 0 or tk[q - 1].v != "." or (q >= 2 and tk[q - 2].v in ("self", "super")):
                    u.bare.add(t.v)
                if q > 0 and tk[q - 1].v in ("var", "const", "for") and ll.level > u.body_level:
                    u.locals.add(t.v)
                if t.v == "func":
                    r = q + 1
                    if r < len(tk) and tk[r].k == "ID":
                        r += 1
                    if r < len(tk) and tk[r].v == "(":
                        d = 0
                        while r < len(tk):
                            if tk[r].v in "([{" and tk[r].k == "OP":
                                d += 1
                            elif tk[r].v in ")]}" and tk[r].k == "OP":
                                d -= 1
                                if d == 0:
                                    break
                            elif d == 1 and tk[r].k == "ID" and tk[r - 1].v in ("(", ",") and r + 1 < len(tk) and tk[r + 1].v in (":", "=", ",", ")"):
                                u.locals.add(tk[r].v)
                            r += 1
            if ll.level <= u.body_level:
                cur_func = None
                j = 1 if rest and rest[0] == "static" else 0
                if len(rest) > j + 1 and rest[j] == "func":
                    cur_func = rest[j + 1]
                    u.funcs.add(cur_func)
                    ll.kind = func_kind(cur_func)
                    ll.func = cur_func
                elif rest and rest[0] == "signal" and len(rest) > 1:
                    u.signals.add(rest[1])
                    ll.kind = "signal"
                    ll.decl = rest[1]
                elif len(rest) > j + 1 and rest[j] in ("var", "const"):
                    u.members.add(rest[j + 1])
                    ll.decl = rest[j + 1]
                    if any(a.startswith("@export") and a not in GROUP_ANNOT for a in ann):
                        ll.kind = "export"
                    elif "@onready" in ann:
                        ll.kind = "onready"
                    else:
                        ll.kind = "member"
                    if rest[j] == "var" and ll.kind != "onready":
                        u.pubvars.add(rest[j + 1])
                elif rest and rest[0] == "enum":
                    if len(rest) > 1 and ll.toks[i + 1].k == "ID":
                        u.members.add(rest[1])
                    ll.kind = "member"
                else:
                    ll.kind = "class_other"
            else:
                if cur_func is not None:
                    ll.kind = func_kind(cur_func)
                    ll.func = cur_func
                else:
                    ll.kind = "member"  # property accessor bodies, multi-line decls
            ll.trivial = is_trivial(ll)
    for u in units:
        local = u.locals - u.members - u.funcs - u.signals
        for ll in u.lines:
            ll.near = " ".join(render_near(t, local) for t in ll.toks)
    counter["inner_units"] += len(units) - 1
    return units


# ----------------------------------------------------------------------------------------------
# Clone detection


def find_regions(seqs, attr):
    """seqs: uid -> list of LL (non-trivial). Returns list of (u1, a1, a2, u2, b1, b2) (half-open)."""
    buckets = collections.defaultdict(list)
    for uid, L in seqs.items():
        texts = [(ll.level, getattr(ll, attr)) for ll in L]
        for i in range(len(L) - K + 1):
            base = texts[i][0]
            key = tuple((texts[j][0] - base, texts[j][1]) for j in range(i, i + K))
            buckets[key].append((uid, i))
    pairs = collections.defaultdict(set)
    big = 0
    for occ in buckets.values():
        if len(occ) < 2:
            continue
        if len(occ) > 200:
            big += 1
        for a in range(len(occ)):
            for b in range(a + 1, len(occ)):
                x, y = occ[a], occ[b]
                if x > y:
                    x, y = y, x
                if x[0] == y[0] and y[1] - x[1] < K:
                    continue
                pairs[(x[0], y[0])].add((x[1], y[1]))
    regs = []
    for (u1, u2), ms in pairs.items():
        ms = sorted(ms, key=lambda m: (m[1] - m[0], m[0]))
        start = prev = None
        for m in ms:
            if prev is not None and m[1] - m[0] == prev[1] - prev[0] and m[0] == prev[0] + 1:
                prev = m
                continue
            if start is not None:
                regs.append((u1, start[0], prev[0] + K, u2, start[1], prev[1] + K))
            start = prev = m
        if start is not None:
            regs.append((u1, start[0], prev[0] + K, u2, start[1], prev[1] + K))
    return regs, big


class UF:
    def __init__(self):
        self.p = {}

    def find(self, x):
        self.p.setdefault(x, x)
        while self.p[x] != x:
            self.p[x] = self.p[self.p[x]]
            x = self.p[x]
        return x

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.p[ra] = rb


def make_groups(regs):
    """Connected components of region copies (overlapping copies in one unit are merged)."""
    uf = UF()
    ivs = collections.defaultdict(list)
    for k, r in enumerate(regs):
        uf.union((k, 0), (k, 1))
        ivs[r[0]].append((r[1], r[2], (k, 0)))
        ivs[r[3]].append((r[4], r[5], (k, 1)))
    for uid, lst in ivs.items():
        lst.sort()
        end, rep = -1, None
        for s, e, node in lst:
            if rep is not None and s < end:
                uf.union(node, rep)
                end = max(end, e)
            else:
                rep, end = node, e
    comps = collections.defaultdict(lambda: collections.defaultdict(list))
    for uid, lst in ivs.items():
        for s, e, node in lst:
            comps[uf.find(node)][uid].append((s, e))
    groups = []
    for root, members in comps.items():
        mem = {}
        for uid, spans in members.items():
            spans.sort()
            merged = []
            for s, e in spans:
                if merged and s <= merged[-1][1]:
                    merged[-1][1] = max(merged[-1][1], e)
                else:
                    merged.append([s, e])
            mem[uid] = merged
        rks = sorted({node[0] for uid, lst in ivs.items() for s, e, node in lst if uf.find(node) == root})
        groups.append({"members": mem, "regions": rks})
    return groups


# ----------------------------------------------------------------------------------------------
# Pair classification


def common_script_ancestor(u, v):
    return bool(set(u.chain) & set(v.chain))


def pair_category(u, v):
    if u.uid == v.uid:
        return "intra_unit"
    if u.test or v.test or u.editor or v.editor:
        return "tool_test"
    if common_script_ancestor(u, v):
        return "same_chain"
    if not u.native or u.native.startswith("?") or not v.native or v.native.startswith("?"):
        return "unresolved"
    if not u.is_node or not v.is_node:
        return "non_node"
    if u.native == v.native:
        return "same_native_unrelated"
    return "cross_branch"


def needs_specific(x, y):
    """Engine members specific to x's native below LCA(x, y) that x accesses on self (name evidence)."""
    sp = specific_names(x.native, lca(x.native, y.native))
    return (x.bare - x.locals - x.members - x.funcs - x.signals - KEYWORDS) & sp


_forced_cache = {}


def forced(u, v):
    key = (u.uid, v.uid)
    if key not in _forced_cache:
        _forced_cache[key] = bool(needs_specific(u, v) or needs_specific(v, u))
    return _forced_cache[key]


CATS = ["cross_branch", "same_native_unrelated", "same_chain", "non_node", "tool_test", "intra_unit", "unresolved"]
KINDS = ["export", "onready", "signal", "member", "lifecycle", "per_frame", "input", "handler", "method", "other_virtual", "class_other"]


# ----------------------------------------------------------------------------------------------
# Main per-repo analysis


def analyze(repo_name, group):
    R = A.Repo(CORPUS, repo_name, group)
    counter = collections.Counter()
    infos, units = {}, {}
    file_units = {}
    for f in R.files:
        if not f.endswith(".gd"):
            continue
        try:
            src = R.read(f)
        except OSError:
            continue
        info = A.parse_header(src)
        infos[f] = info
        us = build_units(R, f, src, info, counter)
        file_units[f] = us
        fp = R.first_party(f)
        for u in us:
            u.first_party = fp
            units[u.uid] = u
    # class_name map per project (all scripts, incl. vendored, so chains through vendored bases resolve)
    cname = collections.defaultdict(dict)
    for f, us in file_units.items():
        if us[0].class_name:
            cname[us[0].proj][us[0].class_name] = us[0].uid
    inner_by_file = collections.defaultdict(dict)
    for f, us in file_units.items():
        for u in us[1:]:
            inner_by_file[f][u.inner] = u.uid
            inner_by_file[f].setdefault(u.inner.split(".")[-1], u.uid)

    def resolve_name(u, name):
        if name in NATIVE_PARENT:
            return None, name
        parts = name.split(".")
        # inner class of the same file (relative to enclosing scopes)
        scope = u.parent
        while scope is not None:
            key = (scope.inner + "." if scope.inner else "") + name
            if key in inner_by_file[u.file]:
                return inner_by_file[u.file][key], None
            scope = scope.parent
        if name in inner_by_file[u.file] and inner_by_file[u.file][name] != u.uid:
            return inner_by_file[u.file][name], None
        if parts[0] in cname[u.proj]:
            t = cname[u.proj][parts[0]]
            if len(parts) == 1:
                return t, None
            tf = units[t].file
            sub = ".".join(parts[1:])
            if sub in inner_by_file[tf]:
                return inner_by_file[tf][sub], None
        return None, "?" + name

    for u in units.values():
        proj = R.project_of(u.file)
        if u.ext_kind == "path":
            rel = proj.to_rel(u.ext, u.file)
            if u.ext.startswith("uid://"):
                rel = R.uid.get(u.ext)
            if rel and rel in file_units:
                u.base = file_units[rel][0].uid
            else:
                u.native = "?" + str(u.ext)
        elif u.ext_kind == "name":
            b, nat = resolve_name(u, u.ext)
            if b:
                u.base = b
            else:
                u.native = nat
        else:
            u.native = "RefCounted"
    for u in units.values():
        chain, cur, seen = [], u, set()
        while cur is not None and cur.uid not in seen:
            seen.add(cur.uid)
            chain.append(cur.uid)
            if cur.base is None:
                u.native = cur.native
                break
            cur = units.get(cur.base)
        u.chain = chain
        nat = u.native or "?"
        u.is_node = not nat.startswith("?") and A.native_is(nat, "Node")
        u.editor = not nat.startswith("?") and nat.startswith("Editor")
    for u in units.values():
        sub = u.file[len(u.proj) + 1:] if u.proj else u.file
        names = {units[c].class_name for c in u.chain if units[c].class_name} | {units[c].ext for c in u.chain if units[c].ext}
        u.test = bool(TEST_RE.search(sub)) or bool(names & {"GdUnitTestSuite", "GutTest", "GdUnitTestSuiteBuilder"})
    fp_units = [u for u in units.values() if u.first_party]

    # ---- per project clone detection
    by_proj = collections.defaultdict(list)
    for u in fp_units:
        by_proj[u.proj].append(u)
    res = {m: [] for m in MODES}
    cov = {m: collections.defaultdict(lambda: collections.defaultdict(set)) for m in MODES}
    seqs_all = {}
    pair_pop = collections.Counter()
    for proj, us in sorted(by_proj.items()):
        seqs = {u.uid: [ll for ll in u.lines if not ll.trivial] for u in us}
        seqs_all.update(seqs)
        # population of unit pairs per category (for rates)
        lst = sorted(us, key=lambda x: x.uid)
        for i in range(len(lst)):
            for j in range(i + 1, len(lst)):
                pair_pop[pair_category(lst[i], lst[j])] += 1
        for mode in MODES:
            regs, big = find_regions(seqs, mode)
            counter[f"big_buckets_{mode}"] += big
            for r in regs:
                u1, u2 = units[r[0]], units[r[3]]
                cat = pair_category(u1, u2)
                sub = ("cross_forced" if forced(u1, u2) else "cross_lowerable") if cat == "cross_branch" else ""
                res[mode].append((proj, cat, sub) + r)
                for k in range(r[1], r[2]):
                    cov[mode][r[0]][k].add(cat)
                    if sub:
                        cov[mode][r[0]][k].add(sub)
                for k in range(r[4], r[5]):
                    cov[mode][r[3]][k].add(cat)
                    if sub:
                        cov[mode][r[3]][k].add(sub)
    return R, units, fp_units, seqs_all, res, cov, pair_pop, counter, infos, file_units, cname


def lines_by_cat(fp_units, seqs, cov):
    out = collections.Counter()
    excl = collections.Counter()
    for u in fp_units:
        c = cov.get(u.uid, {})
        for k, cats in c.items():
            out["any"] += 1
            for cat in cats:
                out[cat] += 1
            main = cats - {"cross_forced", "cross_lowerable"}
            if len(main) == 1:
                excl[next(iter(main))] += 1
                if main == {"cross_branch"}:
                    for s in cats - main:
                        excl[s] += 1
    return out, excl


def main():
    global K, OUT, CORPUS
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--api", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--k", type=int, default=5)
    ap.add_argument("repos", nargs="*")
    args = ap.parse_args()
    K, OUT, CORPUS = args.k, args.out, args.corpus
    init(args.api)
    os.makedirs(OUT, exist_ok=True)
    only = set(args.repos)
    summary = {"K": K, "repos": []}
    unit_rows, region_rows = [], {m: [] for m in MODES}
    group_rows = {m: [] for m in MODES}
    snippets = {m: [] for m in MODES}
    cap_rows, consumer_rows = [], []
    workarounds = {}
    for name, url, sha, grp in REPOS:
        if only and name not in only:
            continue
        group = GROUP_OF[grp]
        R, units, fp_units, seqs, res, cov, pair_pop, counter, infos, file_units, cname = analyze(name, grp)
        lloc = sum(len(seqs[u.uid]) for u in fp_units)
        rs = {"repo": name, "group": group, "commit": R.commit, "units": len(fp_units),
              "files": len({u.file for u in fp_units}), "lloc": lloc,
              "projects_with_code": len({u.proj for u in fp_units}), "pair_pop": dict(pair_pop),
              "inner_units": counter["inner_units"], "big_buckets": {m: counter[f"big_buckets_{m}"] for m in MODES}}
        for mode in MODES:
            byc, excl = lines_by_cat(fp_units, seqs, cov[mode])
            rs[mode] = {"lines": dict(byc), "exclusive": dict(excl)}
            # pairs with clones, per category
            pc = collections.defaultdict(set)
            for row in res[mode]:
                proj, cat, sub, u1, a1, a2, u2, b1, b2 = row
                pc[cat].add((u1, u2))
                if sub:
                    pc[sub].add((u1, u2))
            rs[mode]["pairs_with_clones"] = {c: len(v) for c, v in pc.items()}
            # cross-branch kinds (lines across all copies, from coverage)
            kinds = collections.Counter()
            connect_lines = 0
            for u in fp_units:
                c = cov[mode].get(u.uid, {})
                for k, cats in c.items():
                    if "cross_branch" in cats:
                        ll = seqs[u.uid][k]
                        kinds[ll.kind] += 1
                        if "connect" in ll.vals:
                            connect_lines += 1
            rs[mode]["cross_kinds"] = dict(kinds)
            rs[mode]["cross_connect_lines"] = connect_lines
            # groups per category
            for cat in ("cross_branch", "same_native_unrelated", "same_chain"):
                regs = [r[3:] for r in res[mode] if r[1] == cat]
                groups = make_groups(regs)
                rs[mode].setdefault("groups", {})[cat] = len(groups)
                if cat != "cross_branch":
                    continue
                npairs = collections.Counter()
                for gi, g in enumerate(groups):
                    mem = g["members"]
                    size = sum(e - s for spans in mem.values() for s, e in spans)
                    natives = sorted({units[uid].native for uid in mem})
                    np_ = collections.Counter()
                    rels = set()
                    needs = {}
                    for k in g["regions"]:
                        r = regs[k]
                        a, b = units[r[0]], units[r[3]]
                        pair = tuple(sorted((a.native, b.native)))
                        np_[pair] += 1
                        c = lca(a.native, b.native)
                        rels.add("ancestor" if c in pair else "sibling")
                        for x, y in ((a, b), (b, a)):
                            needs[x.uid] = needs.get(x.uid, set()) | needs_specific(x, y)
                    for p in np_:
                        npairs[p] += 1
                    # representative = largest copy
                    rep_uid, rep_spans = max(mem.items(), key=lambda kv: (sum(e - s for s, e in kv[1]), kv[0]))
                    rep_lines = [seqs[rep_uid][k] for s, e in rep_spans for k in range(s, e)]
                    kinds = collections.Counter(ll.kind for ll in rep_lines)
                    ru = units[rep_uid]
                    used = set()
                    for ll in rep_lines:
                        for t in ll.toks:
                            if t.k == "ID":
                                used.add(t.v)
                    declared_in_region = {ll.decl for ll in rep_lines if ll.decl}
                    state_refs = sorted((used & ru.members) | (used & ru.signals))
                    own_calls = sorted((used & ru.funcs) - {ll.func for ll in rep_lines if ll.func})
                    self_api = sorted(used & api_names_of(ru.native) - KEYWORDS)
                    stateful_decl = any(k in kinds for k in ("export", "onready", "signal", "member"))
                    if stateful_decl or state_refs:
                        h = "b_stateful"
                    elif own_calls or self_api:
                        h = "a_interface_default"
                    else:
                        h = "d_static_helper"
                    gid = f"{name}#{mode}#{gi}"
                    members_txt = ";".join(
                        f"{units[uid].label}:{seqs[uid][s].line}-{seqs[uid][e - 1].line}({e - s})"
                        for uid, spans in sorted(mem.items()) for s, e in spans)
                    is_forced = any(forced(units[regs[k][0]], units[regs[k][3]]) for k in g["regions"])
                    group_rows[mode].append({
                        "gid": gid, "repo": name, "group": group, "project": units[rep_uid].proj, "size_all_copies": size,
                        "forced": is_forced, "unit_pairs": len({(regs[k][0], regs[k][3]) for k in g["regions"]}),
                        "rep_lines": len(rep_lines), "n_units": len(mem), "natives": "|".join(natives),
                        "native_pairs": ";".join(f"{a}/{b}" for a, b in sorted(np_)), "relation": "|".join(sorted(rels)),
                        "needs_specific": ";".join(f"{units[u].label.split('/')[-1]}:{','.join(sorted(v))[:120]}" for u, v in sorted(needs.items())),
                        "kinds": ";".join(f"{k}={v}" for k, v in kinds.most_common()),
                        "state_refs": ",".join(state_refs)[:200], "own_calls": ",".join(own_calls)[:200],
                        "heuristic": h, "members": members_txt,
                    })
                    snippets[mode].append((size, gid, members_txt, rep_uid, rep_lines))
                rs[mode]["cross_native_pairs"] = {f"{a}/{b}": v for (a, b), v in npairs.most_common()}
            for row in res[mode]:
                proj, cat, sub, u1, a1, a2, u2, b1, b2 = row
                U1, U2 = units[u1], units[u2]
                if cat == "same_chain":
                    sub = "parent_child" if (u1 in U2.chain or u2 in U1.chain) else "siblings"
                region_rows[mode].append({
                    "repo": name, "group": group, "project": proj, "category": cat, "sub": sub,
                    "a": U1.label, "a_lines": f"{seqs[u1][a1].line}-{seqs[u1][a2 - 1].line}", "a_native": U1.native,
                    "b": U2.label, "b_lines": f"{seqs[u2][b1].line}-{seqs[u2][b2 - 1].line}", "b_native": U2.native,
                    "n": a2 - a1,
                })
        # ---- per-unit rows
        for u in fp_units:
            unit_rows.append({
                "repo": name, "group": group, "project": u.proj, "unit": u.label, "class_name": u.class_name or "",
                "extends": u.ext or "", "native": u.native, "chain_len": len(u.chain), "is_node": u.is_node,
                "test": u.test, "editor": u.editor, "lloc": len(seqs[u.uid]),
                **{f"exact_{c}": sum(1 for k, cats in cov["exact"].get(u.uid, {}).items() if c in cats) for c in CATS + ["cross_forced"]},
                "exact_any": len(cov["exact"].get(u.uid, {})),
                "near_any": len(cov["near"].get(u.uid, {})),
                "near_cross_branch": sum(1 for k, cats in cov["near"].get(u.uid, {}).items() if "cross_branch" in cats),
                "loose_cross_branch": sum(1 for k, cats in cov["loose"].get(u.uid, {}).items() if "cross_branch" in cats),
            })
        # ---- shared capability
        cap, cons, wk = capability(R, units, fp_units, seqs, cname, infos, file_units)
        rs["capability"] = cap["summary"]
        cap_rows.extend(cap["rows"])
        consumer_rows.extend(cons)
        workarounds[name] = wk
        summary["repos"].append(rs)
        print(f"{name}: {len(fp_units)} units, lloc {lloc}, exact dup {rs['exact']['lines'].get('any', 0)}, "
              f"cross {rs['exact']['lines'].get('cross_branch', 0)} / near {rs['near']['lines'].get('cross_branch', 0)}", flush=True)
    write_outputs(summary, unit_rows, region_rows, group_rows, snippets, cap_rows, consumer_rows, workarounds)


# ----------------------------------------------------------------------------------------------
# Shared-capability names and how they are consumed


def has_or(vals):
    return "or" in vals or any(vals[i] == "|" and vals[i + 1] == "|" for i in range(len(vals) - 1))


def capability(R, units, fp_units, seqs, cname, infos, file_units):
    rows = []
    node_units = [u for u in fp_units if u.is_node and not u.test and not u.editor]
    by_proj = collections.defaultdict(list)
    for u in node_units:
        by_proj[u.proj].append(u)
    cap_names = collections.defaultdict(set)  # (proj, name) -> uids
    cross_names = set()
    pub = {}
    cross_pairs = []
    for proj, us in by_proj.items():
        for u in us:
            eng = api_names_of(u.native)
            pub[u.uid] = {n for n in (u.funcs | u.pubvars | u.signals) if not n.startswith("_") and not n[:1].isupper() and n not in eng}
            for n in pub[u.uid]:
                cap_names[(proj, n)].add(u.uid)
        lst = sorted(us, key=lambda x: x.uid)
        for i in range(len(lst)):
            for j in range(i + 1, len(lst)):
                a, b = lst[i], lst[j]
                if pair_category(a, b) != "cross_branch":
                    continue
                sh = pub[a.uid] & pub[b.uid]
                if sh:
                    cross_pairs.append((a, b, sh))
                    for n in sh:
                        cross_names.add((proj, n))
    # consumers: accesses .name on a non-self receiver, string calls, has_method
    cons = []
    names_by_proj = collections.defaultdict(set)
    for (proj, n) in cross_names:
        names_by_proj[proj].add(n)
    class_names = {u.class_name for u in units.values() if u.class_name}
    mech_count = collections.Counter()
    for u in fp_units:
        if u.test:
            continue
        names = names_by_proj.get(u.proj)
        if not names:
            continue
        # function spans
        funcs = collections.defaultdict(list)
        for ll in u.lines:
            funcs[ll.func].append(ll)
        typed_members = {}
        for ll in u.lines:
            if ll.kind in ("member", "onready", "export") and ll.decl and ll.level <= u.body_level:
                v = ll.vals
                if ":" in v:
                    k = v.index(":")
                    if k + 1 < len(v) and ll.toks[k + 1].k == "ID":
                        typed_members[ll.decl] = v[k + 1]
                    elif k + 1 < len(v) and v[k + 1] == "=":
                        typed_members[ll.decl] = "<inferred>"
                if ll.decl not in typed_members and "as" in v:
                    k = len(v) - 1 - v[::-1].index("as")
                    if k + 1 < len(v) and ll.toks[k + 1].k == "ID":
                        typed_members[ll.decl] = v[k + 1]
        for fn, lls in funcs.items():
            toks = [t for ll in lls for t in ll.toks]
            vals = [t.v for t in toks]
            has_method_strs = {toks[i + 2].v for i in range(len(toks) - 2) if vals[i] == "has_method" and toks[i + 2].k == "STR"}
            group_check = "is_in_group" in vals
            is_classes = [vals[i + 1] for i in range(len(toks) - 1) if vals[i] == "is" and toks[i + 1].k == "ID" and vals[i + 1] in class_names]
            or_class = has_or(vals) and len(set(is_classes)) >= 2
            typed_locals = {}
            for i in range(len(toks) - 2):
                if toks[i].k == "ID" and vals[i + 1] == ":" and (i == 0 or vals[i - 1] in ("var", "(", ",", "for")):
                    if toks[i + 2].k == "ID":
                        typed_locals[vals[i]] = vals[i + 2]
                    elif vals[i + 2] == "=" and vals[i - 1] == "var":
                        typed_locals[vals[i]] = "<inferred>"
            for i, t in enumerate(toks):
                if t.k == "STR" and t.v in names and i >= 2 and vals[i - 1] == "(" and vals[i - 2] in ("call", "call_deferred", "callv", "has_method", "rpc", "rpc_id", "get", "set", "Callable", "emit_signal", "has_signal", "connect"):
                    m = "string_" + vals[i - 2]
                    cons.append({"repo": R.name, "project": u.proj, "unit": u.label, "line": t.line, "name": t.v, "mechanism": m})
                    mech_count[m] += 1
                    continue
                if t.k != "ID" or t.v not in names or i == 0 or vals[i - 1] != "." or i < 2:
                    continue
                recv = toks[i - 2]
                if recv.v in ("self", "super"):
                    continue
                if recv.v == "owner" or (recv.v == ")" and i >= 4 and vals[i - 4] == "get_parent"):
                    m = "owner_or_parent"
                elif i >= 3 and (vals[i - 3] in ("$", "%", "/") or (recv.k == "STR" and vals[i - 3] == "$")):
                    m = "node_path"
                elif t.v in has_method_strs:
                    m = "has_method_guard"
                elif or_class:
                    m = "is_A_or_B"
                elif recv.k == "ID" and (typed_locals.get(recv.v) or typed_members.get(recv.v)):
                    ty = typed_locals.get(recv.v) or typed_members.get(recv.v)
                    m = "typed_" + ("script" if ty in class_names else "inferred" if ty == "<inferred>" else "engine_or_other")
                elif recv.k == "ID" and recv.v in class_names:
                    m = "static_or_autoload_class"
                elif recv.k == "ID" and recv.v[:1].isupper():
                    m = "autoload_or_class"
                elif is_classes:
                    m = "is_A_check"
                elif group_check:
                    m = "group_then_call"
                else:
                    m = "untyped_other"
                cons.append({"repo": R.name, "project": u.proj, "unit": u.label, "line": t.line, "name": t.v, "mechanism": m,
                             "recv": recv.v})
                mech_count[m] += 1
    consumed = {(c["project"], c["name"]) for c in cons}
    pairs_any = sum(1 for a, b, sh in cross_pairs if any((a.proj, n) in consumed for n in sh))
    pairs_2 = sum(1 for a, b, sh in cross_pairs if sum(1 for n in sh if (a.proj, n) in consumed) >= 2)
    by_name_mech = collections.defaultdict(collections.Counter)
    for c in cons:
        by_name_mech[(c["project"], c["name"])][c["mechanism"]] += 1
    for (proj, n) in sorted(cross_names):
        uids = cap_names[(proj, n)]
        rows.append({"repo": R.name, "project": proj, "name": n, "n_declarers": len(uids),
                     "n_natives": len({units[x].native for x in uids}),
                     "natives": "|".join(sorted({units[x].native for x in uids})),
                     "kind": "func" if any(n in units[x].funcs for x in uids) else ("signal" if any(n in units[x].signals for x in uids) else "var"),
                     "consumer_sites": sum(by_name_mech[(proj, n)].values()),
                     "mechanisms": ";".join(f"{k}={v}" for k, v in by_name_mech[(proj, n)].most_common()),
                     "declarers": ";".join(sorted(units[x].label for x in uids))[:400]})
    # workarounds
    wk = {}
    wk["static_funcs"] = sum(1 for u in fp_units for ll in u.lines if ll.vals[:2] == ["static", "func"])
    wk["has_method_calls"] = sum(1 for u in fp_units if not u.test for ll in u.lines for v in ll.vals if v == "has_method")
    wk["string_call"] = sum(1 for u in fp_units if not u.test for ll in u.lines for i, v in enumerate(ll.vals)
                            if v in ("call", "call_deferred", "callv") and i + 2 < len(ll.vals) and ll.vals[i + 1] == "(" and ll.toks[i + 2].k == "STR")
    wk["is_A_or_B_lines"] = 0
    for u in fp_units:
        if u.test:
            continue
        for ll in u.lines:
            iscls = [ll.vals[i + 1] for i in range(len(ll.vals) - 1) if ll.vals[i] == "is" and ll.vals[i + 1] in class_names]
            if len(set(iscls)) >= 2 and has_or(ll.vals):
                wk["is_A_or_B_lines"] += 1
    od_sites, od_units = 0, set()
    for u in fp_units:
        if u.test:
            continue
        for ll in u.lines:
            v = ll.vals
            for i in range(len(v) - 2):
                hit = (v[i] == "owner" and (i == 0 or v[i - 1] != ".") and v[i + 1] == "." and ll.toks[i + 2].k == "ID" and v[i + 2] not in API_NAMES)
                hit = hit or (v[i] == "get_parent" and i + 4 < len(v) and v[i + 1:i + 4] == ["(", ")", "."] and ll.toks[i + 4].k == "ID" and v[i + 4] not in API_NAMES)
                if hit:
                    od_sites += 1
                    od_units.add(u.uid)
    wk["owner_or_parent_duck_sites"] = od_sites
    wk["owner_or_parent_duck_units"] = len(od_units)
    wk["is_in_group_calls"] = sum(1 for u in fp_units if not u.test for ll in u.lines for v in ll.vals if v == "is_in_group")
    summary = {"node_units": len(node_units), "cross_pairs_declaring_same": len(cross_pairs),
               "cross_pairs_sharing_1plus": pairs_any, "cross_pairs_sharing_2plus": pairs_2,
               "cross_shared_names": len(cross_names), "cross_shared_names_consumed": len(consumed),
               "consumer_mechanisms": dict(mech_count)}
    return {"summary": summary, "rows": rows}, cons, wk


def write_outputs(summary, unit_rows, region_rows, group_rows, snippets, cap_rows, consumer_rows, workarounds):
    def wcsv(fname, rows):
        if not rows:
            return
        keys = list(rows[0].keys())
        for r in rows:
            for k in r:
                if k not in keys:
                    keys.append(k)
        with open(os.path.join(OUT, fname), "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=keys, lineterminator="\n")
            w.writeheader()
            w.writerows(rows)

    wcsv("units.csv", unit_rows)
    for mode in MODES:
        wcsv(f"regions_{mode}.csv", region_rows[mode])
        wcsv(f"groups_{mode}.csv", sorted(group_rows[mode], key=lambda r: -r["size_all_copies"]))
        with open(os.path.join(OUT, f"snippets_{mode}.txt"), "w", encoding="utf-8", newline="\n") as fh:
            for size, gid, members, rep_uid, lines in sorted(snippets[mode], key=lambda x: (-x[0], x[1])):
                fh.write(f"=== {gid} size(all copies)={size} rep={rep_uid}\n    members: {members}\n")
                for ll in lines:
                    fh.write(f"  {ll.line:5d} [{ll.kind:13s}] {'    ' * ll.level}{ll.exact}\n")
                fh.write("\n")
    wcsv("capability_names.csv", cap_rows)
    wcsv("consumers.csv", consumer_rows)
    with open(os.path.join(OUT, "workarounds.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(workarounds, fh, indent=1)
    with open(os.path.join(OUT, "summary.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(summary, fh, indent=1)


if __name__ == "__main__":
    main()
