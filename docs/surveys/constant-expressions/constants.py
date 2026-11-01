"""How first-party GDScript in the corpus uses `const`. Lexical (token-level) survey.

Usage: python constants.py CORPUS_DIR EXTENSION_API_JSON
    CORPUS_DIR: fetched with ../cross-branch-duplication/gdcorpus.py
    EXTENSION_API_JSON: thirdparty/godot-cpp/gdextension/extension_api-4-7.json

Per `const` declaration: where it is declared, its type annotation, the form of its initializer and what
computing it takes (no computation, scalar computation, or computation on engine types, with float math
functions and Color from a string apart); uses of constants that preload scripts; accesses from other files;
constants in positions GDScript requires to be constant (annotation arguments, enum values, match patterns,
default parameter values); writes to constant containers.
"""
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cross-branch-duplication"))
import gdcorpus as G  # noqa: E402

if len(sys.argv) != 3:
    sys.exit(__doc__)
CORPUS, API_PATH = sys.argv[1], sys.argv[2]
api = json.load(open(API_PATH, encoding="utf-8"))
MATH = {f["name"]: f["return_type"] for f in api["utility_functions"] if f["category"] == "math"}
BUILTIN = {c["name"] for c in api["builtin_classes"]} - {"Nil"}
PRIMITIVE = {"bool", "int", "float", "String", "StringName", "NodePath"}
CTOR_TYPES = BUILTIN - PRIMITIVE
UTILITY = {f["name"] for f in api["utility_functions"]} | {
    "Color8", "char", "ord", "range", "len", "str", "convert", "type_exists", "load", "inst_to_dict",
    "dict_to_inst", "get_stack", "print_debug", "is_instance_of"} | PRIMITIVE
ENGINE_GLOBAL = {c["name"] for c in api["global_constants"]} | {v["name"] for e in api["global_enums"] for v in e["values"]}
ENGINE_CLASSES = {c["name"] for c in api["classes"]} | {s["name"] for s in api["singletons"]}
ENGINE_CLASS_CONSTS = ({k["name"] for c in api["classes"] for k in c.get("constants", [])}
                       | {v["name"] for c in api["classes"] for e in c.get("enums", []) for v in e["values"]})
ENGINE_CLASS_ENUMS = {e["name"] for c in api["classes"] for e in c.get("enums", [])}
KW_CONST = {"PI", "TAU", "INF", "NAN"}
LIT_KW = {"true", "false", "null"}
OPS_KW = {"and", "or", "not", "in", "is", "as", "if", "else"}
MUTATORS = {"append", "append_array", "push_back", "push_front", "pop_back", "pop_front", "pop_at", "insert",
            "erase", "clear", "remove_at", "sort", "sort_custom", "shuffle", "reverse", "fill", "resize",
            "merge", "assign", "set"}


def ov(t):
    return t.v if t.k == "OP" else None


def close_of(ts, i):
    d = 0
    for j in range(i, len(ts)):
        v = ov(ts[j])
        if v and v in "([{":
            d += 1
        elif v and v in ")]}":
            d -= 1
            if d == 0:
                return j
    return -1


def split_top(ts, sep=","):
    out, cur, d = [], [], 0
    for t in ts:
        v = ov(t)
        if v and v in "([{":
            d += 1
        elif v and v in ")]}":
            d -= 1
        if d == 0 and v == sep:
            out.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        out.append(cur)
    return out


def logical_lines(toks):
    """Lists of tokens per logical line (newlines inside brackets joined)."""
    lines, cur, d = [], [], 0
    for t in toks:
        if t.k == "NL":
            if d <= 0 and cur:
                lines.append(cur)
                cur = []
                d = 0
            continue
        v = ov(t)
        if v and v in "([{":
            d += 1
        elif v and v in ")]}":
            d -= 1
        cur.append(t)
    if cur:
        lines.append(cur)
    return lines


def skip_annotations(ln):
    i = 0
    while i < len(ln) and ln[i].k == "AN":
        if i + 1 < len(ln) and ov(ln[i + 1]) == "(" and ln[i + 1].line == ln[i].line:
            c = close_of(ln, i + 1)
            i = c + 1 if c > 0 else i + 1
        else:
            i += 1
    return i


def is_upper(n):
    return bool(re.fullmatch(r"_?[A-Z][A-Z0-9_]*", n))


def is_literal(e):
    if len(e) == 1:
        t = e[0]
        if t.k == "NUM":
            return "float" if ("." in t.v or ("e" in t.v.lower() and not t.v.lower().startswith("0x"))) else "int"
        if t.k == "STR" and t.pre in ("", "r"):
            return "string"
        if t.k == "ID" and t.v in ("true", "false"):
            return "bool"
        if t.k == "ID" and t.v == "null":
            return "null"
    if len(e) == 2 and ov(e[0]) in ("-", "+") and e[1].k == "NUM":
        return is_literal(e[1:])
    return None


def calls_in(e):
    out = []
    for i in range(len(e) - 1):
        if e[i].k == "ID" and ov(e[i + 1]) == "(":
            if i > 0 and ov(e[i - 1]) == ".":
                recv = e[i - 2] if i > 1 else None
                if recv is not None and recv.k == "ID" and (i < 3 or ov(e[i - 3]) != ".") and recv.v[:1].isupper():
                    out.append(("static", recv.v + "." + e[i].v))
                else:
                    out.append(("method", "." + e[i].v))
            elif e[i].v == "preload":
                out.append(("preload", "preload"))
            elif e[i].v in CTOR_TYPES:
                out.append(("ctor", e[i].v))
            elif e[i].v in UTILITY:
                out.append(("util", e[i].v))
            else:
                out.append(("other", e[i].v))
        elif e[i].k == "ID" and e[i].v in ("Array", "Dictionary") and ov(e[i + 1]) == "[":
            c = close_of(e, i + 1)
            if 0 < c < len(e) - 1 and ov(e[c + 1]) == "(":
                out.append(("ctor", e[i].v + "[]"))
    return out


def container_kind(inner, is_dict):
    if not inner:
        return "empty"
    names = []
    for i, t in enumerate(inner):
        if t.k != "ID" or t.v in LIT_KW or t.v in OPS_KW:
            continue
        if i > 0 and ov(inner[i - 1]) == ".":
            continue
        if is_dict and i + 1 < len(inner) and ov(inner[i + 1]) == "=" and not (i + 2 < len(inner) and ov(inner[i + 2]) == "="):
            continue  # Lua-style key
        names.append(t.v)
    calls = calls_in(inner)
    if any(k == "preload" for k, _ in calls):
        return "includes preload"
    if calls:
        return "includes constructor" if all(k == "ctor" for k, _ in calls) else "includes other calls"
    if names:
        return "includes names"
    return "all literals"


def preload_kind(p):
    p = p.lower()
    if p.endswith(".gd"):
        return ".gd"
    if p.endswith(".tscn") or p.endswith(".scn"):
        return "scene"
    return "other: " + (os.path.splitext(p)[1] or p[:6])


def classify(e):
    """(category, subcategory) of a const initializer."""
    if not e:
        return ("other", "empty")
    lit = is_literal(e)
    if lit:
        return ("1 literal", lit)
    if len(e) == 1 and e[0].k == "STR" and e[0].pre in ("&", "^"):
        return ("2 StringName/NodePath", "&" if e[0].pre == "&" else "^")
    c0 = close_of(e, 1) if len(e) > 1 and ov(e[1]) == "(" else -1
    if e[0].v == "preload" and c0 == len(e) - 1 and len(e) > 2 and e[2].k == "STR":
        return ("3 preload", preload_kind(e[2].v))
    if ov(e[0]) == "[" and close_of(e, 0) == len(e) - 1:
        return ("4 array", container_kind(e[1:-1], False))
    if ov(e[0]) == "{" and close_of(e, 0) == len(e) - 1:
        return ("5 dict", container_kind(e[1:-1], True))
    if e[0].k == "ID" and e[0].v in CTOR_TYPES and c0 == len(e) - 1:
        return ("6 constructor", e[0].v)
    if e[0].k == "ID" and e[0].v in ("Array", "Dictionary") and len(e) > 1 and ov(e[1]) == "[":
        c = close_of(e, 1)
        if 0 < c < len(e) - 1 and ov(e[c + 1]) == "(" and close_of(e, c + 1) == len(e) - 1:
            return ("6 constructor", e[0].v + "[T]")
    if len(e) == 3 and e[0].k == "ID" and e[0].v in BUILTIN and ov(e[1]) == "." and e[2].k == "ID":
        return ("7 builtin constant", e[0].v + ".X")
    calls = calls_in(e)
    if not calls:
        ids = [t for i, t in enumerate(e) if t.k == "ID" and t.v not in LIT_KW and t.v not in OPS_KW]
        if len(e) == 1 and e[0].k == "ID":
            return ("8 name/expr", "bare name (PI/TAU/INF)" if e[0].v in KW_CONST else "bare name")
        if all((i % 2 == 0 and t.k == "ID") or (i % 2 == 1 and ov(t) == ".") for i, t in enumerate(e)):
            return ("8 name/expr", "dotted name")
        return ("8 name/expr", "expression with names" if ids else "expression of literals only")
    if e[0].k == "ID" and c0 == len(e) - 1 and e[0].v not in CTOR_TYPES and e[0].v != "preload":
        return ("9 utility call", e[0].v if e[0].v in UTILITY else "non-utility: " + e[0].v)
    if (len(e) > 4 and e[0].k == "ID" and ov(e[1]) == "." and e[2].k == "ID" and ov(e[3]) == "("
            and close_of(e, 3) == len(e) - 1):
        return ("10 static method", e[0].v + "." + e[2].v)
    kinds = {k for k, _ in calls}
    if kinds == {"ctor"}:
        return ("6 constructor", "in expression")
    if kinds == {"util"}:
        return ("9 utility call", "in expression: " + "/".join(sorted({n for _, n in calls})))
    if kinds == {"static"}:
        return ("10 static method", "in expression")
    return ("11 other", "+".join(sorted(kinds)))


OPERATOR_CHARS = set("+-*/%<>&|^~!=")
OPERATOR_WORDS = {"and", "or", "not", "in", "is", "as", "if", "else"}


def computation(e, engine_consts):
    """What computing the initializer e takes: (kind, details).

    engine_consts: names of constants of the same file whose initializers make engine-type values.
    """
    strings = [(t.v, e[i + 2].v) for i, t in enumerate(e)
               if t.k == "ID" and t.v in CTOR_TYPES and i + 2 < len(e) and ov(e[i + 1]) == "(" and e[i + 2].k == "STR"]
    if strings:
        if any(type_name != "Color" for type_name, _ in strings):
            return "other constructor from a string", ""
        hex_only = all(re.fullmatch(r"#?([0-9a-fA-F]{3,4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})", s) for _, s in strings)
        return "Color from a hex string" if hex_only else "Color from a color name", ""
    ops = []
    for i, t in enumerate(e):
        v = ov(t)
        if v and v in OPERATOR_CHARS:
            prev = ov(e[i - 1]) if i else "("
            if v == "-" and prev in ("(", ",", "[", "{", ":") and i + 1 < len(e) and e[i + 1].k == "NUM":
                continue  # A negative literal.
            ops.append(v)
        elif t.k == "ID" and t.v in OPERATOR_WORDS:
            ops.append(t.v)
    math = [t.v for i, t in enumerate(e) if t.k == "ID" and t.v in MATH and i + 1 < len(e) and ov(e[i + 1]) == "("]
    conversions = [t.v for i, t in enumerate(e) if t.k == "ID" and t.v in ("int", "float") and i + 1 < len(e) and ov(e[i + 1]) == "("]
    if not ops and not math and not conversions:
        return "no computation", ""
    if "%" in ops and any(t.k == "STR" for t in e):
        return "string formatting", ""
    if any(w in ops for w in ("in", "is", "as")):
        return "in, is or as", ""
    engine = any((t.k == "ID" and t.v in CTOR_TYPES and i + 1 < len(e) and ov(e[i + 1]) in ("(", "."))
                 or (t.k == "ID" and t.v in engine_consts and not (i and ov(e[i - 1]) == "."))
                 for i, t in enumerate(e))
    if engine and ops:
        return "operators on engine types", ""
    if any(MATH[m] not in ("int", "bool") for m in math):
        return "math functions returning float", "/".join(sorted(set(math)))
    return "scalar computation", "/".join(sorted(set(math + conversions)))


# ----------------------------------------------------------------------------------------------

class FileData:
    pass


def scan_structure(src):
    """Per file: const decls, enums, funcs, annotations, match patterns (lexical)."""
    toks = G.tokenize(src)
    flat = [t for t in toks if t.k != "NL"]
    lines = logical_lines(toks)
    fd = FileData()
    fd.flat = flat
    fd.consts = []  # dict(name, scope, ann, init, line, cat, sub)
    fd.enum_names, fd.enum_members, fd.unnamed_members = set(), set(), set()
    fd.enum_values = []  # (line, member, expr tokens)
    fd.defaults = []  # (line, expr tokens)
    fd.patterns = []  # (line, alt tokens)
    fd.annotations = []  # (line, name, arg tokens)
    stack = []  # (kind, indent)
    mstack = []  # [match_indent, pattern_indent]
    for ln in lines:
        indent = ln[0].col
        while stack and indent <= stack[-1][1]:
            stack.pop()
        while mstack and indent <= mstack[-1][0]:
            mstack.pop()
        if mstack:
            m = mstack[-1]
            if m[1] is None:
                m[1] = indent
            if indent == m[1]:
                pat = []
                for t in ln:
                    if ov(t) == ":" and _depth(pat) == 0:
                        break
                    if t.k == "ID" and t.v == "when" and _depth(pat) == 0:
                        break
                    pat.append(t)
                for alt in split_top(pat):
                    fd.patterns.append((ln[0].line, alt))
        # annotations anywhere on the line
        for i, t in enumerate(ln):
            if t.k == "AN" and i + 1 < len(ln) and ov(ln[i + 1]) == "(" and ln[i + 1].line == t.line and ln[i + 1].col == t.col + len(t.v):
                c = close_of(ln, i + 1)
                if c > 0:
                    fd.annotations.append((t.line, t.v, ln[i + 2:c]))
        k = skip_annotations(ln)
        if k >= len(ln):
            continue
        head = ln[k]
        if head.k == "ID" and head.v == "static" and k + 1 < len(ln):
            k += 1
            head = ln[k]
        if head.k != "ID":
            continue
        if head.v == "match":
            mstack.append([indent, None])
        elif head.v == "func":
            if k + 2 < len(ln) and ln[k + 1].k == "ID" and ov(ln[k + 2]) == "(":
                c = close_of(ln, k + 2)
                for p in split_top(ln[k + 3:c]):
                    eq = [j for j, t in enumerate(p) if ov(t) == "=" and _depth(p[:j]) == 0]
                    if eq:
                        fd.defaults.append((head.line, p[eq[0] + 1:]))
            stack.append(("func", indent))
        elif head.v == "class" and k + 1 < len(ln) and ln[k + 1].k == "ID":
            stack.append(("class", indent))
        elif head.v == "enum":
            j = k + 1
            named = None
            if j < len(ln) and ln[j].k == "ID":
                named = ln[j].v
                fd.enum_names.add(named)
                j += 1
            if j < len(ln) and ov(ln[j]) == "{":
                c = close_of(ln, j)
                for mem in split_top(ln[j + 1:c]):
                    if not mem or mem[0].k != "ID":
                        continue
                    fd.enum_members.add(mem[0].v)
                    if named is None:
                        fd.unnamed_members.add(mem[0].v)
                    if len(mem) > 2 and ov(mem[1]) == "=":
                        fd.enum_values.append((mem[0].line, mem[0].v, mem[2:]))
        elif head.v == "const" and k + 1 < len(ln) and ln[k + 1].k == "ID":
            in_func = any(s[0] == "func" for s in stack)
            in_class = any(s[0] == "class" for s in stack)
            scope = "local" if in_func else ("inner class" if in_class else "class")
            rest = ln[k + 2:]
            ann = "none"
            init = rest
            if rest and ov(rest[0]) == ":":
                if len(rest) > 1 and ov(rest[1]) == "=":
                    ann, init = ":=", rest[2:]
                else:
                    eq = [j for j, t in enumerate(rest) if ov(t) == "=" and _depth(rest[:j]) == 0]
                    ann = "typed"
                    init = rest[eq[0] + 1:] if eq else []
            elif rest and ov(rest[0]) == "=":
                init = rest[1:]
            while init and ov(init[-1]) == ";":
                init = init[:-1]
            cat, sub = classify(init)
            fd.consts.append(dict(name=ln[k + 1].v, scope=scope, ann=ann, init=init, line=head.line, cat=cat, sub=sub,
                                  tok=ln[k + 1]))
    return fd


def _depth(ts):
    d = 0
    for t in ts:
        v = ov(t)
        if v and v in "([{":
            d += 1
        elif v and v in ")]}":
            d -= 1
    return d


# ----------------------------------------------------------------------------------------------

def _in_typed_container(ts, i):
    j, d = i - 1, 0
    while j >= 0:
        v = ov(ts[j])
        if v == "]":
            d += 1
        elif v == "[":
            if d == 0:
                return j > 0 and ts[j - 1].k == "ID" and ts[j - 1].v in ("Array", "Dictionary")
            d -= 1
        elif v in ("(", ")", "{", "}", "=") or ts[j].line != ts[i].line:
            return False
        j -= 1
    return False


def _count_subscripts(ts, a, b):
    n, j = 0, a
    while j < b and ov(ts[j]) == "[":
        n += 1
        j = close_of(ts, j) + 1
    return n


KEYS = ("all", "games")


def new_counter():
    return {k: collections.Counter() for k in KEYS}


decl = new_counter()       # (dimension, value) -> decl count
dfiles = {k: collections.defaultdict(set) for k in KEYS}  # (dimension, value) -> files
samples = collections.defaultdict(list)  # (dimension, value) -> [(repo, loc, text)]
other_samples = []
computed = []  # (kind, location, text) of initializers that compute more than scalars
totals = {k: collections.Counter() for k in KEYS}
q4 = new_counter()
q4_decls = {k: collections.defaultdict(set) for k in KEYS}
q5 = new_counter()
q5_files = {k: collections.defaultdict(set) for k in KEYS}
q5_pairs = {k: collections.defaultdict(set) for k in KEYS}
q6 = new_counter()
q6_files = {k: collections.defaultdict(set) for k in KEYS}
q6_samples = collections.defaultdict(list)
q6_ann = {k: collections.Counter() for k in KEYS}
q7 = []
q7_dup = new_counter()
q7_res = new_counter()
q7_res_samples = collections.defaultdict(list)
q5_repo = collections.Counter()
q5_class_consts = new_counter()


def bump(c, f, key_dims, keys, fid):
    for k in keys:
        for kd in key_dims:
            c[k][kd] += 1
            if f is not None:
                f[k][kd].add(fid)


def add_sample(dim, repo, loc, text, n=2):
    lst = samples[dim]
    if len(lst) < n and all(r != repo for r, _, _ in lst):
        lst.append((repo, loc, text))


def name_chain(ts, i):
    chain = [ts[i].v]
    j = i + 1
    while j + 1 < len(ts) and ov(ts[j]) == "." and ts[j + 1].k == "ID":
        chain.append(ts[j + 1].v)
        j += 2
    return chain, j


for name, _, _, group in G.REPOS:
    repo = G.Repo(CORPUS, name, group)
    keys = KEYS if group == "game" else ("all",)
    for f in repo.files:
        if f.endswith((".tscn", ".tres")):
            try:
                m = re.search(r'uid="(uid://[^"]+)"', repo.read(f)[:400])
            except OSError:
                m = None
            if m:
                repo.uid.setdefault(m.group(1), f)
    infos = G.script_infos(repo)
    files = {}
    for f in repo.files:
        if f.endswith(".gd") and repo.first_party(f):
            src = repo.read(f)
            fd = scan_structure(src)
            for c in fd.consts:
                c["file"] = f
                if c["cat"] == "3 preload" and c["init"][2].v.startswith("uid://"):
                    c["init"][2].v = repo.uid.get(c["init"][2].v, c["init"][2].v)
                    c["sub"] = preload_kind(c["init"][2].v)
            fd.src_lines = src.splitlines()
            fd.proj = repo.project_of(f).root
            files[f] = fd
    # class_name map, class-level consts per file (with inheritance)
    cname = collections.defaultdict(dict)
    for f in files:
        cn = infos.get(f, {}).get("class_name")
        if cn:
            cname[files[f].proj][cn] = f
    autoload = collections.defaultdict(dict)
    for root, proj in repo.projects.items():
        pg = (root + "/" if root else "") + "project.godot"
        if pg not in repo.fileset:
            continue
        sect = None
        for raw in repo.read(pg).splitlines():
            s = raw.strip()
            if s.startswith("["):
                sect = s
                continue
            m = re.match(r'(\w+)="\*?(res://[^"]+)"', s)
            if sect == "[autoload]" and m:
                rel = proj.to_rel(m.group(2))
                if rel and rel.endswith(".tscn") and rel in repo.fileset:
                    try:
                        sf = G.SceneFile(rel, repo.read(rel))
                        sc = sf.nodes[0].props.get("script", "") if sf.nodes else ""
                        mm = G.EXT_REF_RE.search(sc)
                        if mm:
                            e = sf.ext.get(mm.group(1))
                            rel = proj.to_rel(e[1]) if e and e[1] else None
                            if e and (rel is None or rel not in repo.fileset) and e[2] in repo.uid:
                                rel = repo.uid[e[2]]
                    except Exception:  # noqa
                        rel = None
                if rel in files:
                    autoload[root][m.group(1)] = rel
    own_consts = {f: {c["name"]: c for c in fd.consts if c["scope"] == "class"} for f, fd in files.items()}

    def consts_with_bases(f):
        out, cur, seen = {}, f, set()
        while cur and cur not in seen and cur in files:
            seen.add(cur)
            for n, c in own_consts[cur].items():
                out.setdefault(n, c)
            cur = infos.get(cur, {}).get("base_script")
        return out

    all_consts = {f: consts_with_bases(f) for f in files}
    repo_const_names = {c["name"] for fd in files.values() for c in fd.consts}
    inner_const_names = {c["name"] for fd in files.values() for c in fd.consts if c["scope"] == "inner class"}
    repo_enum_names = set().union(*(fd.enum_names for fd in files.values())) if files else set()
    repo_enum_members = set().union(*(fd.enum_members for fd in files.values())) if files else set()

    def preload_alias(f, c):
        if c["cat"] == "3 preload" and c["sub"] == ".gd":
            p = c["init"][2].v
            rel = p if p in files else repo.projects[files[f].proj].to_rel(p, f)
            return rel if rel in files else None
        return None

    def resolve(f, chain):
        fd = files[f]
        local_names = {c["name"] for c in fd.consts} | set(all_consts[f])
        if len(chain) == 1:
            n = chain[0]
            if n in local_names:
                return "const"
            if n in fd.unnamed_members:
                return "enum"
            if n in KW_CONST:
                return "builtin"
            if n in ENGINE_GLOBAL or n in ENGINE_CLASS_CONSTS:
                return "engine"
            if n in repo_enum_members:
                return "enum"
            if n in repo_const_names:
                return "const"
            return "other"
        head = chain[0]
        if head in fd.enum_names or (len(chain) >= 3 and chain[-2] in repo_enum_names) or head in repo_enum_names:
            return "enum"
        if head in BUILTIN:
            return "builtin"
        if (head in ENGINE_CLASSES or head in ENGINE_CLASS_ENUMS) and head not in cname[fd.proj]:
            return "engine"
        if chain[-1] in repo_const_names:
            return "const"
        if chain[-1] in repo_enum_members:
            return "enum"
        return "other"

    def expr_refs(f, e):
        out = []
        i = 0
        while i < len(e):
            t = e[i]
            if t.k == "ID" and t.v not in LIT_KW and t.v not in OPS_KW and not (i and ov(e[i - 1]) == "."):
                chain, j = name_chain(e, i)
                if j < len(e) and ov(e[j]) == "(":
                    i = j
                    continue
                out.append((".".join(chain), resolve(f, chain)))
                i = j
                continue
            i += 1
        return out

    for f, fd in sorted(files.items()):
        fid = name + "/" + f
        for k in keys:
            totals[k]["files"] += 1
        if fd.consts:
            for k in keys:
                totals[k]["files with const"] += 1
        engine_consts = {c["name"] for c in fd.consts if c["cat"] in ("6 constructor", "7 builtin constant")}
        for c in fd.consts:
            loc = f"{name}/{f}:{c['line']}"
            text = fd.src_lines[c["line"] - 1].strip()[:110]
            for k in keys:
                totals[k]["decls"] += 1
            kind, details = computation(c["init"], engine_consts)
            if kind not in ("no computation", "scalar computation"):
                computed.append((kind, loc, text))
            dims = [("scope", c["scope"]), ("ann:" + c["scope"].replace("inner class", "class"), c["ann"]),
                    ("ann", c["ann"]), ("cat", c["cat"]), ("sub", c["cat"] + " | " + c["sub"]), ("repo", name),
                    ("computation", kind)]
            if details:
                dims.append(("functions", details))
            if c["cat"] in ("8 name/expr", "4 array", "5 dict", "6 constructor"):
                for r in sorted({r for _, r in expr_refs(f, c["init"])}):
                    dims.append(("refs", c["cat"] + " -> " + r))
            dims.append(("ann by cat", c["cat"] + " | " + c["ann"]))
            bump(decl, dfiles, dims, keys, fid)
            add_sample(("cat", c["cat"]), name, loc, text)
            add_sample(("scope", c["scope"]), name, loc, text, 3)
            add_sample(("sub", c["cat"] + " | " + c["sub"]), name, loc, text, 1)
            if c["cat"] in ("11 other",) or c["sub"].startswith("non-utility"):
                other_samples.append((loc, text))

        # Q4: uses of preloaded .gd constants in the same file
        ts = fd.flat
        for c in fd.consts:
            if not (c["cat"] == "3 preload" and c["sub"] == ".gd"):
                continue
            X = c["name"]
            used = collections.Counter()
            for i, t in enumerate(ts):
                if t is c["tok"] or t.k != "ID" or t.v != X:
                    continue
                p = ts[i - 1] if i else None
                pp = ts[i - 2] if i > 1 else None
                nx = ts[i + 1] if i + 1 < len(ts) else None
                nx2 = ts[i + 2] if i + 2 < len(ts) else None
                if p is not None and ov(p) == ".":
                    continue
                if p is not None and ov(p) == ":" and pp is not None and (pp.k == "ID" or ov(pp) == ")"):
                    kind = "type"
                elif p is not None and ov(p) == ">" and pp is not None and ov(pp) == "-":
                    kind = "type"
                elif p is not None and p.k == "ID" and p.v in ("as", "is", "not", "extends"):
                    kind = "type"
                elif p is not None and ov(p) in ("[", ",") and _in_typed_container(ts, i):
                    kind = "type"
                elif nx is not None and ov(nx) == "." and nx2 is not None and nx2.v == "new":
                    kind = "new"
                elif nx is not None and ov(nx) == "." and nx2 is not None and nx2.k == "ID":
                    nx3 = ts[i + 3] if i + 3 < len(ts) else None
                    kind = "member call X.f()" if nx3 is not None and ov(nx3) == "(" else "member X.NAME"
                else:
                    kind = "other value use"
                used[kind] += 1
            for kind, n in used.items():
                for k in keys:
                    q4[k][kind] += n
                    q4_decls[k][kind].add(fid + ":" + X)
            for k in keys:
                q4[k]["decls"] += 1
                if not used:
                    q4_decls[k]["unused in file"].add(fid + ":" + X)

        # Q5: ClassName.CONST / Alias.CONST / instance.CONST
        aliases = {}
        for c in fd.consts:
            tgt = preload_alias(f, c)
            if tgt:
                aliases[c["name"]] = tgt
        cmap = cname[fd.proj]
        for i, t in enumerate(ts):
            if t.k != "ID" or i + 2 >= len(ts) or ov(ts[i + 1]) != "." or ts[i + 2].k != "ID":
                continue
            if i and ov(ts[i - 1]) == ".":
                continue
            C = ts[i + 2].v
            target = cmap.get(t.v) or aliases.get(t.v) or autoload[fd.proj].get(t.v)
            if target and C in all_consts.get(target, {}):
                via = "ClassName" if t.v in cmap else ("preload alias" if t.v in aliases else "autoload")
                where = "same file" if target == f else "other file"
                kd = f"{via} ({where})"
                for k in keys:
                    q5[k][kd] += 1
                    q5_files[k][kd].add(fid)
                    q5_pairs[k][kd].add((name, all_consts[target][C]["file"], C))
                if kd == "ClassName (other file)":
                    q5_repo[name] += 1
            elif target and name_chain(ts, i)[0][-1] in inner_const_names and len(name_chain(ts, i)[0]) >= 3:
                for k in keys:
                    q5[k]["Name.Inner(.Inner).CONST"] += 1
                    q5_files[k]["Name.Inner(.Inner).CONST"].add(fid)
            elif t.v == "self" and C in all_consts[f]:
                for k in keys:
                    q5[k]["self.CONST"] += 1
                    q5_files[k]["self.CONST"].add(fid)
            elif (t.v[:1].islower() and t.v not in aliases and t.v not in autoload[fd.proj] and is_upper(C) and C in repo_const_names
                  and C not in ENGINE_GLOBAL and t.v not in ("self",)):
                for k in keys:
                    q5[k]["instance.CONST (rough)"] += 1
                    q5_files[k]["instance.CONST (rough)"].add(fid)
                    q5_pairs[k]["instance.CONST (rough)"].add((t.v, C))

        # Q6
        def q6_add(kind, line, text):
            for k in keys:
                q6[k][kind] += 1
                q6_files[k][kind].add(fid)
            lst = q6_samples[kind]
            if len(lst) < 3 and all(not s.startswith(name + "/") for s in lst):
                lst.append(f"{name}/{f}:{line}  {text}")

        for line, an, args in fd.annotations:
            refs = expr_refs(f, args)
            if refs:
                kinds = {r for _, r in refs}
                q6_add("annotation args: any name", line, fd.src_lines[line - 1].strip()[:100])
                if "const" in kinds:
                    q6_add("annotation args: user const", line, fd.src_lines[line - 1].strip()[:100])
                    for k in keys:
                        q6_ann[k][an] += 1
                if "enum" in kinds:
                    q6_add("annotation args: user enum", line, fd.src_lines[line - 1].strip()[:100])
                if "engine" in kinds:
                    q6_add("annotation args: engine constant", line, fd.src_lines[line - 1].strip()[:100])
            for k in keys:
                q6[k]["annotations with args"] += 1
        for line, alt in fd.patterns:
            if not alt:
                continue
            for k in keys:
                q6[k]["match pattern alternatives"] += 1
            if all(t.k == "ID" or ov(t) == "." for t in alt) and alt[0].k == "ID" and alt[0].v not in LIT_KW and alt[0].v != "_":
                chain, j = name_chain(alt, 0)
                if j == len(alt):
                    r = resolve(f, chain)
                    shape = "bare" if len(chain) == 1 else "dotted"
                    q6_add(f"match pattern: {shape} name -> {r}", line, " ".join(chain))
        for line, e in fd.defaults:
            for k in keys:
                q6[k]["default parameter values"] += 1
            refs = expr_refs(f, e)
            kinds = {r for _, r in refs}
            for r in ("const", "enum", "engine", "builtin"):
                if r in kinds:
                    q6_add(f"default value refers to {r}", line, fd.src_lines[line - 1].strip()[:100])
        for line, mem, e in fd.enum_values:
            for k in keys:
                q6[k]["enum members with explicit value"] += 1
            refs = expr_refs(f, e)
            kinds = {r for _, r in refs}
            if not refs:
                q6_add("enum value: literal" if is_literal(e) else "enum value: literal expression", line, mem + " = " + " ".join(t.v for t in e))
            for r in ("const", "enum", "engine", "builtin", "other"):
                if r in kinds:
                    q6_add(f"enum value refers to {r}", line, mem + " = " + " ".join(str(t.v) for t in e))

        # Q7: mutation of const containers (own file, and ClassName.CONST from other files)
        def container(c):
            return c["cat"] in ("4 array", "5 dict") or (c["cat"] == "6 constructor" and (c["sub"].startswith("Packed") or c["sub"].startswith("Array") or c["sub"].startswith("Dictionary")))

        own_cont = {c["name"] for c in fd.consts if container(c)} | {n for n, c in all_consts[f].items() if container(c)}
        for i, t in enumerate(ts):
            if t.k != "ID":
                continue
            start = None
            if t.v in own_cont and not (i and ov(ts[i - 1]) == "."):
                start = i + 1
                label = t.v
            elif (i + 2 < len(ts) and ov(ts[i + 1]) == "." and ts[i + 2].k == "ID" and not (i and ov(ts[i - 1]) == ".")):
                target = cmap.get(t.v) or aliases.get(t.v) or autoload[fd.proj].get(t.v)
                if target and ts[i + 2].v in all_consts.get(target, {}) and container(all_consts[target][ts[i + 2].v]):
                    start = i + 3
                    label = t.v + "." + ts[i + 2].v
            if start is None:
                continue
            j, nested = start, False
            while j < len(ts) and ov(ts[j]) == "[":
                c = close_of(ts, j)
                if c < 0:
                    break
                j = c + 1
                nested = j < len(ts) and ov(ts[j]) in ("[", ".")
                if j < len(ts) and ov(ts[j]) == "[":
                    continue
                break
            hit = None
            if j < len(ts):
                if ov(ts[j]) == "=" and j > start and not (j + 1 < len(ts) and ov(ts[j + 1]) == "="):
                    hit = "index assignment" + (" (nested)" if j - start > 0 and _count_subscripts(ts, start, j) > 1 else "")
                elif ov(ts[j]) in ("+", "-", "*", "/") and j > start and j + 1 < len(ts) and ov(ts[j + 1]) == "=":
                    hit = "compound index assignment"
                elif ov(ts[j]) == "." and j + 2 < len(ts) and ts[j + 1].v in MUTATORS and ov(ts[j + 2]) == "(":
                    hit = "." + ts[j + 1].v + ("() on element" if j > start else "()")
                elif ov(ts[j]) == "." and j + 1 < len(ts) and ts[j + 1].v == "duplicate" and j == start:
                    for k in keys:
                        q7_dup[k]["duplicate()"] += 1
            if hit:
                q7.append((group, f"{name}/{f}:{t.line}", label, hit, fd.src_lines[t.line - 1].strip()[:100]))

        # Q7b: preloaded resources / scripts held in consts: property writes and method calls
        pre = {c["name"]: c["sub"] for c in fd.consts if c["cat"] == "3 preload"}
        for i, t in enumerate(ts):
            if t.k != "ID" or t.v not in pre or (i and ov(ts[i - 1]) == "."):
                continue
            if i and (ov(ts[i - 1]) in (":", ">") or ts[i - 1].v in ("as", "is", "extends")):
                continue  # X.Inner used as a type
            if not (i + 2 < len(ts) and ov(ts[i + 1]) == "." and ts[i + 2].k == "ID"):
                continue
            kindp = "script" if pre[t.v] == ".gd" else ("scene" if pre[t.v] == "scene" else "resource")
            n3 = ts[i + 3] if i + 3 < len(ts) else None
            n4 = ts[i + 4] if i + 4 < len(ts) else None
            if n3 is not None and ov(n3) == "=" and not (n4 is not None and ov(n4) == "="):
                kd = f"{kindp}: X.prop = ..."
            elif n3 is not None and ov(n3) in ("+", "-", "*", "/") and n4 is not None and ov(n4) == "=":
                kd = f"{kindp}: X.prop op= ..."
            elif n3 is not None and ov(n3) == "(":
                m = ts[i + 2].v
                kd = f"{kindp}: X.{m}()" if m in ("new", "instantiate", "duplicate") else f"{kindp}: X.other_method()"
            else:
                continue
            for k in keys:
                q7_res[k][kd] += 1
            if len(q7_res_samples[kd]) < 3 and "=" in kd:
                q7_res_samples[kd].append(f"{name}/{f}:{t.line}  {fd.src_lines[t.line - 1].strip()[:90]}")

    for f, fd in files.items():
        if infos.get(f, {}).get("class_name"):
            for c in fd.consts:
                if c["scope"] == "class":
                    for k in keys:
                        q5_class_consts[k]["class-level consts in class_name scripts"] += 1


# ----------------------------------------------------------------------------------------------

def table(title, dim, order=None):
    print(f"\n## {title}")
    vals = sorted({kd[1] for k in KEYS for kd in decl[k] if kd[0] == dim}, key=lambda v: order.index(v) if order and v in order else v)
    print(f"{'':58s} {'all decl':>9s} {'files':>6s} {'game decl':>10s} {'files':>6s}")
    for v in vals:
        a, g = decl["all"][(dim, v)], decl["games"][(dim, v)]
        print(f"{v[:58]:58s} {a:9d} {len(dfiles['all'][(dim, v)]):6d} {g:10d} {len(dfiles['games'][(dim, v)]):6d}")


print("# Totals")
for k in KEYS:
    print(k, dict(totals[k]))
table("Scope", "scope")
table("Annotation (all)", "ann")
table("Annotation (class-level incl. inner)", "ann:class")
table("Annotation (local)", "ann:local")
table("Initializer category", "cat")
table("Initializer subcategory", "sub")
table("Names referenced by name/expr, array, dict, constructor initializers", "refs")
table("Annotation by category", "ann by cat")
table("Computation the initializer takes", "computation")
table("Math functions and conversions called", "functions")
print("\n## Initializers that compute more than scalars")
for kind, loc, text in sorted(computed):
    print(f"  {kind:32s} {loc}  {text}")
table("Per repository", "repo")
print("\n## Samples per category")
for (dim, v), lst in sorted(samples.items()):
    if dim in ("cat", "scope"):
        print(f"  {v}")
        for r, loc, text in lst:
            print(f"      {loc}  {text}")
print("\n## Sample per subcategory")
for (dim, v), lst in sorted(samples.items()):
    if dim == "sub":
        for r, loc, text in lst:
            print(f"  {v[:50]:50s} {loc}  {text}")
print("\n## Other / non-utility samples")
for loc, text in other_samples:
    print(f"  {loc}  {text}")

print("\n## Q4 preloaded .gd constants: uses in the declaring file")
for kind in ("decls", "type", "new", "member call X.f()", "member X.NAME", "other value use"):
    print(f"  {kind:20s} all uses {q4['all'][kind]:5d} decls {len(q4_decls['all'][kind]):4d}   games uses {q4['games'][kind]:5d} decls {len(q4_decls['games'][kind]):4d}")
print(f"  unused in file: all {len(q4_decls['all']['unused in file'])} games {len(q4_decls['games']['unused in file'])}")

print("\n## Q5 qualified const access")
for kd in sorted(set(q5["all"])):
    print(f"  {kd:32s} all {q5['all'][kd]:5d} files {len(q5_files['all'][kd]):4d} distinct {len(q5_pairs['all'][kd]):4d}   "
          f"games {q5['games'][kd]:5d} files {len(q5_files['games'][kd]):4d} distinct {len(q5_pairs['games'][kd]):4d}")
inst = sorted(q5_pairs["all"]["instance.CONST (rough)"])[:30]
print("  instance samples:", inst)
for k in KEYS:
    print(f"  {k}: class-level consts in class_name scripts: {q5_class_consts[k]['class-level consts in class_name scripts']}, "
          f"distinct accessed as ClassName.CONST from another file: {len(q5_pairs[k]['ClassName (other file)'])}")
print("  per repo ClassName.CONST (other file):", q5_repo.most_common())

print("\n## Q6 constant positions")
for kd in sorted(set(q6["all"])):
    print(f"  {kd:45s} all {q6['all'][kd]:5d} files {len(q6_files['all'][kd]):4d}   games {q6['games'][kd]:5d} files {len(q6_files['games'][kd]):4d}")
print("  annotations with user const args:", q6_ann["all"].most_common(), "| games:", q6_ann["games"].most_common())
for kd, lst in sorted(q6_samples.items()):
    print(f"  {kd}")
    for s in lst:
        print(f"      {s}")

print("\n## Q7 const container mutation")
for row in q7:
    print("  ", row)
print("  CONST.duplicate():", dict(q7_dup["all"]), dict(q7_dup["games"]))
print("\n## Q7b preloaded values held in consts: writes and calls (same file)")
for kd in sorted(set(q7_res["all"])):
    print(f"  {kd:40s} all {q7_res['all'][kd]:5d}   games {q7_res['games'][kd]:5d}")
for kd, lst in sorted(q7_res_samples.items()):
    print(f"  {kd}")
    for s in lst:
        print(f"      {s}")
